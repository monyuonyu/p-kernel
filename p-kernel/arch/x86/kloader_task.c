/*
 * kloader_task.c — p-kernel 側 KLOAD 受信タスク
 *
 * pmesh ポート 7382 でカーネルバイナリを受信し、
 * /KL.BIN として VFS に書き込む。
 * 完了後 ACPI リセット → kloader が KL.BIN を起動。
 */

#include "kloader_task.h"
#include "pmesh.h"
#include "kernel.h"
#include "netstack.h"
#include "drpc.h"

IMPORT void sio_send_frame(const UB *buf, INT size);
IMPORT UB _kernel_end[];   /* linker.ld で定義 */

/* ------------------------------------------------------------------ */
/* シリアル出力ヘルパ                                                 */
/* ------------------------------------------------------------------ */

static void kl_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void kl_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { kl_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    kl_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* 受信バッファ                                                        */
/* ------------------------------------------------------------------ */

#define KLOAD_BUF_MAX   (8 * 1024 * 1024)  /* 最大 8 MB */

static UB  kload_buf[KLOAD_BUF_MAX];
static UW  kload_total   = 0;
static UW  kload_written = 0;
static UB  kload_active  = 0;   /* 受信中フラグ */

/* ------------------------------------------------------------------ */
/* ACPI リセット                                                       */
/* ------------------------------------------------------------------ */

static void acpi_reset(void)
{
    /* QEMU ACPI リセットレジスタ経由でフルリセット */
    __asm__ volatile(
        "movw $0xCF9, %%dx\n\t"
        "movb $0x06, %%al\n\t"
        "outb %%al, %%dx\n\t"
        :: : "eax", "edx"
    );
    /* 返らない */
    for (;;);
}

/* ------------------------------------------------------------------ */
/* VFS 書き込みユーティリティ                                         */
/* ------------------------------------------------------------------ */

IMPORT INT  vfs_create(const char *path);
IMPORT INT  vfs_open(const char *path);
IMPORT INT  vfs_write(INT fd, const void *buf, UW len);
IMPORT void vfs_close(INT fd);
IMPORT INT  vfs_unlink(const char *path);

static INT write_kl_bin(void)
{
    /* 既存ファイルを削除してから新規作成 */
    vfs_unlink("/KL.BIN");

    INT fd = vfs_create("/KL.BIN");
    if (fd < 0) return -1;

    UW remain = kload_written;
    UW offset  = 0;
    while (remain > 0) {
        UW chunk = (remain > 512) ? 512 : remain;
        INT r = vfs_write(fd, kload_buf + offset, chunk);
        if (r <= 0) { vfs_close(fd); return -1; }
        offset  += (UW)r;
        remain  -= (UW)r;
    }
    vfs_close(fd);
    return (INT)kload_written;
}

/* ------------------------------------------------------------------ */
/* pmesh 受信コールバック                                              */
/* ------------------------------------------------------------------ */

void kloader_rx(UB src_node, UH dst_port, const UB *data, UH len)
{
    (void)src_node; (void)dst_port;

    if (len < 12) return;   /* ヘッダ最低サイズ */

    const KLOAD_PKT *pkt = (const KLOAD_PKT *)data;
    if (pkt->magic   != KLOAD_MAGIC)   return;
    if (pkt->version != KLOAD_VERSION) return;

    if (pkt->type == KLOAD_START) {
        kload_total   = pkt->total_size;
        kload_written = 0;
        kload_active  = 1;
        /* バッファゼロクリア */
        for (UW i = 0; i < KLOAD_BUF_MAX; i++) kload_buf[i] = 0;

        kl_puts("[kloader_task] KLOAD_START  total=");
        kl_putdec(kload_total);
        kl_puts(" bytes\r\n");
        return;
    }

    if (pkt->type == KLOAD_CHUNK) {
        if (!kload_active) return;

        UW offset = (UW)pkt->chunk_idx * KLOAD_CHUNK_SIZE;
        UH clen   = pkt->chunk_len;

        if (offset + clen > KLOAD_BUF_MAX) {
            kl_puts("[kloader_task] buffer overflow — abort\r\n");
            kload_active = 0;
            return;
        }

        /* データをバッファへコピー */
        const UB *src = pkt->data;
        UB       *dst = kload_buf + offset;
        for (UH i = 0; i < clen; i++) dst[i] = src[i];

        if (offset + clen > kload_written)
            kload_written = offset + clen;

        /* 進捗表示 (32 チャンクごと) */
        if (pkt->chunk_idx % 32 == 0) {
            kl_puts("[kloader_task] chunk ");
            kl_putdec(pkt->chunk_idx);
            kl_puts("  written=");
            kl_putdec(kload_written);
            kl_puts("\r\n");
        }

        /* 転送完了判定 */
        if (kload_written >= kload_total) {
            kl_puts("[kloader_task] transfer complete  size=");
            kl_putdec(kload_written);
            kl_puts("\r\n");
            kload_active = 0;

            /* /KL.BIN へ書き込み */
            kl_puts("[kloader_task] writing /KL.BIN ...\r\n");
            if (write_kl_bin() < 0) {
                kl_puts("[kloader_task] write FAILED\r\n");
                return;
            }
            kl_puts("[kloader_task] /KL.BIN written — rebooting via ACPI reset\r\n");

            /* 少し待ってからリセット (シリアル出力が流れる時間) */
            tk_dly_tsk(500);
            acpi_reset();
        }
    }
}

/* ------------------------------------------------------------------ */
/* 自動プッシュ: IP 直接 UDP 送信版 (bare kloader ノード向け)        */
/* ベアノードは pmesh/dnode_table に登録されていないため直接 UDP 使用 */
/* ------------------------------------------------------------------ */

IMPORT INT udp_send(UW dst_ip, UH src_port, UH dst_port,
                    const UB *data, UH data_len);

static void kloader_push_ip(UW target_ip)
{
    const UB *bin_start = (const UB *)0x100000;
    UW        bin_size  = (UW)(_kernel_end) - 0x100000UL;

    kl_puts("[kloader_task] auto-push to ");
    /* IP 表示 */
    for (INT b = 0; b < 4; b++) {
        if (b) kl_puts(".");
        kl_putdec((target_ip >> (b * 8)) & 0xFF);
    }
    kl_puts("  size=");
    kl_putdec(bin_size);
    kl_puts("\r\n");

    static KLOAD_PKT pkt;
    for (UW i = 0; i < sizeof(pkt); i++) ((UB *)&pkt)[i] = 0;
    pkt.magic      = KLOAD_MAGIC;
    pkt.version    = KLOAD_VERSION;
    pkt.type       = KLOAD_START;
    pkt.src_node   = (UB)drpc_my_node;
    pkt.total_size = bin_size;
    udp_send(target_ip, KLOAD_PORT, KLOAD_PORT,
             (const UB *)&pkt, (UH)sizeof(pkt));

    tk_dly_tsk(200);

    UW chunk_idx = 0;
    UW offset    = 0;
    while (offset < bin_size) {
        UW remain = bin_size - offset;
        UH clen   = (remain >= KLOAD_CHUNK_SIZE)
                    ? (UH)KLOAD_CHUNK_SIZE : (UH)remain;

        for (UW i = 0; i < sizeof(pkt); i++) ((UB *)&pkt)[i] = 0;
        pkt.magic      = KLOAD_MAGIC;
        pkt.version    = KLOAD_VERSION;
        pkt.type       = KLOAD_CHUNK;
        pkt.src_node   = (UB)drpc_my_node;
        pkt.total_size = bin_size;
        pkt.chunk_idx  = chunk_idx;
        pkt.chunk_len  = clen;
        const UB *src = bin_start + offset;
        for (UH i = 0; i < clen; i++) pkt.data[i] = src[i];

        udp_send(target_ip, KLOAD_PORT, KLOAD_PORT,
                 (const UB *)&pkt, (UH)sizeof(pkt));

        offset += clen;
        chunk_idx++;
        if (chunk_idx % 32 == 0) tk_dly_tsk(50);
    }

    kl_puts("[kloader_task] auto-push done  chunks=");
    kl_putdec(chunk_idx);
    kl_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* KLOAD_BEACON 受信コールバック (UDP direct, port 7383)              */
/* ------------------------------------------------------------------ */

/* ビーコンキュー (自動プッシュ待ち IP) */
#define BEACON_QUEUE_MAX  4

static UW beacon_queue[BEACON_QUEUE_MAX];
static UB beacon_queue_head = 0;
static UB beacon_queue_tail = 0;

IMPORT INT udp_bind(UH port, void (*fn)(UW src_ip, UH src_port,
                                        const UB *data, UH len));

/* KLOAD_BEACON パケット構造 (kl_net.h と同一レイアウト) */
typedef struct __attribute__((packed)) {
    UW magic;
    UB version;
    UB type;
    UB node_id;
    UB _pad;
    UW src_ip;
} KL_BEACON_PKT;

void kloader_beacon_rx(UW src_ip, UH src_port,
                       const UB *data, UH len)
{
    (void)src_port;
    if (len < (UH)sizeof(KL_BEACON_PKT)) return;

    const KL_BEACON_PKT *b = (const KL_BEACON_PKT *)data;
    if (b->magic   != KLOAD_MAGIC)   return;
    if (b->version != KLOAD_VERSION) return;
    if (b->type    != KLOAD_BEACON)  return;

    /* src_ip が 0 のときはパケット内の IP を使う */
    UW push_ip = src_ip ? src_ip : b->src_ip;
    if (!push_ip) return;

    kl_puts("[kloader_task] BEACON from node ");
    kl_putdec(b->node_id);
    kl_puts("  -> queuing auto-push\r\n");

    /* キューへ追加 (重複チェック) */
    for (UB i = beacon_queue_head; i != beacon_queue_tail;
         i = (UB)((i + 1) % BEACON_QUEUE_MAX)) {
        if (beacon_queue[i] == push_ip) return;  /* 既にキュー済み */
    }
    beacon_queue[beacon_queue_tail] = push_ip;
    beacon_queue_tail = (UB)((beacon_queue_tail + 1) % BEACON_QUEUE_MAX);
}

/* ------------------------------------------------------------------ */
/* タスク: ビーコンキューを監視して自動プッシュ                       */
/* ------------------------------------------------------------------ */

void kloader_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    tk_dly_tsk(3000);   /* NIC/ARP 安定待ち */

    for (;;) {
        tk_dly_tsk(500);

        while (beacon_queue_head != beacon_queue_tail) {
            UW ip = beacon_queue[beacon_queue_head];
            beacon_queue_head = (UB)((beacon_queue_head + 1) % BEACON_QUEUE_MAX);
            kloader_push_ip(ip);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void kloader_task_init(void)
{
    kload_total   = 0;
    kload_written = 0;
    kload_active  = 0;
    beacon_queue_head = 0;
    beacon_queue_tail = 0;

    /* KLOAD_CHUNK/START 受信 (pmesh 経由 — 既存 p-kernel ノードから) */
    pmesh_bind(KLOAD_PORT, kloader_rx);

    /* KLOAD_BEACON 受信 (direct UDP — bare kloader ノードから) */
    udp_bind(KLOAD_PORT_BCN, kloader_beacon_rx);

    kl_puts("[kloader_task] KLOAD receiver ready  port=7382\r\n");
    kl_puts("[kloader_task] BEACON listener ready  port=7383\r\n");
}

/* ------------------------------------------------------------------ */
/* kpush — シェルから呼ぶ: 自分自身のカーネルをターゲットノードへ送信 */
/* ------------------------------------------------------------------ */

void kloader_push(UB target_node)
{
    if (target_node >= DNODE_MAX) {
        kl_puts("[kpush] invalid node\r\n");
        return;
    }

    const UB *bin_start = (const UB *)0x100000;
    UW        bin_size  = (UW)(_kernel_end) - 0x100000UL;

    kl_puts("[kpush] pushing to node ");
    kl_putdec(target_node);
    kl_puts("  size=");
    kl_putdec(bin_size);
    kl_puts("\r\n");

    /* KLOAD_START 送信 */
    static KLOAD_PKT pkt;
    for (UW i = 0; i < sizeof(pkt); i++) ((UB *)&pkt)[i] = 0;
    pkt.magic      = KLOAD_MAGIC;
    pkt.version    = KLOAD_VERSION;
    pkt.type       = KLOAD_START;
    pkt.src_node   = (UB)drpc_my_node;
    pkt.total_size = bin_size;
    pmesh_send(target_node, KLOAD_PORT, (const UB *)&pkt, (UH)sizeof(pkt));

    tk_dly_tsk(100);  /* 相手の START 処理を待つ */

    /* チャンク送信 */
    UW  chunk_idx = 0;
    UW  offset    = 0;

    while (offset < bin_size) {
        UW remain = bin_size - offset;
        UH clen   = (remain >= KLOAD_CHUNK_SIZE)
                    ? (UH)KLOAD_CHUNK_SIZE
                    : (UH)remain;

        for (UW i = 0; i < sizeof(pkt); i++) ((UB *)&pkt)[i] = 0;
        pkt.magic      = KLOAD_MAGIC;
        pkt.version    = KLOAD_VERSION;
        pkt.type       = KLOAD_CHUNK;
        pkt.src_node   = (UB)drpc_my_node;
        pkt.total_size = bin_size;
        pkt.chunk_idx  = chunk_idx;
        pkt.chunk_len  = clen;

        const UB *src = bin_start + offset;
        for (UH i = 0; i < clen; i++) pkt.data[i] = src[i];

        pmesh_send(target_node, KLOAD_PORT, (const UB *)&pkt, (UH)sizeof(pkt));

        offset    += clen;
        chunk_idx++;

        /* 32 チャンクごとに少し待機 (pmesh/UDP バッファ保護) */
        if (chunk_idx % 32 == 0) {
            tk_dly_tsk(50);
        }
    }

    kl_puts("[kpush] done  chunks=");
    kl_putdec(chunk_idx);
    kl_puts("\r\n");
}
