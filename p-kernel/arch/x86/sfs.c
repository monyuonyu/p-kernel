/*
 *  sfs.c (x86)
 *  Shared Folder Sync — /shared/ 以下のファイルをクラスタ全ノードで複製
 *
 *  独自 UDP プロトコル (port 7381) を使用。
 *  K-DDS の 128B 制限を回避し、最大 32KB のファイルを 512B チャンクで転送。
 */

#include "sfs.h"
#include "drpc.h"
#include "netstack.h"
#include "pmesh.h"
#include "vfs.h"
#include "kernel.h"
#include <tmonitor.h>

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void sf_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void sf_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { sf_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    sf_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* 文字列ユーティリティ                                                */
/* ------------------------------------------------------------------ */

static INT sf_strlen(const char *s) { INT n = 0; while (s[n]) n++; return n; }

static INT sf_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static void sf_strncpy(char *dst, const char *src, INT max)
{
    INT i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void sf_memcpy(void *dst, const void *src, INT n)
{
    const UB *s = (const UB *)src;
    UB       *d = (UB *)dst;
    for (INT i = 0; i < n; i++) d[i] = s[i];
}

static void sf_memset(void *dst, UB val, INT n)
{
    UB *d = (UB *)dst; for (INT i = 0; i < n; i++) d[i] = val;
}

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

SFS_STATS sfs_stats;

/* Tombstone テーブル — 削除されたファイルパスを保持 */
static char sfs_tomb[SFS_TOMB_MAX][SFS_PATH_MAX];
static UB   sfs_tomb_active[SFS_TOMB_MAX];

/* 受信バッファ (1 ファイルのみ同時受信) */
static struct {
    char  path[SFS_PATH_MAX];
    UW    total_size;
    UW    received;          /* 受信済みバイト数 */
    UW    next_chunk;        /* 期待する次のチャンク番号 */
    UB    active;
    UB    buf[SFS_MAX_FILE_SIZE];
} sfs_rx_state;

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void sfs_init(void)
{
    sf_memset(&sfs_stats,    0, (INT)sizeof(sfs_stats));
    sf_memset(&sfs_rx_state, 0, (INT)sizeof(sfs_rx_state));
    sf_memset(sfs_tomb,        0, (INT)sizeof(sfs_tomb));
    sf_memset(sfs_tomb_active, 0, (INT)sizeof(sfs_tomb_active));

    pmesh_bind(SFS_PORT, sfs_rx);

    sf_puts("[sfs] shared folder sync ready  root=");
    sf_puts(SFS_ROOT);
    sf_puts("  port=");
    sf_putdec(SFS_PORT);
    sf_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* パスチェック                                                        */
/* ------------------------------------------------------------------ */

BOOL sfs_is_shared(const char *path)
{
    /* "/shared" または "/shared/" で始まるか確認 */
    const char *prefix = SFS_ROOT;
    INT plen = sf_strlen(prefix);
    for (INT i = 0; i < plen; i++)
        if (path[i] != prefix[i]) return FALSE;
    /* 末尾が "/" か '\0' であればOK */
    return (path[plen] == '/' || path[plen] == '\0');
}

/* ------------------------------------------------------------------ */
/* Tombstone 管理                                                      */
/* ------------------------------------------------------------------ */

static void tomb_add(const char *path)
{
    /* 既存エントリを更新 */
    for (INT i = 0; i < SFS_TOMB_MAX; i++) {
        if (sfs_tomb_active[i] && sf_streq(sfs_tomb[i], path)) return;
    }
    /* 空きスロットへ追加 */
    for (INT i = 0; i < SFS_TOMB_MAX; i++) {
        if (!sfs_tomb_active[i]) {
            sf_strncpy(sfs_tomb[i], path, SFS_PATH_MAX);
            sfs_tomb_active[i] = 1;
            return;
        }
    }
    /* フル時は最初のスロットを上書き */
    sf_strncpy(sfs_tomb[0], path, SFS_PATH_MAX);
    sfs_tomb_active[0] = 1;
}

static INT tomb_contains(const char *path)
{
    for (INT i = 0; i < SFS_TOMB_MAX; i++)
        if (sfs_tomb_active[i] && sf_streq(sfs_tomb[i], path)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 単一ノードへパケット送信                                            */
/* ------------------------------------------------------------------ */

static void send_pkt_to(UB node_id, const SFS_PKT *pkt, UH len)
{
    pmesh_send(node_id, SFS_PORT, (const UB *)pkt, len);
}

/* ------------------------------------------------------------------ */
/* 全ノードへパケット送信 (自ノードを除く、pmesh で中継)              */
/* ------------------------------------------------------------------ */

static void broadcast_pkt(const SFS_PKT *pkt, UH len)
{
    if (drpc_my_node == 0xFF) return;   /* 非分散モード */
    for (INT n = 0; n < DNODE_MAX; n++) {
        if ((UB)n == drpc_my_node) continue;
        pmesh_send((UB)n, SFS_PORT, (const UB *)pkt, len);
    }
}

/* ------------------------------------------------------------------ */
/* sfs_push — ファイルを全 ALIVE ノードへ転送                         */
/* ------------------------------------------------------------------ */

void sfs_push(const char *path)
{
    if (!sfs_is_shared(path)) return;
    if (!vfs_ready) return;
    if (drpc_my_node == 0xFF) return;

    INT fd = vfs_open(path);
    if (fd < 0) return;

    UW fsize = vfs_fsize(fd);
    if (fsize == 0 || fsize > SFS_MAX_FILE_SIZE) {
        vfs_close(fd);
        return;
    }

    /* SFS_FILE_START パケット */
    SFS_PKT pkt;
    sf_memset(&pkt, 0, (INT)sizeof(pkt));
    pkt.magic      = SFS_MAGIC;
    pkt.version    = SFS_VERSION;
    pkt.type       = SFS_FILE_START;
    pkt.src_node   = drpc_my_node;
    sf_strncpy(pkt.path, path, SFS_PATH_MAX);
    pkt.total_size = fsize;
    pkt.chunk_idx  = 0;
    pkt.chunk_len  = 0;

    broadcast_pkt(&pkt, (UH)sizeof(pkt));

    /* チャンク送信 */
    UW idx = 0;
    for (;;) {
        INT n = vfs_read(fd, pkt.data, SFS_CHUNK_SIZE);
        if (n <= 0) break;

        pkt.type      = SFS_FILE_CHUNK;
        pkt.chunk_idx = idx;
        pkt.chunk_len = (UH)n;

        broadcast_pkt(&pkt, (UH)sizeof(pkt));

        sfs_stats.chunks_sent++;
        idx++;
    }
    vfs_close(fd);

    sfs_stats.files_sent++;
    sf_puts("[sfs] pushed \"");
    sf_puts(path);
    sf_puts("\"  chunks=");
    sf_putdec(idx);
    sf_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* sfs_delete — ファイル削除を全 ALIVE ノードへ伝播 (tombstone 付き)  */
/* ------------------------------------------------------------------ */

void sfs_delete(const char *path)
{
    if (!sfs_is_shared(path)) return;
    if (drpc_my_node == 0xFF) return;

    /* tombstone 登録 */
    tomb_add(path);

    /* DELETE パケット送信 */
    SFS_PKT pkt;
    sf_memset(&pkt, 0, (INT)sizeof(pkt));
    pkt.magic    = SFS_MAGIC;
    pkt.version  = SFS_VERSION;
    pkt.type     = SFS_FILE_DELETE;
    pkt.src_node = drpc_my_node;
    sf_strncpy(pkt.path, path, SFS_PATH_MAX);

    broadcast_pkt(&pkt, (UH)sizeof(pkt));

    sfs_stats.files_deleted++;
    sf_puts("[sfs] delete broadcast: \"");
    sf_puts(path);
    sf_puts("\"\r\n");
}

/* ------------------------------------------------------------------ */
/* sfs_boot_sync — 全 ALIVE ノードへ SYNC_REQ を送信                  */
/* ------------------------------------------------------------------ */

void sfs_boot_sync(void)
{
    if (drpc_my_node == 0xFF) return;

    SFS_PKT pkt;
    sf_memset(&pkt, 0, (INT)sizeof(pkt));
    pkt.magic    = SFS_MAGIC;
    pkt.version  = SFS_VERSION;
    pkt.type     = SFS_SYNC_REQ;
    pkt.src_node = drpc_my_node;

    broadcast_pkt(&pkt, (UH)sizeof(pkt));
    sf_puts("[sfs] boot sync request sent\r\n");
}

/* ------------------------------------------------------------------ */
/* sfs_rx — UDP 受信コールバック                                       */
/* ------------------------------------------------------------------ */

void sfs_rx(UB src_node, UH dst_port, const UB *data, UH len)
{
    (void)dst_port;

    if (len < (UH)(sizeof(SFS_PKT) - SFS_CHUNK_SIZE)) return;  /* ヘッダ最小長 */
    const SFS_PKT *pkt = (const SFS_PKT *)data;

    if (pkt->magic   != SFS_MAGIC)   return;
    if (pkt->version != SFS_VERSION) return;
    if (pkt->src_node == drpc_my_node) return;   /* 自分からのエコー防止 */

    /* パスの null 終端を確認 */
    INT plen = 0;
    while (plen < SFS_PATH_MAX && pkt->path[plen]) plen++;
    if (plen == 0 && pkt->type != SFS_SYNC_REQ) return;

    switch (pkt->type) {

    /* ---- ファイル転送開始 ----------------------------------------- */
    case SFS_FILE_START:
        if (!vfs_ready) break;
        if (tomb_contains(pkt->path)) {
            sf_puts("[sfs] START ignored (tombstone): ");
            sf_puts(pkt->path); sf_puts("\r\n");
            break;
        }
        if (pkt->total_size == 0 || pkt->total_size > SFS_MAX_FILE_SIZE) break;

        /* 進行中の受信があれば破棄 */
        sf_memset(&sfs_rx_state, 0, (INT)sizeof(sfs_rx_state));
        sf_strncpy(sfs_rx_state.path, pkt->path, SFS_PATH_MAX);
        sfs_rx_state.total_size  = pkt->total_size;
        sfs_rx_state.received    = 0;
        sfs_rx_state.next_chunk  = 0;
        sfs_rx_state.active      = 1;

        sf_puts("[sfs] START \"");
        sf_puts(pkt->path);
        sf_puts("\"  size=");
        sf_putdec(pkt->total_size);
        sf_puts("\r\n");
        break;

    /* ---- データチャンク ------------------------------------------- */
    case SFS_FILE_CHUNK:
        if (!vfs_ready) break;
        if (!sfs_rx_state.active) break;
        if (!sf_streq(sfs_rx_state.path, pkt->path)) break;
        if (pkt->chunk_idx != sfs_rx_state.next_chunk) break;  /* 順序外は破棄 */
        if (pkt->chunk_len == 0 || pkt->chunk_len > SFS_CHUNK_SIZE) break;
        if (sfs_rx_state.received + pkt->chunk_len > SFS_MAX_FILE_SIZE) break;

        sf_memcpy(sfs_rx_state.buf + sfs_rx_state.received,
                  pkt->data, pkt->chunk_len);
        sfs_rx_state.received   += pkt->chunk_len;
        sfs_rx_state.next_chunk++;
        sfs_stats.chunks_received++;

        /* 全チャンク受信完了チェック */
        if (sfs_rx_state.received >= sfs_rx_state.total_size) {
            /* /shared/ ディレクトリが存在しない場合は作成 */
            vfs_mkdir(SFS_ROOT);

            /* ファイルを書き込む */
            vfs_unlink(sfs_rx_state.path);   /* 既存ファイルを削除 */
            INT fd = vfs_create(sfs_rx_state.path);
            if (fd >= 0) {
                vfs_write(fd, sfs_rx_state.buf, sfs_rx_state.received);
                vfs_close(fd);
                sfs_stats.files_received++;
                sf_puts("[sfs] received \"");
                sf_puts(sfs_rx_state.path);
                sf_puts("\"  ");
                sf_putdec(sfs_rx_state.received);
                sf_puts(" bytes\r\n");
            } else {
                sfs_stats.errors++;
                sf_puts("[sfs] write failed: ");
                sf_puts(sfs_rx_state.path);
                sf_puts("\r\n");
            }
            sfs_rx_state.active = 0;
        }
        break;

    /* ---- ファイル削除通知 ----------------------------------------- */
    case SFS_FILE_DELETE:
        if (!vfs_ready) break;
        tomb_add(pkt->path);
        if (vfs_unlink(pkt->path) == 0) {
            sfs_stats.files_deleted++;
            sf_puts("[sfs] deleted (remote): \"");
            sf_puts(pkt->path);
            sf_puts("\"\r\n");
        }
        break;

    /* ---- 同期要求 — 持っているファイルを全部送る ------------------- */
    case SFS_SYNC_REQ:
        if (!vfs_ready) break;
        {
            VFS_DIRENT entries[32];
            INT n = vfs_readdir(SFS_ROOT, entries, 32);
            if (n < 0) break;

            for (INT e = 0; e < n; e++) {
                if (entries[e].is_dir) continue;

                /* "/shared/" + name */
                char fpath[SFS_PATH_MAX];
                INT pi = 0;
                const char *root = SFS_ROOT;
                while (*root && pi < SFS_PATH_MAX - 2) fpath[pi++] = *root++;
                fpath[pi++] = '/';
                const char *nm = entries[e].name;
                while (*nm && pi < SFS_PATH_MAX - 1) fpath[pi++] = *nm++;
                fpath[pi] = '\0';

                if (tomb_contains(fpath)) continue;

                /* SYNC_REQ を送ってきたノードのみへ送信 */
                INT fd = vfs_open(fpath);
                if (fd < 0) continue;
                UW fsize = vfs_fsize(fd);
                if (fsize == 0 || fsize > SFS_MAX_FILE_SIZE) { vfs_close(fd); continue; }

                SFS_PKT spkt;
                sf_memset(&spkt, 0, (INT)sizeof(spkt));
                spkt.magic      = SFS_MAGIC;
                spkt.version    = SFS_VERSION;
                spkt.type       = SFS_FILE_START;
                spkt.src_node   = drpc_my_node;
                sf_strncpy(spkt.path, fpath, SFS_PATH_MAX);
                spkt.total_size = fsize;

                send_pkt_to(src_node, &spkt, (UH)sizeof(spkt));

                UW cidx = 0;
                for (;;) {
                    INT nr = vfs_read(fd, spkt.data, SFS_CHUNK_SIZE);
                    if (nr <= 0) break;
                    spkt.type      = SFS_FILE_CHUNK;
                    spkt.chunk_idx = cidx;
                    spkt.chunk_len = (UH)nr;
                    send_pkt_to(src_node, &spkt, (UH)sizeof(spkt));
                    sfs_stats.chunks_sent++;
                    cidx++;
                }
                vfs_close(fd);
                sfs_stats.files_sent++;
            }
            sf_puts("[sfs] sync response sent to node ");
            sf_putdec(src_node);
            sf_puts("\r\n");
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* sfs_stat — 統計と tombstone 一覧を表示                             */
/* ------------------------------------------------------------------ */

void sfs_stat(void)
{
    sf_puts("[sfs] stats:\r\n");
    sf_puts("  files_sent=");     sf_putdec(sfs_stats.files_sent);
    sf_puts("  files_received="); sf_putdec(sfs_stats.files_received);
    sf_puts("  files_deleted=");  sf_putdec(sfs_stats.files_deleted);
    sf_puts("\r\n");
    sf_puts("  chunks_sent=");    sf_putdec(sfs_stats.chunks_sent);
    sf_puts("  chunks_received=");sf_putdec(sfs_stats.chunks_received);
    sf_puts("  errors=");         sf_putdec(sfs_stats.errors);
    sf_puts("\r\n");

    sf_puts("[sfs] tombstones:\r\n");
    INT any = 0;
    for (INT i = 0; i < SFS_TOMB_MAX; i++) {
        if (!sfs_tomb_active[i]) continue;
        sf_puts("  "); sf_puts(sfs_tomb[i]); sf_puts("\r\n");
        any++;
    }
    if (!any) sf_puts("  (none)\r\n");
}

/* ------------------------------------------------------------------ */
/* sfs_list — /shared/ 以下のファイルを一覧表示                       */
/* ------------------------------------------------------------------ */

void sfs_list(void)
{
    if (!vfs_ready) {
        sf_puts("[sfs] disk not available\r\n");
        return;
    }

    VFS_DIRENT entries[32];
    INT n = vfs_readdir(SFS_ROOT, entries, 32);
    if (n < 0) {
        sf_puts("[sfs] " SFS_ROOT " not found (no files yet)\r\n");
        return;
    }

    sf_puts("[sfs] " SFS_ROOT ":\r\n");
    for (INT e = 0; e < n; e++) {
        if (entries[e].is_dir) continue;
        sf_puts("  "); sf_puts(entries[e].name);
        sf_puts("  ("); sf_putdec(entries[e].size); sf_puts(" B)\r\n");
    }
    if (n == 0) sf_puts("  (empty)\r\n");
}
