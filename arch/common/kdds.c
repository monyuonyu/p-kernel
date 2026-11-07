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
#include "pk_nosw.h"
#if defined(_TK_HOSTED_LIBC_)
#include "swim.h"      /* swim_wide_alive (N2 (ii)(c)) */
#endif

/* 直近の kdds_pub() の fanout (UDP 送信したピア数) — スコープ効果の観測用 */
static UW kdds_last_fanout = 0;
UW kdds_pub_fanout(void) { return kdds_last_fanout; }

/* --- 局所性 (locality) 計測カウンタ — wave-12 F隊 / G25 (§4 の数値検証) ----
 * survival-network.md §4 は「region 局所ルーティング = 遠方通信を減らす =
 * 光速とエネルギーへの答え」を掲げるが、その主張を一度も数で測っていない。
 * kdds_pub() の実配送 (delivery) を「同 region 宛 (近傍)」と「異 region 宛
 * (遠方)」に分けて累積する。harness が推論前後でスナップショットを取り差分を
 * 出すことで「1 推論あたり何メッセージ・何バイトが遠方を渡ったか」を測れる。
 * バイトは送出フレーム長 sizeof(KDDS_PKT) 固定 (UDP 上の実バイト)。 */
static UW kdds_tx_msgs       = 0;   /* 配送総数 (= 累積 fanout)           */
static UW kdds_tx_cross      = 0;   /* うち異 region (遠方) への配送数    */
static UW kdds_tx_bytes      = 0;   /* 配送総バイト                       */
static UW kdds_tx_bytes_cross = 0;  /* うち異 region (遠方) への配送バイト */
void kdds_locality_stats(UW *msgs, UW *cross, UW *bytes, UW *bytes_cross)
{
    if (msgs)        *msgs        = kdds_tx_msgs;
    if (cross)       *cross       = kdds_tx_cross;
    if (bytes)       *bytes       = kdds_tx_bytes;
    if (bytes_cross) *bytes_cross = kdds_tx_bytes_cross;
}

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
#ifdef _TK_HOSTED_LIBC_
/* ------------------------------------------------------------------ */
/* R-KDDSQ, stage 1 (kdds-queue.md, hosted only): kdds_sub_next reads a */
/* handle's unread messages oldest first. Before the topic's one slot  */
/* is overwritten while a next-reader of it has not read it, the slot  */
/* goes into a shared ring of KDDS_Q_N. kdds_sub is unchanged.         */
/* -DKDDS_NC_NO_QUEUE: nothing is kept (negative control).             */
/* ------------------------------------------------------------------ */
#define KDDS_Q_N 32
typedef struct { W tidx; UH seq; UH len; UB used; UB data[KDDS_DATA_MAX]; } KDDS_QSLOT;
static KDDS_QSLOT kq_ring[KDDS_Q_N];
static INT kq_w;                                /* next slot to write          */
static UB  kq_on[KDDS_HANDLE_MAX];              /* the handle reads with _next */
static UH  kq_seq[KDDS_HANDLE_MAX];             /* the last seq it read        */
static UW  kq_drop[KDDS_TOPIC_MAX];             /* kept messages lost unread   */
/* the queue's own count of the slot's messages (pub and rx both step it). Not
 * data_seq: kdds_rx does not step that one, and replica.c compares it to merge
 * (stepping it on rx would change which replica wins) */
static UH  kq_tseq[KDDS_TOPIC_MAX];

/* d = seq - last in 1..0x7FFF: after `last` (UH wraps) */
static BOOL kq_after(UH seq, UH last) { UH d = (UH)(seq - last); return d >= 1 && d <= 0x7FFF; }
/* is the message seq of topic tidx still unread by some next-reader? */
static BOOL kq_wanted(W tidx, UH seq)
{
    for (W h = 0; h < KDDS_HANDLE_MAX; h++)
        if (kdds_handles[h].open && kq_on[h] && kdds_handles[h].topic_idx == tidx && kq_after(seq, kq_seq[h]))
            return TRUE;
    return FALSE;
}
/* caller holds PK_NOSW: keep the slot's current message before it is overwritten */
static void kq_save(W tidx)
{
#ifndef KDDS_NC_NO_QUEUE
    KDDS_TOPIC *t = &kdds_topics[tidx];
    if (t->data_len > 0 && kq_wanted(tidx, kq_tseq[tidx])) {
        KDDS_QSLOT *q = &kq_ring[kq_w];
        if (q->used && kq_wanted(q->tidx, q->seq)) kq_drop[q->tidx]++;
        q->tidx = tidx; q->seq = kq_tseq[tidx]; q->len = t->data_len; q->used = 1;
        kd_memcpy(q->data, t->data, t->data_len);
        kq_w = (kq_w + 1) % KDDS_Q_N;
    }
#else
    (void)kq_wanted; (void)kq_w;
#endif
    kq_tseq[tidx]++;                            /* the message about to be written */
}
UW kdds_queue_dropped(W handle)
{
    if (handle < 0 || handle >= KDDS_HANDLE_MAX || !kdds_handles[handle].open) return 0;
    return kq_drop[kdds_handles[handle].topic_idx];
}
#define KQ_SAVE(tidx) kq_save(tidx)
#define KQ_OPEN(h)    (kq_on[(h)] = 0)
#else
#define KQ_SAVE(tidx) ((void)0)
#define KQ_OPEN(h)    ((void)0)
#endif

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
            kdds_handles[h].owner     = 0;   /* 既定: 所有者なし (カーネル所有)。
                                              * ring3 デーモンのハンドルは
                                              * SYS_TOPIC_OPEN が kdds_set_owner()
                                              * で所有タスクを刻む。 */
            kdds_handles[h].open      = 1;
            KQ_OPEN(h);

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

#if defined(_TK_HOSTED_LIBC_) && !defined(N2_NC_KDDS_NARROW_ONLY)
/* N2 (c)(1): the wide peers a broadcast goes to, out of SWIM's ALIVE list:
 * not myself, not a narrow id (the loop over 0..DNODE_MAX-1 has them), and
 * for a REGION topic only region members. Pure (member is region_is_member
 * in production) so `kdds wide` can hold the region rule: on localhost every
 * peer is inside the RTT line, so a live swarm cannot show it (audit-151).
 * out may be alive (k never runs behind m). -DN2_NC_KDW_NO_REGION: no region
 * rule for wide peers (falsifier, [kdds-wide] region). */
static INT kdds_wide_pick(const pk_node_t *alive, INT n, pk_node_t self, BOOL region_scoped,
                          BOOL (*member)(pk_node_t), pk_node_t *out)
{
    INT m = 0;
    for (INT k = 0; k < n; k++) {
        pk_node_t w = alive[k];
        if (w == self || w < DNODE_MAX) continue;
#ifndef N2_NC_KDW_NO_REGION
        if (region_scoped && !member(w)) continue;
#else
        (void)region_scoped; (void)member;
#endif
        out[m++] = w;
    }
    return m;
}
#endif

/* dst < 0: 今までどおり (scope の全員)。dst >= 0: 網へは dst の 1 台だけ (N1)。 */
static W kdds_pub_core(W handle, const void *data, W len, INT dst)
{
    if (handle < 0 || handle >= KDDS_HANDLE_MAX || !kdds_handles[handle].open)
        return E_PAR;
    if (len <= 0 || len > KDDS_DATA_MAX) return E_PAR;

    W tidx = kdds_handles[handle].topic_idx;
    KDDS_TOPIC *t = &kdds_topics[tidx];

    /* トピックデータを更新 (seq をインクリメントして複製判定に使う)
     * pk_nosw.h: the slot is shared; a task switch in the middle of the copy
     * would let another publisher or a reader see half of each */
    PK_NOSW_ENTER();
    KQ_SAVE(tidx);
    kd_memcpy(t->data, data, len);
    t->data_len = (UH)len;
    t->data_seq++;
    PK_NOSW_LEAVE();

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
        pkt.src_node = pk_node_v1(drpc_my_node);
        pkt.data_len = (UH)len;

        INT nlen = kd_strlen(t->name) + 1;
        if (nlen > KDDS_NAME_MAX) nlen = KDDS_NAME_MAX;
        pkt.name_len = (UH)nlen;
        kd_memcpy(pkt.name, t->name, nlen);
        kd_memcpy(pkt.data, data, len);

        /* REGION スコープなら自 region メンバにだけ送る。region_recompute()
         * を一度回し、ループ内は region_is_member() で安く判定する。
         * locality 計測のため region 判定は GLOBAL スコープでも要るので
         * (各配送先が近傍か遠方かを分類する)、常に一度 recompute する。 */
        BOOL region_scoped = (t->scope == KDDS_SCOPE_REGION);
        region_recompute();

        /* G9 (honesty): count DELIVERIES, not attempts. pmesh_send() returns
         * 0 only when the frame was actually handed to the link layer; on a
         * missing route it bumps pmesh_stats.no_route and returns -1. Counting
         * the attempt made fanout report packets that fell into a no_route
         * hole as if they had been sent — a quiet lie about scope reach. */
        UW fanout = 0;
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n == drpc_my_node) continue;
            if (dst >= 0 && (INT)n != dst) continue;
            if (dst < 0 && region_scoped && !region_is_member(n)) continue;
            if (pmesh_send(n, KDDS_PORT, (const UB *)&pkt, (UH)sizeof(pkt)) == 0) {
                fanout++;
                /* locality 累積: 配送先 n が自 region 外なら「遠方 (cross)」 */
                BOOL cross = !region_is_member(n);
                kdds_tx_msgs++;
                kdds_tx_bytes += (UW)sizeof(pkt);
                if (cross) {
                    kdds_tx_cross++;
                    kdds_tx_bytes_cross += (UW)sizeof(pkt);
                }
            }
        }
#if defined(_TK_HOSTED_LIBC_) && !defined(N2_NC_KDDS_NARROW_ONLY)
        /* N2 (ii)(c): the wide peers SWIM holds ALIVE get the broadcast too,
         * over the one-node PMESH v2 (pmesh_send picks v2 for a wide id and
         * sends only to a reader). kdds_rx does not read the src byte, so the
         * KDDS frame itself stays v1. kdds_pub_to keeps to narrow ids. */
        if (dst < 0) {
            static pk_node_t wide_ids[DREGION_MAX];
            INT nw = swim_wide_alive(wide_ids, DREGION_MAX);
            nw = kdds_wide_pick(wide_ids, nw, drpc_my_node, region_scoped, region_is_member, wide_ids);
            for (INT k = 0; k < nw; k++) {
                pk_node_t w = wide_ids[k];
                if (pmesh_send(w, KDDS_PORT, (const UB *)&pkt, (UH)sizeof(pkt)) == 0) {
                    fanout++;
                    kdds_tx_msgs++;
                    kdds_tx_bytes += (UW)sizeof(pkt);
                    if (!region_is_member(w)) {
                        kdds_tx_cross++;
                        kdds_tx_bytes_cross += (UW)sizeof(pkt);
                    }
                }
            }
        }
#endif
        kdds_last_fanout = fanout;
    } else {
        kdds_last_fanout = 0;
    }

    return 0;
}

W kdds_pub(W handle, const void *data, W len)
{
    return kdds_pub_core(handle, data, len, -1);
}

W kdds_pub_to(W handle, const void *data, W len, pk_node_t dst)
{
    /* 自分・範囲外は網へ送らない (ローカルの更新だけ) */
    INT d = (dst < DNODE_MAX && dst != drpc_my_node) ? (INT)dst : DNODE_MAX;
    return kdds_pub_core(handle, data, len, d);
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
        PK_NOSW_ENTER();                       /* pk_nosw.h: see kdds_pub */
        W copy = t->data_len < buflen ? t->data_len : buflen;
        kd_memcpy(buf, t->data, copy);
        PK_NOSW_LEAVE();
        return copy;
    }

    /* セマフォで次の pub を待つ */
    TMO tmo = (timeout_ms < 0) ? TMO_FEVR : (TMO)timeout_ms;
    ER  er  = tk_wai_sem(sem, 1, tmo);
    if (er != E_OK) return (W)er;

    /* データをコピーして返す */
    PK_NOSW_ENTER();
    W copy = t->data_len < buflen ? t->data_len : (W)buflen;
    kd_memcpy(buf, t->data, copy);
    PK_NOSW_LEAVE();
    return copy;
}

#ifdef _TK_HOSTED_LIBC_
/* R-KDDSQ stage 1: the oldest message this handle has not read (from the
 * ring or the slot), or 0 when there is none. Never waits. The first call
 * starts the handle's reading at the slot's current message. */
W kdds_sub_next(W handle, void *buf, W buflen)
{
    if (handle < 0 || handle >= KDDS_HANDLE_MAX || !kdds_handles[handle].open) return E_PAR;
    if (!buf || buflen <= 0) return E_PAR;
    W tidx = kdds_handles[handle].topic_idx;
    KDDS_TOPIC *t = &kdds_topics[tidx];
    W got = 0;
    PK_NOSW_ENTER();
    if (!kq_on[handle]) {
        kq_on[handle] = 1;
        kq_seq[handle] = (UH)(kq_tseq[tidx] - (t->data_len > 0 ? 1 : 0));
    }
    UH last = kq_seq[handle], best = 0;
    const UB *src = 0; UH slen = 0;
    for (INT i = 0; i < KDDS_Q_N; i++) {
        const KDDS_QSLOT *q = &kq_ring[i];
        if (!q->used || q->tidx != tidx || !kq_after(q->seq, last)) continue;
        UH d = (UH)(q->seq - last);
        if (!src || d < best) { best = d; src = q->data; slen = q->len; }
    }
    if (t->data_len > 0 && kq_after(kq_tseq[tidx], last)) {
        UH d = (UH)(kq_tseq[tidx] - last);
        if (!src || d < best) { best = d; src = t->data; slen = t->data_len; }
    }
    if (src) {
        got = slen < buflen ? slen : buflen;
        kd_memcpy(buf, src, got);
        kq_seq[handle] = (UH)(last + best);
    }
    PK_NOSW_LEAVE();
    return got;
}
#endif

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
    kdds_handles[handle].owner = 0;
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
/* kdds_set_owner / kdds_close_by_owner / kdds_handle_count           */
/*                                                                     */
/* KDDS-HANDLE-LEAK fix (wave-56, found during the kill-churn x86      */
/* work): a ring3 daemon (e.g. /infer_d.elf) opens "ai/req"+"ai/rsp"  */
/* each life via SYS_TOPIC_OPEN, but its kill/teardown                 */
/* (dproc_kill_by_name -> user_proc_teardown) never closed those       */
/* kdds handles.  The dying task vanished while its handle slots       */
/* stayed open; >~200 kill/respawn cycles exhausted KDDS_HANDLE_MAX    */
/* ("cannot open ai/rsp") and a downstream #DE.                        */
/*                                                                     */
/* The mechanism: tag each handle with the owning task id at           */
/* SYS_TOPIC_OPEN, then sweep-close all of a dying task's handles at   */
/* user_proc_teardown.  owner==0 (MIN_TSKID==1, so 0 is never a valid  */
/* task) marks kernel-internal handles (dproc/dkva/world/pfs/...) —    */
/* those are NEVER swept, because a real teardown always passes a      */
/* tid>=1 and the per-handle match requires owner==tid.                */
/* ------------------------------------------------------------------ */

void kdds_set_owner(W handle, ID tid)
{
    if (handle < 0 || handle >= KDDS_HANDLE_MAX) return;
    if (!kdds_handles[handle].open) return;
    if (tid <= 0) return;   /* 0/負は所有者なしのまま (掃き出し対象外) */
    kdds_handles[handle].owner = tid;
}

INT kdds_close_by_owner(ID tid)
{
    if (tid <= 0) return 0;   /* カーネル所有 (owner==0) は決して掃かない */
    INT closed = 0;
    for (W h = 0; h < KDDS_HANDLE_MAX; h++) {
        if (!kdds_handles[h].open) continue;
        if (kdds_handles[h].owner != tid) continue;
        kdds_close(h);   /* sem 削除 + owner クリア + 必要ならトピック解放 */
        closed++;
    }
    if (closed > 0) {
        kd_puts("[kdds] teardown closed "); kd_putdec((UW)closed);
        kd_puts(" handle(s) of tid="); kd_putdec((UW)tid);
        kd_puts("\r\n");
    }
    return closed;
}

INT kdds_handle_count(void)
{
    INT n = 0;
    for (W h = 0; h < KDDS_HANDLE_MAX; h++)
        if (kdds_handles[h].open) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* kdds_rx — リモートノードからのデータ受信                           */
/* ------------------------------------------------------------------ */

void kdds_rx(pk_node_t src_node, UH dst_port, const UB *data, UH len)
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
    PK_NOSW_ENTER();                           /* pk_nosw.h: see kdds_pub */
    KQ_SAVE(tidx);
    kd_memcpy(t->data, pkt->data, pkt->data_len);
    t->data_len = pkt->data_len;
    PK_NOSW_LEAVE();

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
        kdds_handles[h].owner     = 0;
    }
    pmesh_bind(KDDS_PORT, kdds_rx);
    kd_puts("[kdds] K-DDS ready  port=7376\r\n");
}

/* ------------------------------------------------------------------ */
/* kdds_delete_cluster — トピックをローカル削除 + 全ノードへ tombstone */
/* ------------------------------------------------------------------ */

void kdds_delete_cluster(const char *name)
{
    /* 削除対象トピックのインデックスを先に確定する。slot を閉じる前に
     * 捕まえておくことで、ハンドルを topic_idx で正確に判定できる
     * (名前再検索は slot を閉じた後だと一致しなくなる)。 */
    W dead_tidx = -1;
    for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
        if (!kdds_topics[i].open) continue;
        if (!kd_streq(kdds_topics[i].name, name)) continue;
        dead_tidx = i;
        break;
    }

    /* 対応するハンドルを全て閉じる (KDDS-DELCLUSTER, external audit
     * 2026-06-13: 以前はループ本体が空でハンドルが開きっぱなしで漏れていた)。
     * kdds_close と同じ後始末: subscriber セマフォを削除し、スロットを解放。 */
    if (dead_tidx >= 0) {
        for (W h = 0; h < KDDS_HANDLE_MAX; h++) {
            if (!kdds_handles[h].open) continue;
            if (kdds_handles[h].topic_idx != dead_tidx) continue;
            if (kdds_handles[h].sub_sem >= 0) {
                tk_del_sem(kdds_handles[h].sub_sem);
                kdds_handles[h].sub_sem = -1;
            }
            kdds_handles[h].topic_idx = -1;
            kdds_handles[h].owner     = 0;
            kdds_handles[h].open      = 0;
        }
        /* 全ハンドルを閉じたのでトピックスロットを解放する */
        kdds_topics[dead_tidx].open = 0;
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

    /* locality 計測カウンタ (wave-12 F隊 / G25) — harness が grep して
     * 推論前後の差分を取る。near = 同 region 宛, far = 異 region 宛。 */
    kd_puts("[locality] tx_msgs=");  kd_putdec(kdds_tx_msgs);
    kd_puts(" far_msgs=");           kd_putdec(kdds_tx_cross);
    kd_puts(" near_msgs=");          kd_putdec(kdds_tx_msgs - kdds_tx_cross);
    kd_puts(" tx_bytes=");           kd_putdec(kdds_tx_bytes);
    kd_puts(" far_bytes=");          kd_putdec(kdds_tx_bytes_cross);
    kd_puts(" near_bytes=");         kd_putdec(kdds_tx_bytes - kdds_tx_bytes_cross);
    kd_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* KDDS-DELCLUSTER self-test (external audit 2026-06-13, gap-ledger)   */
/*                                                                     */
/* Opens handles on a cluster topic via the REAL kdds_open path,       */
/* deletes the cluster via the REAL kdds_delete_cluster, and confirms  */
/* NO handle to the deleted topic remains open (the old empty loop     */
/* body leaked them) while an UNRELATED topic's handle is untouched.   */
/* Emits "[kdds-delcluster] PASS" (returns 0) or "FAIL ...".           */
/* ------------------------------------------------------------------ */
INT kdds_delcluster_self_test(void (*emit)(const char *))
{
    INT fails = 0;
    void (*say)(const char *) = emit ? emit : kd_puts;

    /* run in non-distributed mode so the tail replica_tombstone() is skipped */
    pk_node_t saved_my  = drpc_my_node;
    drpc_my_node = 0xFF;

    const char *DEL   = "test/delcluster";
    const char *KEEP  = "test/keepalive";

    /* open two handles on the SAME doomed topic + one on an unrelated topic */
    W h1 = kdds_open(DEL,  KDDS_QOS_LATEST_ONLY);
    W h2 = kdds_open(DEL,  KDDS_QOS_LATEST_ONLY);
    W hk = kdds_open(KEEP, KDDS_QOS_LATEST_ONLY);

    if (h1 < 0 || h2 < 0 || hk < 0) {
        say("[kdds-delcluster] FAIL open (table full?)\r\n");
        /* best-effort cleanup */
        if (h1 >= 0) kdds_close(h1);
        if (h2 >= 0) kdds_close(h2);
        if (hk >= 0) kdds_close(hk);
        drpc_my_node = saved_my;
        return 1;
    }

    W del_tidx = kdds_handles[h1].topic_idx;

    /* the bug: delete the cluster, then check the doomed handles */
    kdds_delete_cluster(DEL);

    /* (1) no handle to the deleted topic may remain open */
    for (W h = 0; h < KDDS_HANDLE_MAX; h++) {
        if (kdds_handles[h].open && kdds_handles[h].topic_idx == del_tidx) {
            say("[kdds-delcluster] FAIL leaked-handle (deleted topic still open)\r\n");
            fails++;
            break;
        }
    }
    /* (2) the topic slot itself must be freed */
    if (kdds_topics[del_tidx].open) {
        say("[kdds-delcluster] FAIL topic-slot-not-freed\r\n");
        fails++;
    }
    /* (3) the unrelated topic's handle must be untouched */
    if (!kdds_handles[hk].open) {
        say("[kdds-delcluster] FAIL collateral (unrelated handle closed)\r\n");
        fails++;
    }

    /* cleanup the survivor */
    kdds_close(hk);
    drpc_my_node = saved_my;

    if (fails == 0) say("[kdds-delcluster] PASS\r\n");
    return fails;
}

#ifdef _TK_HOSTED_LIBC_
/* ------------------------------------------------------------------ */
/* N2 (c)(1) `kdds wide`: which wide peers a broadcast goes to          */
/*   alive {260, 300, 5, 270}, region members 260 (and 5); self 270:    */
/*   [region] a REGION topic goes to 260 only (300 is out of region)    */
/*   [global] a GLOBAL topic goes to 260 and 300                        */
/*   [self]   self 260 is skipped; [narrow] 5 is never picked here      */
/* Falsifier: -DN2_NC_KDW_NO_REGION ([region] red).                     */
/* ------------------------------------------------------------------ */
#ifndef N2_NC_KDDS_NARROW_ONLY
static BOOL kw_member(pk_node_t n) { return (n == 260 || n == 5) ? TRUE : FALSE; }
#endif
INT kdds_wide_self_test(void (*emit)(const char *))
{
    void (*say)(const char *) = emit ? emit : kd_puts;
#ifdef N2_NC_KDDS_NARROW_ONLY
    say("[kdds-wide] FAIL narrow only (no wide peers at all)\r\n");
    return 1;
#else
    INT fails = 0;
    static const pk_node_t alive[4] = { 260, 300, 5, 270 };
    pk_node_t out[4];
    INT m;
    m = kdds_wide_pick(alive, 4, 270, TRUE, kw_member, out);
    if (m != 1 || out[0] != 260) { say("[kdds-wide] FAIL region (a REGION topic is not 260 only)\r\n"); fails++; }
    m = kdds_wide_pick(alive, 4, 270, FALSE, kw_member, out);
    if (m != 2 || out[0] != 260 || out[1] != 300) { say("[kdds-wide] FAIL global (a GLOBAL topic is not 260 and 300)\r\n"); fails++; }
    m = kdds_wide_pick(alive, 4, 260, FALSE, kw_member, out);
    if (m != 2 || out[0] != 300 || out[1] != 270) { say("[kdds-wide] FAIL self (self 260 was picked or a peer lost)\r\n"); fails++; }
    for (INT k = 0; k < m; k++) if (out[k] < DNODE_MAX) { say("[kdds-wide] FAIL narrow (a narrow id picked)\r\n"); fails++; break; }
    {   /* in place, as kdds_pub_core calls it */
        pk_node_t io[4] = { 300, 5, 260, 270 };
        m = kdds_wide_pick(io, 4, 270, TRUE, kw_member, io);
        if (m != 1 || io[0] != 260) { say("[kdds-wide] FAIL inplace (the in-place pick is not 260)\r\n"); fails++; }
    }
    if (fails == 0) say("[kdds-wide] PASS (wide peers: not self, not narrow, region members only for a REGION topic)\r\n");
    return fails;
#endif
}

/* ------------------------------------------------------------------ */
/* R-KDDSQ `kdds queue` (kdds-queue.md §3): non-distributed, one topic, */
/*   [pub]   three kdds_pub in a row -> kdds_sub_next gives 1 2 3, then 0 */
/*   [sub]   kdds_sub still gives the last one, every time               */
/*   [two]   a second next-reader also gets 1 2 3                        */
/*   [rx]    three kdds_rx in a row (as net_task hands them) -> 4 5 6    */
/*   [over]  40 in a row: increasing, ends at the 40th, drops counted    */
/* Falsifier: -DKDDS_NC_NO_QUEUE ([pub] [two] [rx] red).                */
/* ------------------------------------------------------------------ */
static void kq_rx_one(const char *name, UB v)
{
    static KDDS_PKT p;
    for (INT i = 0; i < (INT)sizeof p; i++) ((UB *)&p)[i] = 0;
    p.magic = KDDS_MAGIC; p.version = KDDS_VERSION; p.type = KDDS_DATA_PKT;
    p.src_node = 1; p.data_len = 1;
    INT n = kd_strlen(name) + 1;
    kd_memcpy(p.name, name, n); p.name_len = (UH)n;
    p.data[0] = v;
    kdds_rx(1, KDDS_PORT, (const UB *)&p, (UH)sizeof p);
}
INT kdds_queue_self_test(void (*emit)(const char *))
{
    void (*say)(const char *) = emit ? emit : kd_puts;
    INT fails = 0;
    pk_node_t saved_my = drpc_my_node;
    drpc_my_node = 0xFF;                       /* no wire: kdds_pub stays local */
    const char *NM = "test/kqueue";
    W hp = kdds_open_poll(NM, KDDS_QOS_LATEST_ONLY);
    W h1 = kdds_open_poll(NM, KDDS_QOS_LATEST_ONLY);
    W h2 = kdds_open_poll(NM, KDDS_QOS_LATEST_ONLY);
    if (hp < 0 || h1 < 0 || h2 < 0) {
        say("[kdds-queue] FAIL open\r\n");
        if (hp >= 0) kdds_close(hp);
        if (h1 >= 0) kdds_close(h1);
        if (h2 >= 0) kdds_close(h2);
        drpc_my_node = saved_my;
        return 1;
    }
    UB b[4];
    W r1 = kdds_sub_next(h1, b, 4), r2 = kdds_sub_next(h2, b, 4);   /* both start reading: nothing yet */
    if (r1 != 0 || r2 != 0) { say("[kdds-queue] FAIL empty (a message before any was sent)\r\n"); fails++; }
    for (UB v = 1; v <= 3; v++) kdds_pub(hp, &v, 1);
    INT ok = 1;
    for (UB v = 1; v <= 3; v++) { b[0] = 0; if (kdds_sub_next(h1, b, 4) != 1 || b[0] != v) ok = 0; }
    if (kdds_sub_next(h1, b, 4) != 0) ok = 0;
    if (!ok) { say("[kdds-queue] FAIL pub (three in a row did not come back 1 2 3 then none)\r\n"); fails++; }
    ok = 1;
    for (INT k = 0; k < 2; k++) { b[0] = 0; if (kdds_sub(h1, b, 4, 0) != 1 || b[0] != 3) ok = 0; }
    if (!ok) { say("[kdds-queue] FAIL sub (kdds_sub no longer gives the last one)\r\n"); fails++; }
    ok = 1;
    for (UB v = 1; v <= 3; v++) { b[0] = 0; if (kdds_sub_next(h2, b, 4) != 1 || b[0] != v) ok = 0; }
    if (!ok) { say("[kdds-queue] FAIL two (the second reader did not get 1 2 3)\r\n"); fails++; }
    for (UB v = 4; v <= 6; v++) kq_rx_one(NM, v);
    ok = 1;
    for (UB v = 4; v <= 6; v++) { b[0] = 0; if (kdds_sub_next(h1, b, 4) != 1 || b[0] != v) ok = 0; }
    if (kdds_sub_next(h1, b, 4) != 0) ok = 0;
    if (!ok) { say("[kdds-queue] FAIL rx (three received in a row did not come back 4 5 6)\r\n"); fails++; }
    (void)kdds_sub_next(h2, b, 4); (void)kdds_sub_next(h2, b, 4); (void)kdds_sub_next(h2, b, 4);
    UW d0 = kdds_queue_dropped(h1);
    for (UB v = 11; v <= 50; v++) kdds_pub(hp, &v, 1);
    ok = 1; INT got = 0; UB prev = 0, lastv = 0;
    for (;;) { b[0] = 0; W r = kdds_sub_next(h1, b, 4); if (r == 0) break; if (r != 1 || b[0] <= prev) ok = 0; prev = lastv = b[0]; got++; if (got > 60) break; }
    UW dr = kdds_queue_dropped(h1) - d0;
    if (!ok || lastv != 50 || got < KDDS_Q_N || got > KDDS_Q_N + 1 || dr == 0) {
        say("[kdds-queue] FAIL over (40 in a row: not increasing to 50, or no drop counted)\r\n"); fails++;
    }
    kd_puts("[kdds-queue] over: read "); kd_putdec((UW)got); kd_puts(" of 40, dropped "); kd_putdec(dr); kd_puts("\r\n");
    kdds_close(hp); kdds_close(h1); kdds_close(h2);
    drpc_my_node = saved_my;
    if (fails == 0) say("[kdds-queue] PASS (unread messages kept and read oldest first; kdds_sub unchanged)\r\n");
    return fails;
}
#endif
