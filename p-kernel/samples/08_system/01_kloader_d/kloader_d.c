/*
 * kloader_d.c — p-kernel カーネルローダーデーモン
 *
 * ユーザー空間 ELF32 デーモン。/etc/init.rc から自動起動される。
 * heal ガードが常に生存を監視する。
 *
 * 役割:
 *   1. KLOAD_BEACON 受信 (port 7383)
 *        → bare kloader ノード発見 → /PKNL.BIN を読んで自動プッシュ
 *   2. KLOAD_CHUNK 受信 (port 7382)
 *        → 自ノードへのカーネル更新受信 → /KL.BIN 書き込み → sys_reboot()
 *
 * ネットワーク: SYS_UDP_BIND / SYS_UDP_RECV / SYS_UDP_SEND (direct UDP)
 * ファイル IO : SYS_OPEN / SYS_READ / SYS_WRITE / SYS_CLOSE
 * 再起動      : SYS_REBOOT (ACPI リセット, ring-0 thin shim)
 */

#include "plibc.h"

/* ------------------------------------------------------------------ */
/* KLOAD プロトコル定数 (kl_net.h と同一)                             */
/* ------------------------------------------------------------------ */

#define KLOAD_MAGIC      0x44414F4CU   /* "LOAD" LE */
#define KLOAD_VERSION    1
#define KLOAD_START      0x01
#define KLOAD_CHUNK      0x02
#define KLOAD_BEACON     0x03
#define KLOAD_PORT       7382
#define KLOAD_PORT_BCN   7383
#define KLOAD_CHUNK_SIZE 1024

/* ------------------------------------------------------------------ */
/* カーネルバイナリ受信バッファ (最大 8 MB)                           */
/* ------------------------------------------------------------------ */

#define KLOAD_BUF_MAX (8 * 1024 * 1024)

static unsigned char kload_buf[KLOAD_BUF_MAX];
static unsigned int  kload_total   = 0;
static unsigned int  kload_written = 0;
static int           kload_active  = 0;

/* ------------------------------------------------------------------ */
/* パケット構造体                                                      */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    unsigned int   magic;
    unsigned char  version;
    unsigned char  type;
    unsigned char  src_node;
    unsigned char  _pad;
    unsigned int   total_size;
    unsigned int   chunk_idx;
    unsigned short chunk_len;
    unsigned char  data[KLOAD_CHUNK_SIZE];
} KLOAD_PKT;

typedef struct __attribute__((packed)) {
    unsigned int  magic;
    unsigned char version;
    unsigned char type;
    unsigned char node_id;
    unsigned char _pad;
    unsigned int  src_ip;   /* ホストバイトオーダー */
} KL_BEACON_PKT;

/* ------------------------------------------------------------------ */
/* ユーティリティ                                                      */
/* ------------------------------------------------------------------ */

static void d_puts(const char *s)    { plib_puts(s); }
static void d_putdec(unsigned int v) { plib_putu(v); }

static void d_putip(unsigned int ip)
{
    plib_putu(ip & 0xFF);         d_puts(".");
    plib_putu((ip >> 8) & 0xFF);  d_puts(".");
    plib_putu((ip >> 16) & 0xFF); d_puts(".");
    plib_putu((ip >> 24) & 0xFF);
}

static void d_memcpy(void *dst, const void *src, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned int i = 0; i < n; i++) d[i] = s[i];
}

static void d_memset(void *dst, unsigned char v, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;
    for (unsigned int i = 0; i < n; i++) d[i] = v;
}

/* ------------------------------------------------------------------ */
/* /KL.BIN 書き込み                                                   */
/* ------------------------------------------------------------------ */

static int write_kl_bin(void)
{
    sys_unlink("/KL.BIN");

    int fd = sys_open("/KL.BIN", O_WRONLY | O_CREAT);
    if (fd < 0) {
        d_puts("[kloader_d] ERROR: cannot create /KL.BIN\r\n");
        return -1;
    }

    unsigned int remain = kload_written;
    unsigned int offset = 0;
    while (remain > 0) {
        unsigned int chunk = (remain > 512) ? 512 : remain;
        int r = sys_write(fd, kload_buf + offset, (int)chunk);
        if (r <= 0) { sys_close(fd); return -1; }
        offset += (unsigned int)r;
        remain -= (unsigned int)r;
    }
    sys_close(fd);
    return (int)kload_written;
}

/* ------------------------------------------------------------------ */
/* /PKNL.BIN → KLOAD チャンク送信 (bare kloader ノードへ)            */
/* ------------------------------------------------------------------ */

static unsigned char push_buf[sizeof(KLOAD_PKT)];

static void push_kernel_to(unsigned int target_ip)
{
    /* /PKNL.BIN を開く */
    int fd = sys_open("/PKNL.BIN", O_RDONLY);
    if (fd < 0) {
        d_puts("[kloader_d] /PKNL.BIN not found — skip push\r\n");
        return;
    }

    /* ファイルサイズ取得 */
    sys_lseek(fd, 0, 2);    /* SEEK_END=2 */
    int fsize = sys_lseek(fd, 0, 2);
    sys_lseek(fd, 0, 0);    /* 先頭に戻す */
    if (fsize <= 0) { sys_close(fd); return; }

    d_puts("[kloader_d] pushing ");
    d_putdec((unsigned int)fsize);
    d_puts(" bytes to ");
    d_putip(target_ip);
    d_puts("\r\n");

    PK_SYS_UDP_SEND us;
    us.dst_ip   = target_ip;
    us.src_port = (unsigned short)KLOAD_PORT;
    us.dst_port = (unsigned short)KLOAD_PORT;
    us.buf      = push_buf;

    /* KLOAD_START */
    KLOAD_PKT *pkt = (KLOAD_PKT *)push_buf;
    d_memset(pkt, 0, sizeof(KLOAD_PKT));
    pkt->magic      = KLOAD_MAGIC;
    pkt->version    = KLOAD_VERSION;
    pkt->type       = KLOAD_START;
    pkt->total_size = (unsigned int)fsize;
    us.len          = 12;   /* ヘッダのみ */
    sys_udp_send(&us);

    tk_slp_tsk(200);    /* kloader 側の START 処理を待つ */

    /* チャンク送信 */
    unsigned int chunk_idx = 0;
    static unsigned char file_buf[KLOAD_CHUNK_SIZE];
    for (;;) {
        int n = sys_read(fd, file_buf, KLOAD_CHUNK_SIZE);
        if (n <= 0) break;

        d_memset(pkt, 0, sizeof(KLOAD_PKT));
        pkt->magic      = KLOAD_MAGIC;
        pkt->version    = KLOAD_VERSION;
        pkt->type       = KLOAD_CHUNK;
        pkt->total_size = (unsigned int)fsize;
        pkt->chunk_idx  = chunk_idx;
        pkt->chunk_len  = (unsigned short)n;
        d_memcpy(pkt->data, file_buf, (unsigned int)n);
        us.len  = (unsigned short)(12 + n);
        us.buf  = push_buf;
        sys_udp_send(&us);

        chunk_idx++;
        if (chunk_idx % 32 == 0) tk_slp_tsk(50);
    }

    sys_close(fd);
    d_puts("[kloader_d] push done  chunks=");
    d_putdec(chunk_idx);
    d_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* KLOAD_CHUNK 受信処理 (自ノードへの更新)                            */
/* ------------------------------------------------------------------ */

static void handle_kload(const unsigned char *data, unsigned short len)
{
    if (len < 12) return;
    const KLOAD_PKT *pkt = (const KLOAD_PKT *)data;
    if (pkt->magic   != KLOAD_MAGIC)   return;
    if (pkt->version != KLOAD_VERSION) return;

    if (pkt->type == KLOAD_START) {
        kload_total   = pkt->total_size;
        kload_written = 0;
        kload_active  = 1;
        d_memset(kload_buf, 0, kload_total < KLOAD_BUF_MAX
                               ? kload_total : KLOAD_BUF_MAX);
        d_puts("[kloader_d] KLOAD_START  total=");
        d_putdec(kload_total);
        d_puts(" bytes\r\n");
        return;
    }

    if (pkt->type == KLOAD_CHUNK && kload_active) {
        unsigned int offset = pkt->chunk_idx * KLOAD_CHUNK_SIZE;
        unsigned short clen = pkt->chunk_len;
        if (offset + clen > KLOAD_BUF_MAX) return;

        d_memcpy(kload_buf + offset, pkt->data, clen);
        if (offset + clen > kload_written)
            kload_written = offset + clen;

        if (pkt->chunk_idx % 64 == 0) {
            d_puts("[kloader_d] chunk=");
            d_putdec(pkt->chunk_idx);
            d_puts("  written=");
            d_putdec(kload_written);
            d_puts("\r\n");
        }

        if (kload_written >= kload_total) {
            kload_active = 0;
            d_puts("[kloader_d] received complete kernel  size=");
            d_putdec(kload_written);
            d_puts("\r\n");

            d_puts("[kloader_d] writing /KL.BIN ...\r\n");
            if (write_kl_bin() < 0) {
                d_puts("[kloader_d] write FAILED\r\n");
                return;
            }
            d_puts("[kloader_d] /KL.BIN written — rebooting\r\n");
            tk_slp_tsk(300);
            sys_reboot();   /* → ACPI reset → kloader → KL.BIN 起動 */
        }
    }
}

/* ------------------------------------------------------------------ */
/* BEACON キュー (発見した bare ノードの IP)                          */
/* ------------------------------------------------------------------ */

#define BEACON_QUEUE_MAX 8

static unsigned int beacon_queue[BEACON_QUEUE_MAX];
static int beacon_head = 0, beacon_tail = 0;

static void beacon_enqueue(unsigned int ip)
{
    /* 重複チェック */
    for (int i = beacon_head; i != beacon_tail;
         i = (i + 1) % BEACON_QUEUE_MAX) {
        if (beacon_queue[i] == ip) return;
    }
    beacon_queue[beacon_tail] = ip;
    beacon_tail = (beacon_tail + 1) % BEACON_QUEUE_MAX;
}

static unsigned int beacon_dequeue(void)
{
    if (beacon_head == beacon_tail) return 0;
    unsigned int ip = beacon_queue[beacon_head];
    beacon_head = (beacon_head + 1) % BEACON_QUEUE_MAX;
    return ip;
}

/* ------------------------------------------------------------------ */
/* _start                                                              */
/* ------------------------------------------------------------------ */

void _start(void)
{
    d_puts("[kloader_d] starting\r\n");

    /* ポートバインド */
    if (sys_udp_bind(KLOAD_PORT) < 0) {
        d_puts("[kloader_d] WARNING: port 7382 bind failed\r\n");
    }
    if (sys_udp_bind(KLOAD_PORT_BCN) < 0) {
        d_puts("[kloader_d] WARNING: port 7383 bind failed\r\n");
    }

    d_puts("[kloader_d] listening on ports 7382 (KLOAD) and 7383 (BEACON)\r\n");

    static unsigned char rx_buf[sizeof(KLOAD_PKT) + 64];
    PK_SYS_UDP_RECV ur;

    for (;;) {
        /* --- ビーコンキュー処理 --- */
        unsigned int push_ip;
        while ((push_ip = beacon_dequeue()) != 0)
            push_kernel_to(push_ip);

        /* --- 受信ポーリング (port 7383: BEACON) --- */
        ur.port       = KLOAD_PORT_BCN;
        ur.buf        = rx_buf;
        ur.buflen     = (unsigned short)sizeof(rx_buf);
        ur.timeout_ms = 50;
        ur.src_ip     = 0;
        ur.src_port   = 0;
        ur.data_len   = 0;
        if (sys_udp_recv(&ur) == 0 && ur.data_len >= (unsigned short)sizeof(KL_BEACON_PKT)) {
            const KL_BEACON_PKT *b = (const KL_BEACON_PKT *)rx_buf;
            if (b->magic == KLOAD_MAGIC && b->version == KLOAD_VERSION
                && b->type == KLOAD_BEACON) {
                unsigned int src = ur.src_ip ? ur.src_ip : b->src_ip;
                d_puts("[kloader_d] BEACON from node ");
                d_putdec(b->node_id);
                d_puts(" (");
                d_putip(src);
                d_puts(") — queuing push\r\n");
                beacon_enqueue(src);
            }
        }

        /* --- 受信ポーリング (port 7382: KLOAD チャンク) --- */
        ur.port       = KLOAD_PORT;
        ur.buf        = rx_buf;
        ur.buflen     = (unsigned short)sizeof(rx_buf);
        ur.timeout_ms = 50;
        ur.src_ip     = 0;
        ur.src_port   = 0;
        ur.data_len   = 0;
        if (sys_udp_recv(&ur) == 0 && ur.data_len > 0)
            handle_kload(rx_buf, ur.data_len);
    }
}
