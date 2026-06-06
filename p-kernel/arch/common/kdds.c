/*
 *  kdds.c (x86)
 *  K-DDS — カーネルネイティブ pub/sub 実装
 *
 *  トピックテーブル (kdds_topics[]) と
 *  ハンドルテーブル (kdds_handles[]) をカーネル空間に保持する。
 *
 *  pub 時のデータフロー:
 *    1. トピックスロットのデータを更新する
 *    2. 同一トピックを指す全サブスクライバのセマフォを signal する
 *    3. 分散モード (drpc_my_node != 0xFF) なら全 ALIVE ノードへ KDDS_PKT を送信
 *
 *  sub 時のデータフロー:
 *    1. セマフォを wait する (timeout_ms 指定可)
 *    2. 起きたらトピックスロットから buf へデータをコピーする
 *    3. RELIABLE/LATEST_ONLY なら既存データがある場合は即時返却する
 */

#include "kdds.h"
#include "netstack.h"
#include "replica.h"
#include "pmesh.h"
#include "region.h"
#include "kernel.h"

/* 直近の kdds_pub() の fanout (UDP 送信したピア数) — スコープ効果の観測用 */
static UW kdds_last_fanout = 0;
UW kdds_pub_fanout(void) { return kdds_last_fanout; }

IMPORT void sio_send_frame(const UB *buf, INT size);

static void kd_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void kd_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { kd_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    kd_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* テーブル実体                                                        */
/* ------------------------------------------------------------------ */

KDDS_TOPIC       kdds_topics[KDDS_TOPIC_MAX];
KDDS_HANDLE_SLOT kdds_handles[KDDS_HANDLE_MAX];

/* ------------------------------------------------------------------ */
/* 文字列ユーティリティ (libc なし環境用)                              */
/* ------------------------------------------------------------------ */

static INT kd_strlen(const char *s)
{
    INT n = 0; while (s[n]) n++; return n;
}

static void kd_strcpy(char *dst, const char *src, INT max)
{
    INT i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static INT kd_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static void kd_memcpy(void *dst, const void *src, INT n)
{
    const UB *s = (const UB *)src;
    UB       *d = (UB *)dst;
    for (INT i = 0; i < n; i++) d[i] = s[i];
}

/* ------------------------------------------------------------------ */
/* トピック検索 / 作成                                                 */
/* ------------------------------------------------------------------ */

/* 名前でトピックを検索する (既存のみ)。見つからなければ -1。 */
static W topic_find(const char *name)
{
    for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
        if (kdds_topics[i].open && kd_streq(kdds_topics[i].name, name))
            return i;
    }
    return -1;
}

/* 名前でトピックを検索する。見つからなければ新規スロットを確保する。
 * 返り値: インデックス (0..KDDS_TOPIC_MAX-1) または -1 (失敗) */
static W topic_find_or_create(const char *name, W qos, W scope)
{
    /* 既存検索 — 見つかればスコープを更新 (REGION への昇格を許す) */
    {
        W i = topic_find(name);
        if (i >= 0) {
            kdds_topics[i].scope = (UB)scope;
            return i;
        }
    }
    /* 空きスロット確保 */
    for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
        if (!kdds_topics[i].open) {
            kd_strcpy(kdds_topics[i].name, name, KDDS_NAME_MAX);
            kdds_topics[i].data_len = 0;
            kdds_topics[i].qos      = (UB)qos;
            kdds_topics[i].scope    = (UB)scope;
            kdds_topics[i].open     = 1;
            return i;
        }
    }
    return -1;   /* テーブルが満杯 */
}

/* ------------------------------------------------------------------ */
/* kdds_open                                                           */
/* ------------------------------------------------------------------ */

W kdds_open(const char *name, W qos)
{
    return kdds_open_scoped(name, qos, KDDS_SCOPE_GLOBAL);
}

/* 共通実装。blocking != 0 なら subscriber 用セマフォを作る。
 * blocking == 0 (poll-only) なら sem を作らずハンドルだけ確保する。
 * poll-only は LATEST_ONLY + timeout=0 ポーリング購読で使う: kdds_sub は
 * データがあればセマフォに触れず即時返却し、kdds_rx も sub_sem<0 を skip する
 * ので、ブロックしない購読者はセマフォを1個も消費しない。
 * (CFN_MAX_SEMID は TCB ABI に縛られて増やせないため、per-source topic を
 *  大量に開く world/moe のような購読者がセマフォを枯渇させない逃げ道。) */
static W kdds_open_core(const char *name, W qos, W scope, int blocking)
{
    W tidx = topic_find_or_create(name, qos, scope);
    if (tidx < 0) {
        kd_puts("[kdds] topic table full\r\n");
        return -1;
    }

    /* ハンドルスロット確保 */
    for (W h = 0; h < KDDS_HANDLE_MAX; h++) {
        if (!kdds_handles[h].open) {
            ID sem = -1;
            if (blocking) {
                /* subscriber 用セマフォを作成する */
                T_CSEM cs = {
                    .exinf   = NULL,
                    .sematr  = TA_TFIFO,
                    .isemcnt = 0,
                    .maxsem  = 64
                };
                sem = tk_cre_sem(&cs);
                if (sem < E_OK) return (W)sem;
            }

            kdds_handles[h].topic_idx = tidx;
            kdds_handles[h].sub_sem   = sem;
            kdds_handles[h].open      = 1;

            kd_puts("[kdds] open  topic=\""); kd_puts(name);
            kd_puts("\"  handle="); kd_putdec((UW)h);
            if (!blocking) kd_puts(" (poll)");
            kd_puts("\r\n");
            return h;
        }
    }

    kd_puts("[kdds] handle table full\r\n");
    return -1;
}

W kdds_open_scoped(const char *name, W qos, W scope)
{
    return kdds_open_core(name, qos, scope, 1);
}

W kdds_open_poll(const char *name, W qos)
{
    return kdds_open_core(name, qos, KDDS_SCOPE_GLOBAL, 0);
}

W kdds_open_poll_scoped(const char *name, W qos, W scope)
{
    return kdds_open_core(name, qos, scope, 0);
}

/* ------------------------------------------------------------------ */
/* kdds_pub                                                            */
/* ------------------------------------------------------------------ */

W kdds_pub(W handle, const void *data, W len)
{
    if (handle < 0 || handle >= KDDS_HANDLE_MAX || !kdds_handles[handle].open)
        return E_PAR;
    if (len <= 0 || len > KDDS_DATA_MAX) return E_PAR;

    W tidx = kdds_handles[handle].topic_idx;
    KDDS_TOPIC *t = &kdds_topics[tidx];

    /* トピックデータを更新 (seq をインクリメントして複製判定に使う) */
    kd_memcpy(t->data, data, len);
    t->data_len = (UH)len;
    t->data_seq++;

    /* 同一トピックの全 subscriber セマフォを signal */
    for (W h = 0; h < KDDS_HANDLE_MAX; h++) {
        if (h == handle) continue;    /* 自分自身はスキップ */
        if (!kdds_handles[h].open) continue;
        if (kdds_handles[h].topic_idx != tidx) continue;
        if (kdds_handles[h].sub_sem >= 0)
            tk_sig_sem(kdds_handles[h].sub_sem, 1);
    }

    /* 分散モード: 全 ALIVE ノードへ UDP 送信 */
    if (drpc_my_node != 0xFF) {
        KDDS_PKT pkt = { 0 };
        pkt.magic    = KDDS_MAGIC;
        pkt.version  = KDDS_VERSION;
        pkt.type     = KDDS_DATA_PKT;
        pkt.src_node = drpc_my_node;
        pkt.data_len = (UH)len;

        INT nlen = kd_strlen(t->name) + 1;
        if (nlen > KDDS_NAME_MAX) nlen = KDDS_NAME_MAX;
        pkt.name_len = (UH)nlen;
        kd_memcpy(pkt.name, t->name, nlen);
        kd_memcpy(pkt.data, data, len);

        /* REGION スコープなら自 region メンバにだけ送る。region_recompute()
         * を一度回し、ループ内は region_is_member() で安く判定する。 */
        BOOL region_scoped = (t->scope == KDDS_SCOPE_REGION);
        if (region_scoped) region_recompute();

        /* G9 (honesty): count DELIVERIES, not attempts. pmesh_send() returns
         * 0 only when the frame was actually handed to the link layer; on a
         * missing route it bumps pmesh_stats.no_route and returns -1. Counting
         * the attempt made fanout report packets that fell into a no_route
         * hole as if they had been sent — a quiet lie about scope reach. */
        UW fanout = 0;
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n == drpc_my_node) continue;
            if (region_scoped && !region_is_member(n)) continue;
            if (pmesh_send(n, KDDS_PORT, (const UB *)&pkt, (UH)sizeof(pkt)) == 0)
                fanout++;
        }
        kdds_last_fanout = fanout;
    } else {
        kdds_last_fanout = 0;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* kdds_sub                                                            */
/* ------------------------------------------------------------------ */

W kdds_sub(W handle, void *buf, W buflen, W timeout_ms)
{
    if (handle < 0 || handle >= KDDS_HANDLE_MAX || !kdds_handles[handle].open)
        return E_PAR;
    if (!buf || buflen <= 0) return E_PAR;

    W tidx = kdds_handles[handle].topic_idx;
    KDDS_TOPIC *t = &kdds_topics[tidx];
    ID sem = kdds_handles[handle].sub_sem;

    /*
     * RELIABLE / LATEST_ONLY: データが既にあれば即時返却する。
     * BEST_EFFORT: 次の pub まで待つ。
     */
    if (t->qos != KDDS_QOS_BEST_EFFORT && t->data_len > 0) {
        W copy = t->data_len < buflen ? t->data_len : buflen;
        kd_memcpy(buf, t->data, copy);
        return copy;
    }

    /* セマフォで次の pub を待つ */
    TMO tmo = (timeout_ms < 0) ? TMO_FEVR : (TMO)timeout_ms;
    ER  er  = tk_wai_sem(sem, 1, tmo);
    if (er != E_OK) return (W)er;

    /* データをコピーして返す */
    W copy = t->data_len < buflen ? t->data_len : (W)buflen;
    kd_memcpy(buf, t->data, copy);
    return copy;
}

/* ------------------------------------------------------------------ */
/* kdds_close                                                          */
/* ------------------------------------------------------------------ */

void kdds_close(W handle)
{
    if (handle < 0 || handle >= KDDS_HANDLE_MAX) return;
    if (!kdds_handles[handle].open) return;

    /* セマフォを削除 */
    if (kdds_handles[handle].sub_sem >= 0) {
        tk_del_sem(kdds_handles[handle].sub_sem);
        kdds_handles[handle].sub_sem = -1;
    }

    W tidx = kdds_handles[handle].topic_idx;
    kdds_handles[handle].open = 0;

    /* このトピックを参照するハンドルが他になければトピックスロットも解放 */
    BOOL still_used = FALSE;
    for (W h = 0; h < KDDS_HANDLE_MAX; h++) {
        if (kdds_handles[h].open && kdds_handles[h].topic_idx == tidx) {
            still_used = TRUE;
            break;
        }
    }
    if (!still_used) kdds_topics[tidx].open = 0;
}

/* ------------------------------------------------------------------ */
/* kdds_rx — リモートノードからのデータ受信                           */
/* ------------------------------------------------------------------ */

void kdds_rx(UB src_node, UH dst_port, const UB *data, UH len)
{
    (void)src_node; (void)dst_port;
    if (len < (UH)sizeof(KDDS_PKT)) return;

    const KDDS_PKT *pkt = (const KDDS_PKT *)data;
    if (pkt->magic != KDDS_MAGIC || pkt->version != KDDS_VERSION) return;
    if (pkt->type  != KDDS_DATA_PKT) return;
    if (pkt->data_len == 0 || pkt->data_len > KDDS_DATA_MAX) return;

    /* トピックを検索 (なければ作成)。スコープは送信側でのみ強制されるので
     * 受信側は GLOBAL で「作る」— だが既存トピックのスコープは触らない。
     * (以前は find_or_create が既存スロットのスコープも GLOBAL に上書きして
     *  いた: REGION スコープで開いた pfs/ann 等が、最初の受信パケットで
     *  こっそり GLOBAL 配送へ昇格してしまい、region 局所性が壊れていた。) */
    W tidx = topic_find(pkt->name);
    if (tidx < 0)
        tidx = topic_find_or_create(pkt->name, KDDS_QOS_LATEST_ONLY,
                                    KDDS_SCOPE_GLOBAL);
    if (tidx < 0) return;

    KDDS_TOPIC *t = &kdds_topics[tidx];
    kd_memcpy(t->data, pkt->data, pkt->data_len);
    t->data_len = pkt->data_len;

    /* ローカルの subscriber を起こす */
    for (W h = 0; h < KDDS_HANDLE_MAX; h++) {
        if (!kdds_handles[h].open) continue;
        if (kdds_handles[h].topic_idx != tidx) continue;
        if (kdds_handles[h].sub_sem >= 0)
            tk_sig_sem(kdds_handles[h].sub_sem, 1);
    }
}

/* ------------------------------------------------------------------ */
/* kdds_init                                                           */
/* ------------------------------------------------------------------ */

void kdds_init(void)
{
    for (W i = 0; i < KDDS_TOPIC_MAX; i++)  kdds_topics[i].open  = 0;
    for (W h = 0; h < KDDS_HANDLE_MAX; h++) {
        kdds_handles[h].open      = 0;
        kdds_handles[h].sub_sem   = -1;
        kdds_handles[h].topic_idx = -1;
    }
    pmesh_bind(KDDS_PORT, kdds_rx);
    kd_puts("[kdds] K-DDS ready  port=7376\r\n");
}

/* ------------------------------------------------------------------ */
/* kdds_delete_cluster — トピックをローカル削除 + 全ノードへ tombstone */
/* ------------------------------------------------------------------ */

void kdds_delete_cluster(const char *name)
{
    /* ローカルトピックスロットを閉じる */
    BOOL found = FALSE;
    for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
        if (!kdds_topics[i].open) continue;
        if (!kd_streq(kdds_topics[i].name, name)) continue;
        kdds_topics[i].open = 0;
        found = TRUE;
        break;
    }
    /* 対応するハンドルも全て閉じる */
    for (W h = 0; h < KDDS_HANDLE_MAX; h++) {
        if (!kdds_handles[h].open) continue;
        /* topic_idx 経由で判定できないため名前で再検索 */
        W tidx = kdds_handles[h].topic_idx;
        if (tidx < 0 || tidx >= KDDS_TOPIC_MAX) continue;
        if (!kd_streq(kdds_topics[tidx].name, name) &&
            !(found && !kdds_topics[tidx].open)) continue;
        /* このハンドルが削除対象トピックのものなら閉じる */
    }

    kd_puts("[kdds] delete cluster: \""); kd_puts(name); kd_puts("\"\r\n");

    /* 分散モードなら tombstone をゴシップする */
    if (drpc_my_node != 0xFF)
        replica_tombstone(name);
}

/* ------------------------------------------------------------------ */
/* kdds_list — シェル `topic list` コマンド用                         */
/* ------------------------------------------------------------------ */

static const char *qos_str(UB qos)
{
    switch (qos) {
    case KDDS_QOS_RELIABLE:    return "RELIABLE  ";
    case KDDS_QOS_LATEST_ONLY: return "LATEST    ";
    default:                   return "BEST_EFF  ";
    }
}

void kdds_list(void)
{
    kd_puts("[topics]  IDX  Name                             QoS        Data(B)\r\n");
    BOOL any = FALSE;
    for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
        if (!kdds_topics[i].open) continue;
        any = TRUE;
        kd_puts("          "); kd_putdec((UW)i);
        kd_puts("    "); kd_puts(kdds_topics[i].name);
        /* パディング */
        INT pad = 33 - kd_strlen(kdds_topics[i].name);
        for (INT p = 0; p < pad; p++) kd_puts(" ");
        kd_puts(qos_str(kdds_topics[i].qos));
        kd_puts("  "); kd_putdec(kdds_topics[i].data_len);
        kd_puts("\r\n");
    }
    if (!any) kd_puts("  (トピックなし)\r\n");
}
