/*
 *  dkva.c (x86)
 *  Phase 10 — Distributed KV Attention
 *
 *  【動作フロー】
 *
 *  Requester (どのノードでもよい — dkva_infer / dkva_cmd から):
 *    1. DKVA_Q_PKT を "dtr/dkva/q" へ pub
 *    2. 各ノードの "dtr/dkva/resp/<node>" を順にポーリングして
 *       DKVA_RESP_PKT を収集 (per-source topic なので peer ごとに独立)
 *    3. partial_out を集約して mhsa_out を計算
 *
 *  Responder (requester 以外の全ノード, dkva_task から):
 *    1. "dtr/dkva/q" を sub してキャッシュに対して Attention 計算
 *    2. partial_out + attn_sum を自ノード "dtr/dkva/resp/<my_node>" へ pub
 *
 *  集約 (Attention 合成):
 *    partial_out_total[t][h][d] = Σ_nodes partial_out[t][h][d]
 *    attn_sum_total  [t][h]     = Σ_nodes attn_sum[t][h]
 *    mhsa_out[t] = W_o · concat_heads(partial_out_total / attn_sum_total)
 */

#include "dkva.h"
#include "drpc.h"
#include "kdds.h"
#include "region.h"
#include "world.h"
#include "degrade.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);
IMPORT void dtr_seed_kv_cache(UB node);   /* dtr.c: KV キャッシュ warmup */

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void dk_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void dk_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { dk_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    dk_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* 数学ヘルパー                                                        */
/* ------------------------------------------------------------------ */

/* accurate range-reduced exp from dtr.c (R3a). The raw 7-term Taylor
 * that used to live here was wrong past |x|~3, which would make the
 * DISTRIBUTED attention softmax disagree with the local one now that
 * trained weights produce real logit gaps. */
IMPORT float dtr_expf(float x);
static float dk_exp(float x) { return dtr_expf(x); }

static float dk_sqrt(float x)
{
    if (x <= 0.0f) return 0.0f;
    float r = x > 1.0f ? x * 0.5f : 1.0f;
    r = (r + x / r) * 0.5f; r = (r + x / r) * 0.5f;
    r = (r + x / r) * 0.5f; r = (r + x / r) * 0.5f;
    return r;
}

/* ------------------------------------------------------------------ */
/* KV キャッシュ                                                       */
/* ------------------------------------------------------------------ */

/* ring buffer: 各エントリに DKVA_SEQ トークン分の K/V を保持 */
typedef struct {
    float K[DKVA_NH][DKVA_SEQ][DKVA_DH];
    float V[DKVA_NH][DKVA_SEQ][DKVA_DH];
    UB    valid;
} KV_ENTRY;

static KV_ENTRY kv_cache[DKVA_CACHE_SIZE];
static INT      kv_head  = 0;     /* 次回書き込み位置 */
static INT      kv_count = 0;     /* 有効エントリ数   */

/* ------------------------------------------------------------------ */
/* 統計                                                                */
/* ------------------------------------------------------------------ */

static UW stat_req_sent  = 0;
static UW stat_resp_got  = 0;
static UW stat_timeout   = 0;
static UW stat_resp_sent = 0;
static UW stat_degraded  = 0;   /* 部分集約で完遂した回数 (survival, wave 8) */

/* ------------------------------------------------------------------ */
/* K-DDS ハンドル                                                     */
/* ------------------------------------------------------------------ */

/* per-origin Query topics (G1, wave 10): h_q_pub[o] / h_q_sub[o] は
 * "dtr/dkva/q/<o>" を指す。requester は自ノードの h_q_pub[drpc_my_node] へ
 * 問いを発行し、responder は全 origin の h_q_sub[o] を購読する。dkva_init は
 * drpc_my_node 確定前 (boot 時) に走るので全 DNODE_MAX 分を pre-open しておき、
 * 実行時にノード ID で選ぶ (resp/rsum と同じ方式)。 */
static W h_q_pub[DNODE_MAX];
static W h_q_sub[DNODE_MAX];
/* per-source response topics: h_resp_pub[n] / h_resp_sub[n] は
 * "dtr/dkva/resp/<n>" を指す。responder は自ノード (drpc_my_node) の
 * pub ハンドルへ発行し、requester は自分以外の全 sub ハンドルから収集する。
 * dkva_init は drpc_my_node が確定する前 (boot 時) に走るので、全 DNODE_MAX
 * 分のトピックを開いておき、実行時にノード ID で選ぶ。 */
static W h_resp_pub[DNODE_MAX];
static W h_resp_sub[DNODE_MAX];

/* region 要約トピック (GLOBAL): coordinator が自 region の集約結果を rsum/<rid>
 * へ発行し、requester が region 間で畳む (regions R2, Y)。 */
static W h_rsum_pub[DNODE_MAX];
static W h_rsum_sub[DNODE_MAX];

/* "<pfx><node>" を out に組み立てる (node は 0..DNODE_MAX-1, 最大 2 桁。
 * resp/rsum が共用するので DNODE_MAX=32 対応で 2 桁化) */
static void node_topic_name(char *out, const char *pfx, UB node)
{
    INT i = 0;
    while (pfx[i]) { out[i] = pfx[i]; i++; }
    if (node >= 10) out[i++] = (char)('0' + node / 10);
    out[i++] = (char)('0' + node % 10);
    out[i]   = '\0';
}

/* ------------------------------------------------------------------ */
/* KV キャッシュ更新 (dtr.c から呼ぶ)                                */
/* ------------------------------------------------------------------ */

void dkva_cache_update(const float K[DKVA_NH][DKVA_SEQ][DKVA_DH],
                       const float V[DKVA_NH][DKVA_SEQ][DKVA_DH])
{
    INT idx = kv_head;
    for (INT h = 0; h < DKVA_NH; h++)
        for (INT t = 0; t < DKVA_SEQ; t++)
            for (INT d = 0; d < DKVA_DH; d++) {
                kv_cache[idx].K[h][t][d] = K[h][t][d];
                kv_cache[idx].V[h][t][d] = V[h][t][d];
            }
    kv_cache[idx].valid = 1;
    kv_head = (kv_head + 1) % DKVA_CACHE_SIZE;
    if (kv_count < DKVA_CACHE_SIZE) kv_count++;
}

/* ------------------------------------------------------------------ */
/* Responder: Q に対してキャッシュの K/V で Attention を計算          */
/* ------------------------------------------------------------------ */

static void compute_partial(const DKVA_Q_PKT *qpkt, DKVA_RESP_PKT *resp)
{
    float scale = 1.0f / dk_sqrt((float)DKVA_DH);

    /* partial_out / attn_sum を 0 初期化 */
    for (INT t = 0; t < DKVA_SEQ; t++)
        for (INT h = 0; h < DKVA_NH; h++) {
            resp->attn_sum[t][h] = 0.0f;
            for (INT d = 0; d < DKVA_DH; d++)
                resp->partial_out[t][h][d] = 0.0f;
        }

    INT n_used = 0;
    INT limit  = kv_count < DKVA_CACHE_SIZE ? kv_count : DKVA_CACHE_SIZE;

    for (INT ci = 0; ci < limit; ci++) {
        if (!kv_cache[ci].valid) continue;
        n_used++;

        for (INT h = 0; h < DKVA_NH; h++) {
            /* Q[t][h] · K[h][tk] → attn score */
            for (INT t = 0; t < DKVA_SEQ; t++) {
                for (INT tk = 0; tk < DKVA_SEQ; tk++) {
                    /* dot product: Q[t] · K[tk] */
                    float dot = 0.0f;
                    for (INT d = 0; d < DKVA_DH; d++)
                        dot += qpkt->Q[t][h][d] * kv_cache[ci].K[h][tk][d];
                    float a = dk_exp(dot * scale);   /* unnormalized attention */
                    resp->attn_sum[t][h] += a;

                    /* V の加重和 */
                    for (INT d = 0; d < DKVA_DH; d++)
                        resp->partial_out[t][h][d] += a * kv_cache[ci].V[h][tk][d];
                }
            }
        }
    }

    resp->n_entries = (UB)n_used;
}

/* DKVA_RESP_PKT (= {分子 partial_out, 分母 attn_sum}) を accumulator に加算。
 * partial も region 要約も同じ型なので共用できる。 */
static void accumulate(float total_out[DKVA_SEQ][DKVA_NH][DKVA_DH],
                       float total_sum[DKVA_SEQ][DKVA_NH],
                       const DKVA_RESP_PKT *rp)
{
    for (INT t = 0; t < DKVA_SEQ; t++)
        for (INT h = 0; h < DKVA_NH; h++) {
            total_sum[t][h] += rp->attn_sum[t][h];
            for (INT d = 0; d < DKVA_DH; d++)
                total_out[t][h][d] += rp->partial_out[t][h][d];
        }
}

/* rp の {分子, 分母} を別の RESP_PKT acc へ畳む (区分和は単純加算)。
 * G13 の event-driven 集約で running region 要約 (cagg[o]) を更新する。 */
static void accumulate_pkt(DKVA_RESP_PKT *acc, const DKVA_RESP_PKT *rp)
{
    for (INT t = 0; t < DKVA_SEQ; t++)
        for (INT h = 0; h < DKVA_NH; h++) {
            acc->attn_sum[t][h] += rp->attn_sum[t][h];
            for (INT d = 0; d < DKVA_DH; d++)
                acc->partial_out[t][h][d] += rp->partial_out[t][h][d];
        }
}

/* 自分以外に「自 region 外の生存ノード」が居るか (= rsum を読む他 region が
 * 存在するか)。region_is_member() を使うので呼ぶ前に region_recompute() 済みの
 * こと (region_coordinator() が直前に再計算する)。単一 region では coordinator が
 * rsum 集約 (200ms 窓) を丸ごと省けるので、同時多発の時間多重をブロックしない。 */
static BOOL have_remote_region(void)
{
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        if (dnode_table[n].state != DNODE_ALIVE) continue;
        if (!region_is_member(n)) return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Coordinator: 自 region の partial を集約して region 要約を発行       */
/*  ── G13: per-origin・event-driven (単一 200ms 同期窓を撤去) ──      */
/*                                                                     */
/* 全 region 内ノードが resp/<n> (region スコープ) へ partial を出すので、 */
/* coordinator はそれらを畳んで {分子, 分母} の region 要約を作り、       */
/* rsum/<my_node> (GLOBAL) へ発行する。requester はこれを region 間で      */
/* 疎に集約する。softmax の分子Σa·V と分母Σa は単純和なので、region 分割 */
/* → 和 → 最後に1回正規化、で単一ノード全体 attention と厳密に一致する。  */
/*                                                                     */
/* 旧実装は問いを受けるたびこの集約を responder ループ内で 200ms ブロック  */
/* していた → region 横断の同時多発を再直列化 (G13)。新実装は origin ごとに */
/* 独立した集約状態 (cagg[]) を持ち、responder ループの毎反復で少しずつ進め */
/* (cagg_step)、何もブロックしない。完了 (region メンバ出揃い) か per-origin */
/* 締切で確定し、resp と同じ round-robin 時間多重で rsum/<me> へ再発行する。 */
/* どの判断も local/region_recompute() の結果だけを読む — 中央調停も        */
/* グローバル順序も無い (§7)。                                            */
/* ------------------------------------------------------------------ */

/* origin o ごとの coordinator 集約状態 (すべて static — タスクスタックを汚さない)。
 *   phase: 0 = 非活性, 1 = 収集中 (region partial を fan-in), 2 = 確定済 (再発行中)
 *   cagg[o] : running region 要約 ({分子, 分母} を畳み込み中)。確定後そのまま rsum。
 *   exp[o][n]/got[o][n] : 期待した自 region メンバ / 既に畳んだメンバ
 *   dl[o]   : 収集締切 (残りループ反復数)。出揃わなくても締切で確定 (死を待たない)。
 *   rttl[o] : 確定後の rsum 再発行 TTL (round-robin で 1 反復 1 件)。 */
static DKVA_RESP_PKT cagg[DNODE_MAX];
static UB  cagg_phase[DNODE_MAX];
static UB  cagg_exp[DNODE_MAX][DNODE_MAX];
static UB  cagg_got[DNODE_MAX][DNODE_MAX];
static INT cagg_dl  [DNODE_MAX];
static INT cagg_rttl[DNODE_MAX];
static UW  cagg_entries[DNODE_MAX];
static UB  cagg_rr = 0;   /* rsum 再発行のラウンドロビン位置 */

static void cagg_reset_all(void)
{
    for (UB o = 0; o < DNODE_MAX; o++) {
        cagg_phase[o] = 0; cagg_dl[o] = 0; cagg_rttl[o] = 0; cagg_entries[o] = 0;
        for (UB n = 0; n < DNODE_MAX; n++) { cagg_exp[o][n] = 0; cagg_got[o][n] = 0; }
    }
    cagg_rr = 0;
}

/* 新しい問い (origin o, req_id) に対する coordinator 集約を開始/再起動する。
 * 自分の partial で seed し、いま自 region に生存するメンバを期待集合に取る。
 * region_recompute() は呼び出し側 (region_coordinator()) が直前に実行済み。 */
static void cagg_start(UB o, const DKVA_Q_PKT *qpkt, const DKVA_RESP_PKT *self_partial)
{
    /* running 要約 = 自分の partial。確定後そのまま rsum/<me> へ出すのでヘッダも整える。 */
    cagg[o] = *self_partial;
    cagg[o].magic    = DKVA_RESP_MAGIC;
    cagg[o].req_id   = qpkt->req_id;
    cagg[o].src_node = drpc_my_node;     /* = region id (coordinator)        */
    cagg[o].origin   = o;                /* この要約の宛先 = 問いの起点 (G1) */
    cagg_entries[o]  = self_partial->n_entries;

    for (UB n = 0; n < DNODE_MAX; n++) { cagg_exp[o][n] = 0; cagg_got[o][n] = 0; }
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        UB st = dnode_table[n].state;
        if (st != DNODE_ALIVE && st != DNODE_SUSPECT) continue;
        if (!region_is_member(n)) continue;
        cagg_exp[o][n] = 1;
    }
    cagg_dl[o]    = DKVA_RSUM_WIN_ITERS;
    cagg_rttl[o]  = 0;
    cagg_phase[o] = 1;                    /* 収集開始 */
}

/* responder ループ毎反復で 1 回呼ぶ。活性な全 origin の収集を少しずつ進め、
 * 出揃い or 締切で確定 (phase 2 へ)。何もブロックしない。 */
static void cagg_step(void)
{
    BOOL any = FALSE;
    for (UB o = 0; o < DNODE_MAX; o++) if (cagg_phase[o] == 1) { any = TRUE; break; }
    if (!any) return;

    region_recompute();   /* 全 origin で 1 回だけ */
    for (UB o = 0; o < DNODE_MAX; o++) {
        if (cagg_phase[o] != 1) continue;

        /* 自 region メンバの per-source partial を非ブロッキングで取り込む */
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n == drpc_my_node || cagg_got[o][n] || h_resp_sub[n] < 0) continue;
            if (!cagg_exp[o][n]) continue;
            DKVA_RESP_PKT rp = { 0 };
            W r = kdds_sub(h_resp_sub[n], &rp, (W)sizeof(rp), 0);
            if (r >= (W)sizeof(DKVA_RESP_PKT) &&
                rp.magic == DKVA_RESP_MAGIC && rp.req_id == cagg[o].req_id &&
                rp.src_node == n && rp.origin == o) {
                cagg_got[o][n] = 1;
                cagg_entries[o] += rp.n_entries;
                accumulate_pkt(&cagg[o], &rp);
            }
        }

        /* 期待メンバのうち未着で、かつ SWIM が DEAD と判定したものは待たない
         * (死を待たない; その欠損は requester 側の degraded(k/n) が正直に計上)。 */
        INT pending = 0;
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (cagg_exp[o][n] && !cagg_got[o][n] &&
                dnode_table[n].state != DNODE_DEAD) pending++;
        }

        cagg_dl[o]--;
        if (pending == 0 || cagg_dl[o] <= 0) {
            /* region 要約を確定 → round-robin 再発行フェーズへ */
            cagg[o].n_entries = (UB)(cagg_entries[o] > 255 ? 255 : cagg_entries[o]);
            cagg_phase[o] = 2;
            cagg_rttl[o]  = DKVA_ANSWER_ITERS;
            dk_puts("[dkva] region summary published  rid="); dk_putdec(drpc_my_node);
            dk_puts("  origin="); dk_putdec(o);
            dk_puts("  entries="); dk_putdec(cagg_entries[o]); dk_puts("\r\n");
        }
    }
}

/* responder ループ毎反復で 1 回呼ぶ。確定済 (phase 2) の origin の rsum を
 * round-robin で 1 件だけ rsum/<me> へ再発行する (resp と同じ時間多重)。
 * これで複数 origin の rsum が単一 LATEST_ONLY スロットを潰し合わず、各起点が
 * 自分宛 (origin==自ノード) の要約を自分のポーリング窓内で取り出せる。 */
static void cagg_republish(void)
{
    if (drpc_my_node >= DNODE_MAX || h_rsum_pub[drpc_my_node] < 0) return;
    for (INT scan = 0; scan < DNODE_MAX; scan++) {
        cagg_rr = (UB)((cagg_rr + 1) % DNODE_MAX);
        if (cagg_phase[cagg_rr] == 2 && cagg_rttl[cagg_rr] > 0) {
            kdds_pub(h_rsum_pub[drpc_my_node], &cagg[cagg_rr],
                     (W)sizeof(DKVA_RESP_PKT));
            if (--cagg_rttl[cagg_rr] == 0) cagg_phase[cagg_rr] = 0;
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Requester: 自 region 直接集約 + 他 region 要約を畳んで mhsa_out 計算 */
/* ------------------------------------------------------------------ */

ER dkva_infer(const float Q[DKVA_SEQ][DKVA_NH][DKVA_DH],
              const float W_o[DKVA_DM][DKVA_DM],
              float mhsa_out[DKVA_SEQ][DKVA_DM],
              UW req_id)
{
    UB me = drpc_my_node;
    if (me == 0xFF || me >= DNODE_MAX || h_q_pub[me] < 0) return E_NOEXS;

    /* Query パケット送信 (起点ごとの per-origin トピックへ — G1) */
    DKVA_Q_PKT qpkt = { 0 };
    qpkt.magic    = DKVA_Q_MAGIC;
    qpkt.req_id   = req_id;
    qpkt.src_node = me;
    qpkt.n_cached = DKVA_CACHE_SIZE;
    for (INT t = 0; t < DKVA_SEQ; t++)
        for (INT h = 0; h < DKVA_NH; h++)
            for (INT d = 0; d < DKVA_DH; d++)
                qpkt.Q[t][h][d] = Q[t][h][d];

    kdds_pub(h_q_pub[me], &qpkt, (W)sizeof(qpkt));
    stat_req_sent++;

    dk_puts("[dkva] Q broadcast req="); dk_putdec(req_id); dk_puts("\r\n");

    /* 部分レスポンスを集約 */
    float total_out [DKVA_SEQ][DKVA_NH][DKVA_DH];
    float total_sum [DKVA_SEQ][DKVA_NH];
    for (INT t = 0; t < DKVA_SEQ; t++)
        for (INT h = 0; h < DKVA_NH; h++) {
            total_sum[t][h] = 0.0f;
            for (INT d = 0; d < DKVA_DH; d++) total_out[t][h][d] = 0.0f;
        }

    /* capacity(N) の KV-context 軸: 実際に畳み込んだ KV エントリ総数を測る。 */
    UW total_entries = 0;

    /* 自分のキャッシュも含める */
    {
        DKVA_RESP_PKT self_resp = { 0 };
        self_resp.req_id = req_id;
        self_resp.origin = me;
        compute_partial(&qpkt, &self_resp);
        accumulate(total_out, total_sum, &self_resp);
        total_entries += self_resp.n_entries;
    }

    /* 階層集約 (regions R2, Y):
     *   ・自 region: 各メンバが resp/<n> (region スコープ) に出した partial を
     *     直接集約する。resp は region 内に閉じるので requester には自 region の
     *     partial しか届かない。
     *   ・他 region: 各 region の coordinator が rsum/<rid> (GLOBAL) に出した
     *     region 要約を畳む。自 region の coordinator の rsum は直接集約と二重に
     *     なるのでスキップする。
     * softmax の分子Σa·V と分母Σa は単純和なので、これで単一ノード全体 attention
     * と厳密に同じ結果になる。 */
    UB my_coord = region_coordinator();   /* region_recompute() 込み */
    INT  tmo_left  = DKVA_INFER_TMO;
    INT  resp_cnt  = 0;       /* 自 region の直接 partial 数         */
    INT  rsum_cnt  = 0;       /* 畳んだ他 region 要約数              */
    UB   got [DNODE_MAX];     /* 自 region partial の重複防止        */
    UB   rgot[DNODE_MAX];     /* 他 region 要約の重複防止 (rid 単位)  */

    /* --- 死を待たない / region 横断の正直な degraded -----------------
     * (wave 8 + G2/G8, wave 10; gossip 鮮度の正直化 wave 12 / G12)
     *  期待集合は 2 種類 + 不確実枠:
     *    expect[n]   : fan-out 時に SWIM 生存 (ALIVE/SUSPECT) と見ている
     *                  自 region のピア (直接 partial を待つ)。SWIM の RTT に
     *                  基づくので gossip 鮮度に依存しない (= 確実)。
     *    rc_expect[c]: 応答すべき他 region の coordinator c (rsum を待つ)。
     *                  ★G12: coordinator の特定は world gossip 由来なので、
     *                  「新鮮に確認できた」region_id (world_peer_region_fresh)
     *                  だけから組む。これが degraded の確定分母になる。
     *    uncertain[n]: SWIM では生存 remote だが gossip 未着/古くて region を
     *                  ★確認できないノード。以前 (wave 10) はこれを「n 自身=1
     *                  region」と決め打ちして rc_cnt0 に積んでいた → 複数メンバ
     *                  から成る remote region で coordinator ビーコン未着だと各
     *                  メンバを別 region と過大計上し、実際は全寄与が畳めていても
     *                  degraded を誤って出していた (数字が gossip-timing 依存=嘘)。
     *                  wave 12 では確定分母に積まず、不確実 (uncertain) として
     *                  別軸で正直に明示する (沈黙で確定値を装わない / §10)。
     *  SWIM が DEAD と判定したノード/region は待たない (ハングしない) が、
     *  欠損として degraded (k/n) に正直計上する (黙って成功にしない)。
     *  exp_cnt0 / rc_cnt0 は fan-out 時の確定期待数 — degraded の分母を成す。 */
    UB   expect[DNODE_MAX];
    UB   rc_expect[DNODE_MAX];
    UB   uncertain[DNODE_MAX];   /* gossip 未確認の生存 remote (別軸)   */
    INT  exp_cnt0     = 0;    /* 期待した自 region ピア数            */
    INT  exp_got      = 0;    /* うち実際に応答した数                */
    INT  rc_cnt0      = 0;    /* 確認できた他 region 数 (coordinator) */
    INT  rc_got       = 0;    /* うち rsum が届いた数                */
    INT  uncertain_cnt = 0;  /* region を確認できない生存 remote 数  */
    region_recompute();
    for (UB i = 0; i < DNODE_MAX; i++) {
        got[i] = 0; rgot[i] = 0; expect[i] = 0; rc_expect[i] = 0;
        uncertain[i] = 0;
    }
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == me) continue;
        UB st = dnode_table[n].state;
        if (st != DNODE_ALIVE && st != DNODE_SUSPECT) continue;
        if (region_is_member(n)) { expect[n] = 1; exp_cnt0++; }
        else {
            /* 他 region のノード: その coordinator (= 応答すべき rsum の発行者)
             * を world gossip から引く。★新鮮に確認できたときだけ確定分母に積む
             * (G12)。確認できない (未着/古い) ノードは別 region と決め打ちせず
             * uncertain として別計上する — degraded の数字を gossip 鮮度に依存
             * させない (過大計上で嘘をつかない)。 */
            INT cid = world_peer_region_fresh(n);
            if (cid >= 0 && cid < DNODE_MAX && (UB)cid != me) {
                if (!rc_expect[cid]) { rc_expect[cid] = 1; rc_cnt0++; }
            } else {
                uncertain[n] = 1; uncertain_cnt++;
            }
        }
    }

    while (tmo_left > 0) {
        for (UB n = 0; n < DNODE_MAX; n++) {
            /* --- 自 region の per-source partial (自分宛 origin==me のみ) --- */
            if (n != me && !got[n] && h_resp_sub[n] >= 0) {
                DKVA_RESP_PKT rp = { 0 };
                W r = kdds_sub(h_resp_sub[n], &rp, (W)sizeof(rp), 0);
                if (r >= (W)sizeof(DKVA_RESP_PKT) &&
                    rp.magic == DKVA_RESP_MAGIC && rp.req_id == req_id &&
                    rp.src_node == n && rp.origin == me) {
                    got[n] = 1;
                    if (expect[n]) exp_got++;
                    resp_cnt++;
                    stat_resp_got++;
                    total_entries += rp.n_entries;
                    accumulate(total_out, total_sum, &rp);
                    dk_puts("[dkva] resp from node "); dk_putdec(rp.src_node);
                    dk_puts("  entries="); dk_putdec(rp.n_entries); dk_puts("\r\n");
                }
            }
            /* --- 他 region の要約 (自 region coordinator はスキップ; origin==me) --- */
            if (n != my_coord && !rgot[n] && h_rsum_sub[n] >= 0) {
                DKVA_RESP_PKT rs = { 0 };
                W r = kdds_sub(h_rsum_sub[n], &rs, (W)sizeof(rs), 0);
                if (r >= (W)sizeof(DKVA_RESP_PKT) &&
                    rs.magic == DKVA_RESP_MAGIC && rs.req_id == req_id &&
                    rs.src_node == n && rs.origin == me) {
                    rgot[n] = 1;
                    rsum_cnt++;
                    /* rsum が届いた = n は実在する region coordinator だと
                     * 確定した。gossip 未確認 (uncertain) だった場合はここで
                     * 確定分母へ昇格させる (到着 = 確証)。これで「不確実だった
                     * が応答した」region は正しく k/n の両方に乗り、過大計上も
                     * 取りこぼしもしない (G12)。 */
                    if (uncertain[n]) {
                        uncertain[n] = 0; uncertain_cnt--;
                        rc_expect[n] = 1; rc_cnt0++;
                    }
                    if (rc_expect[n]) rc_got++;
                    total_entries += rs.n_entries;
                    accumulate(total_out, total_sum, &rs);
                    dk_puts("[dkva] region summary rid="); dk_putdec(n);
                    dk_puts("  entries="); dk_putdec(rs.n_entries); dk_puts("\r\n");
                }
            }
        }

        /* SWIM が待っている間に DEAD と判定したノードはもう待たない
         * (が degraded の分子には数えないので欠損として正直に残る)。 */
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (expect[n] && !got[n] && dnode_table[n].state == DNODE_DEAD) {
                expect[n] = 0;
                dk_puts("[dkva] not waiting for node "); dk_putdec(n);
                dk_puts(" (SWIM: DEAD)\r\n");
            }
            /* gossip 未確認のまま死んだ remote はもう不確実枠でも待たない
             * (生存していないので uncertain として明示する意味も無い)。 */
            if (uncertain[n] && dnode_table[n].state == DNODE_DEAD) {
                uncertain[n] = 0; uncertain_cnt--;
            }
        }
        /* 期待した全寄与 (自 region ピア + 他 region coordinator) が
         * got もしくは DEAD で解決したら、窓を待たず確定する。
         * ★ただし uncertain な生存 remote が残る間は早期確定しない (G12):
         * その rsum がまだ来るかもしれず、ここで打ち切ると「gossip 未収束」を
         * 確定値で覆い隠してしまう。確実分が片付いていても窓を待ち切る。
         * 期待も不確実も 0 のときは早期確定しない (SWIM が peer 未発見なだけ
         * かも)。 */
        if ((exp_cnt0 + rc_cnt0) > 0 && uncertain_cnt == 0) {
            INT pending = 0;
            for (UB n = 0; n < DNODE_MAX; n++) {
                if (expect[n] && !got[n]) pending++;
                if (rc_expect[n] && !rgot[n] &&
                    dnode_table[n].state != DNODE_DEAD) pending++;
            }
            if (pending == 0) break;
        }

        tk_dly_tsk(20);
        tmo_left -= 20;
    }

    /* 期待も寄与も不確実枠も無い = 真の単独ノード → ローカル MHSA へ
     * フォールバック。uncertain な生存 remote が居る場合は単独ではない
     * (peer は居るが gossip で region を確認できなかっただけ) ので solo 扱い
     * しない — 自分の partial を持って下の honesty 報告へ進む (G12)。 */
    if (resp_cnt == 0 && rsum_cnt == 0 &&
        (exp_cnt0 + rc_cnt0) == 0 && uncertain_cnt == 0) {
        stat_timeout++;
        dk_puts("[dkva] timeout: no cluster peers\r\n");
        return E_TMOUT;
    }

    /* 正直さ規約 (wave 8 + G2/G8; gossip 鮮度の正直化 wave 12 / G12):
     * fan-out 時に期待した寄与 (自 region ピア + 他 region) が欠けたまま完遂
     * するときは、黙って成功にせず必ず明示する。
     *   k = 自分 + 応答した region ピア + 畳んだ (確認できた) 他 region
     *   n = 自分 + 期待した region ピア + 確認できた他 region
     * 他 region の coordinator 消失 (G8) もこの分母に乗るので無音消失しない。
     * ★G12: k/n は「新鮮に確認できた」期待だけで組む (gossip 鮮度に依存して
     * 過大計上しない)。それとは別に、gossip 未着/古さで region を確認できない
     * 生存 remote が m 個あるなら、それを uncertain として degraded 表示に
     * 添える: `degraded (k/n; m uncertain)`。degraded でなくても m>0 なら
     * 「確定値を装わず」未収束を明示する (§10 古さ・不完全さの明示 / I4)。 */
    INT got_total = 1 + exp_got + rc_got;
    INT exp_total = 1 + exp_cnt0 + rc_cnt0;
    if (got_total < exp_total) {
        stat_degraded++;
        dk_puts("[dkva] degraded ("); dk_putdec((UW)got_total);
        dk_puts("/"); dk_putdec((UW)exp_total);
        if (uncertain_cnt > 0) {
            dk_puts("; "); dk_putdec((UW)uncertain_cnt);
            dk_puts(" uncertain");
        }
        dk_puts("): completed with partial aggregation");
        if (uncertain_cnt > 0)
            dk_puts(" (gossip unconverged — count provisional)");
        dk_puts("  req="); dk_putdec(req_id); dk_puts("\r\n");
    } else if (uncertain_cnt > 0) {
        /* 確定分は全て畳めた (k==n) が、gossip 未着/古さで region を確認できない
         * 生存 remote が残る。確定値で「完全成功」を装わず、不確実性を明示する
         * — degraded の数字自体が gossip 収束まで暫定であることを正直に添える。 */
        stat_degraded++;
        dk_puts("[dkva] degraded ("); dk_putdec((UW)got_total);
        dk_puts("/"); dk_putdec((UW)exp_total);
        dk_puts("; "); dk_putdec((UW)uncertain_cnt);
        dk_puts(" uncertain): confirmed contributions complete, but ");
        dk_putdec((UW)uncertain_cnt);
        dk_puts(" remote node(s) unconfirmed by gossip — degraded count ");
        dk_puts("provisional until gossip converges  req=");
        dk_putdec(req_id); dk_puts("\r\n");
    }

    /* 正規化: attn_out[t][h] = total_out[t][h] / total_sum[t][h] */
    /* W_o 投影: mhsa_out[t] = W_o · concat_heads(attn_out[t]) */
    for (INT t = 0; t < DKVA_SEQ; t++) {
        float concat[DKVA_DM];   /* [h0_d0..h0_dDH-1, h1_d0..h1_dDH-1] */
        for (INT h = 0; h < DKVA_NH; h++) {
            float denom = total_sum[t][h] > 1e-10f ? total_sum[t][h] : 1e-10f;
            for (INT d = 0; d < DKVA_DH; d++)
                concat[h * DKVA_DH + d] = total_out[t][h][d] / denom;
        }
        /* mhsa_out[t] = W_o · concat */
        for (INT m = 0; m < DKVA_DM; m++) {
            float s = 0.0f;
            for (INT n = 0; n < DKVA_DM; n++) s += W_o[m][n] * concat[n];
            mhsa_out[t][m] = s;
        }
    }

    /* capacity(N) の KV-context 軸へ実測値を供給 (regions.md §3.2)。 */
    capacity_note_kv(total_entries);

    dk_puts("[dkva] aggregated "); dk_putdec((UW)resp_cnt);
    dk_puts(" region peers + "); dk_putdec((UW)rsum_cnt);
    dk_puts(" remote regions  ("); dk_putdec(total_entries);
    dk_puts(" KV entries)  req="); dk_putdec(req_id); dk_puts("\r\n");
    return E_OK;
}

/* ------------------------------------------------------------------ */
/* Responder タスク: dtr_task とは別タスクで回す                      */
/* ------------------------------------------------------------------ */

void dkva_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    if (drpc_my_node == 0xFF) return;
    UB me = drpc_my_node;

    /* 全ノードが Responder として動作する。Requester も特定ノードでは
     * なく「いま問いを発行したノード」でしかない (起点に特権はない)。 */

    dk_puts("[dkva] responder task started  node=");
    dk_putdec(me); dk_puts("\r\n");

    /* KV キャッシュ warmup: 新規 FULL クラスタでは全ノードのキャッシュが空で
     * partial Attention が自明 (entries=0) になるため、自ノード固有の合成入力で
     * seed しておく。dkva_task は drpc_my_node 確定後に走るので node 固有値が使える。 */
    dtr_seed_kv_cache(me);
    dk_puts("[dkva] kv-cache seeded for node="); dk_putdec(me);
    dk_puts("\r\n");

    /* per-origin の応答キャッシュ (G1 時間多重). すべて static — 小タスクスタック
     * (4KB) に大物 (resp_cache ≈ 5.5KB) を置かない。
     *   pend_req[o] : いま応答中の req_id (0=なし)。q/<o> は LATEST_ONLY で
     *                 同じ Q を再配信するので、req_id 変化時だけ再計算する
     *                 (dkva_infer は応答を SUM するので二重計上防止に必須)。
     *   pend_ttl[o] : 残り再発行回数。新規問いが来たらリセットする。
     * 同時に複数 origin が問うても、各 origin の応答を resp/<me> の単一スロットへ
     * ラウンドロビンで再発行することで、どの起点も自分のポーリング窓内で
     * 自分宛 (origin==自ノード) の応答を取り出せる。 */
    static DKVA_RESP_PKT resp_cache[DNODE_MAX];
    static UW  pend_req[DNODE_MAX];
    static INT pend_ttl[DNODE_MAX];
    for (UB i = 0; i < DNODE_MAX; i++) { pend_req[i] = 0; pend_ttl[i] = 0; }
    UB rr = 0;
    cagg_reset_all();   /* G13: per-origin event-driven coordinator 集約状態 */

    for (;;) {
        /* 1) 全 origin の q/<o> を走査。新しい req_id には partial を計算して
         *    resp_cache[o] に積む (再発行用に保持)。 */
        for (UB o = 0; o < DNODE_MAX; o++) {
            if (o == me || h_q_sub[o] < 0) continue;
            DKVA_Q_PKT qpkt = { 0 };
            W r = kdds_sub(h_q_sub[o], &qpkt, (W)sizeof(qpkt), 0);
            if (r < (W)sizeof(DKVA_Q_PKT) || qpkt.magic != DKVA_Q_MAGIC) continue;
            if (qpkt.src_node != o || qpkt.src_node >= DNODE_MAX) continue;
            if (qpkt.req_id == pend_req[o]) continue;   /* 既に応答中 */

            DKVA_RESP_PKT *rc = &resp_cache[o];
            UB *rb = (UB *)rc;
            for (INT z = 0; z < (INT)sizeof(*rc); z++) rb[z] = 0;
            rc->magic    = DKVA_RESP_MAGIC;
            rc->req_id   = qpkt.req_id;
            rc->src_node = me;
            rc->origin   = o;
            compute_partial(&qpkt, rc);
            pend_req[o] = qpkt.req_id;
            pend_ttl[o] = DKVA_ANSWER_ITERS;
            stat_resp_sent++;

            dk_puts("[dkva] responded to node "); dk_putdec(o);
            dk_puts("  req="); dk_putdec(qpkt.req_id);
            dk_puts("  entries="); dk_putdec(rc->n_entries); dk_puts("\r\n");

            /* 自 region の coordinator かつ他 region が存在するときだけ、region
             * partial を畳んで rsum/<me> を発行する (regions R2, Y)。単一 region
             * では rsum は誰も読まないので集約自体を省く。
             * ★G13: 集約は「この origin の event-driven 状態」を *開始* するだけ
             * (cagg_start)。実際の fan-in は下の cagg_step が毎反復で少しずつ進める
             * ので、ここで 200ms ブロックして同時多発を直列化しない。
             * region_coordinator() が region_recompute() するので have_remote_region()
             * / cagg_start の生存判定は最新を見る。 */
            if (region_coordinator() == me && have_remote_region())
                cagg_start(o, &qpkt, rc);
        }

        /* 2) pending な応答を 1 つラウンドロビンで resp/<me> へ再発行 (時間多重)。
         *    1 反復に 1 件だけ発行することで、各 origin の応答が単一スロットに
         *    一定時間ずつ滞在し、全起点が自分宛を取りこぼさない。 */
        if (me < DNODE_MAX && h_resp_pub[me] >= 0) {
            for (INT scan = 0; scan < DNODE_MAX; scan++) {
                rr = (UB)((rr + 1) % DNODE_MAX);
                if (pend_ttl[rr] > 0) {
                    kdds_pub(h_resp_pub[me], &resp_cache[rr],
                             (W)sizeof(DKVA_RESP_PKT));
                    pend_ttl[rr]--;
                    break;
                }
            }
        }

        /* 3) G13: coordinator の per-origin 集約を 1 ステップ進め (cagg_step)、
         *    確定済の rsum を round-robin で 1 件再発行する (cagg_republish)。
         *    どちらもブロックしないので、何件の問いが region をまたいで同時に
         *    来ても responder ループは凍らず、全 origin が並行に集約される。 */
        cagg_step();
        cagg_republish();

        tk_dly_tsk(10);
    }
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void dkva_init(void)
{
    kv_head  = 0;
    kv_count = 0;
    stat_req_sent = stat_resp_got = stat_timeout = stat_resp_sent = 0;
    for (INT i = 0; i < DKVA_CACHE_SIZE; i++) kv_cache[i].valid = 0;
    for (UB n = 0; n < DNODE_MAX; n++) {
        h_q_pub[n]    = -1; h_q_sub[n]    = -1;
        h_resp_pub[n] = -1; h_resp_sub[n] = -1;
        h_rsum_pub[n] = -1; h_rsum_sub[n] = -1;
    }

    /* 階層集約 (regions R2, Y — docs/architecture/regions.md):
     *   Q     : GLOBAL  — 全 region のノードが partial を計算する (per-origin, G1)
     *   resp  : REGION  — per-node partial は自 region 内に留まる (密)
     *   rsum  : GLOBAL  — coordinator の region 要約だけ region 間を渡る (疎)
     * これで通信は region 内 O(region²) + region 間 O(#region) に収まりつつ、
     * 結果は単一ノード全体 attention と厳密に一致する。
     *
     * トピック名は drpc_my_node に依存しない (q/<o>, resp/<n>, rsum/<rid>) ので、
     * my_node 未確定の boot 段階で全 DNODE_MAX 分を pre-open でき、実行時に
     * ノード ID で選べる。全ハンドルは timeout=0 ポーリングで読むため
     * kdds_open_poll_scoped (zero-sem) で開く。 */
    for (UB n = 0; n < DNODE_MAX; n++) {
        char tn[KDDS_NAME_MAX];
        node_topic_name(tn, DKVA_TOPIC_Q_PFX, n);      /* q は GLOBAL (per-origin) */
        h_q_pub[n]    = kdds_open_poll_scoped(tn, KDDS_QOS_LATEST_ONLY,
                                              KDDS_SCOPE_GLOBAL);
        h_q_sub[n]    = kdds_open_poll_scoped(tn, KDDS_QOS_LATEST_ONLY,
                                              KDDS_SCOPE_GLOBAL);
        node_topic_name(tn, DKVA_TOPIC_RESP_PFX, n);   /* resp は REGION */
        h_resp_pub[n] = kdds_open_poll_scoped(tn, KDDS_QOS_LATEST_ONLY,
                                              KDDS_SCOPE_REGION);
        h_resp_sub[n] = kdds_open_poll_scoped(tn, KDDS_QOS_LATEST_ONLY,
                                              KDDS_SCOPE_REGION);
        node_topic_name(tn, DKVA_TOPIC_RSUM_PFX, n);   /* rsum は GLOBAL */
        h_rsum_pub[n] = kdds_open_poll_scoped(tn, KDDS_QOS_LATEST_ONLY,
                                              KDDS_SCOPE_GLOBAL);
        h_rsum_sub[n] = kdds_open_poll_scoped(tn, KDDS_QOS_LATEST_ONLY,
                                              KDDS_SCOPE_GLOBAL);
    }

    /* ── 容量検算 (DNODE_MAX=32, wave 10 G1) ───────────────────────────
     *  dkva が pre-open する数 (1 ノードあたり):
     *    トピック : q 32 + resp 32 + rsum 32              = 96   (< KDDS_TOPIC_MAX  160)
     *    ハンドル : (pub+sub) × 3 × 32                    = 192  (< KDDS_HANDLE_MAX 320)
     *    セマフォ : 0  ← 全て kdds_open_poll_scoped(zero-sem)(< CFN_MAX_SEMID   256)
     *  per-origin Q (G1) は単一トピック (+1) を 32 トピック (+31) に増やすが、
     *  旧実装は q/resp/rsum を blocking open して 130 個のセマフォを浪費していた。
     *  これらは全て timeout=0 ポーリングで読むので poll-open に切替え、dkva の
     *  セマフォ消費を 0 にした → CFN_MAX_SEMID を上げる必要なし。
     *  注: 実クラスタは regions が近接ノードを小さく束ねるため、moe/world 等の
     *  per-source トピックと合算しても 1 ホストで 32 ノード全部を同時開放する
     *  構成は元々取らない (テストは ≤4 ノード, 実測トピック数 << 160)。 */

    dk_puts("[dkva] initialized (hierarchical, per-origin Q)  cache=");
    dk_putdec(DKVA_CACHE_SIZE);
    dk_puts("  Q=global resp=region rsum=global (poll, zero-sem)\r\n");
}

/* ------------------------------------------------------------------ */
/* In-process プロパティ自己テスト (G13)                               */
/*                                                                     */
/* §5「同時多発・並行分散」の COMPUTE 側の核を「数で」守る。証明したい   */
/* 不変量は 3 つ:                                                       */
/*   P1 順序非依存 : ある origin の region 要約は partial の畳み込み順序 */
/*                   に依らず同じ (到着順がどうであれ同じ答え)。         */
/*   P2 origin 非汚染: 複数 origin の集約を *交互に* 進めても、各 origin */
/*                   の結果はその origin の partial だけの和に等しい     */
/*                   (単一共有 accumulator/窓が無い ⇔ 相互非干渉)。       */
/*   P3 同時数不変 : ある origin の結果は「同時に集約中の他 origin の数」 */
/*                   に依らない (= 並行しても直列と同じ; 単一窓が無い証拠)。*/
/* 値は小整数 float なので加算は厳密 — 順序差を浮動小数誤差で誤魔化さない。*/
/* 純ローカル (network/kdds 非依存) なのでベアメタルでも走る。           */
/* ------------------------------------------------------------------ */

static void st_zero(DKVA_RESP_PKT *p)
{
    UB *b = (UB *)p;
    for (INT i = 0; i < (INT)sizeof(*p); i++) b[i] = 0;
    p->magic = DKVA_RESP_MAGIC;
}

/* origin/seed から決定論的に小整数 partial を作る (1..n の整数 float)。 */
static void st_fill(DKVA_RESP_PKT *p, UB origin, INT seed)
{
    st_zero(p);
    p->origin = origin;
    p->src_node = (UB)seed;
    for (INT t = 0; t < DKVA_SEQ; t++)
        for (INT h = 0; h < DKVA_NH; h++) {
            p->attn_sum[t][h] = (float)((seed * 7 + t * 2 + h) % 5 + 1);
            for (INT d = 0; d < DKVA_DH; d++)
                p->partial_out[t][h][d] =
                    (float)((seed * 3 + t + h * 2 + d) % 7 + 1);
        }
}

static INT st_eq(const DKVA_RESP_PKT *a, const DKVA_RESP_PKT *b)
{
    for (INT t = 0; t < DKVA_SEQ; t++)
        for (INT h = 0; h < DKVA_NH; h++) {
            if (a->attn_sum[t][h] != b->attn_sum[t][h]) return 0;
            for (INT d = 0; d < DKVA_DH; d++)
                if (a->partial_out[t][h][d] != b->partial_out[t][h][d]) return 0;
        }
    return 1;
}

INT dkva_self_test(void)
{
    /* test-only バッファ (タスクスタックを汚さない) */
    static DKVA_RESP_PKT s0, s1, s2;
    static DKVA_RESP_PKT accA, accB, refA, refB;
    INT fails = 0;

    dk_puts("[g13-parallel] ==== §5 concurrent-aggregation property tests ====\r\n");

    /* --- P1: 順序非依存 ------------------------------------------------ */
    st_fill(&s0, 1, 1); st_fill(&s1, 1, 2); st_fill(&s2, 1, 3);
    st_zero(&accA); st_zero(&accB);
    accumulate_pkt(&accA, &s0); accumulate_pkt(&accA, &s1); accumulate_pkt(&accA, &s2);
    accumulate_pkt(&accB, &s2); accumulate_pkt(&accB, &s0); accumulate_pkt(&accB, &s1);
    if (st_eq(&accA, &accB)) dk_puts("[g13-parallel] P1 order-independent  : PASS\r\n");
    else { fails++; dk_puts("[g13-parallel] P1 order-independent  : FAIL\r\n"); }

    /* --- P2: origin 非汚染 (交互に集約しても混ざらない) ---------------- */
    /* origin A = {seed 10, 11}, origin B = {seed 20, 21}. 独立した参照和。 */
    st_zero(&refA); st_fill(&s0, 1, 10); st_fill(&s1, 1, 11);
    accumulate_pkt(&refA, &s0); accumulate_pkt(&refA, &s1);
    st_zero(&refB); st_fill(&s0, 2, 20); st_fill(&s1, 2, 21);
    accumulate_pkt(&refB, &s0); accumulate_pkt(&refB, &s1);

    st_zero(&accA); st_zero(&accB);
    /* インターリーブ畳み込み (responder が A/B を並行に進めるのを模す) */
    st_fill(&s0, 1, 10); accumulate_pkt(&accA, &s0);
    st_fill(&s0, 2, 20); accumulate_pkt(&accB, &s0);
    st_fill(&s0, 1, 11); accumulate_pkt(&accA, &s0);
    st_fill(&s0, 2, 21); accumulate_pkt(&accB, &s0);
    if (st_eq(&accA, &refA) && st_eq(&accB, &refB) && !st_eq(&accA, &accB))
        dk_puts("[g13-parallel] P2 origin-isolated    : PASS\r\n");
    else { fails++; dk_puts("[g13-parallel] P2 origin-isolated    : FAIL\r\n"); }

    /* --- P3: 同時数不変 (他 origin が何件並行しても A の結果は不変) ---- */
    /* A を「単独」で集約 → resultA. 次に B/C を同時に走らせながら A を集約。 */
    st_zero(&accA);
    st_fill(&s0, 1, 30); accumulate_pkt(&accA, &s0);
    st_fill(&s1, 1, 31); accumulate_pkt(&accA, &s1);
    /* now: A interleaved with two other concurrent origins B,C */
    st_zero(&accB);   /* reuse accB as "A under concurrency" */
    st_zero(&refB);   /* reuse refB/refA as throwaway B,C accumulators */
    st_zero(&refA);
    st_fill(&s0, 1, 30); accumulate_pkt(&accB, &s0);
    st_fill(&s2, 2, 99); accumulate_pkt(&refB, &s2);   /* B noise */
    st_fill(&s2, 3, 77); accumulate_pkt(&refA, &s2);   /* C noise */
    st_fill(&s1, 1, 31); accumulate_pkt(&accB, &s1);
    st_fill(&s2, 2, 98); accumulate_pkt(&refB, &s2);   /* more B noise */
    if (st_eq(&accA, &accB))
        dk_puts("[g13-parallel] P3 concurrency-invariant: PASS\r\n");
    else { fails++; dk_puts("[g13-parallel] P3 concurrency-invariant: FAIL\r\n"); }

    /* --- 構造: per-origin スロットが DNODE_MAX 本ある (単一共有窓でない) - */
    if ((INT)(sizeof(cagg) / sizeof(cagg[0])) == DNODE_MAX)
        dk_puts("[g13-parallel] P4 per-origin slots    : PASS\r\n");
    else { fails++; dk_puts("[g13-parallel] P4 per-origin slots    : FAIL\r\n"); }

    if (fails == 0) dk_puts("[g13-parallel] PASS\r\n");
    else { dk_puts("[g13-parallel] FAIL  failures="); dk_putdec((UW)fails);
           dk_puts("\r\n"); }
    return fails;
}

/* ------------------------------------------------------------------ */
/* 統計表示                                                            */
/* ------------------------------------------------------------------ */

void dkva_stat(void)
{
    dk_puts("[dkva] kv_cache  : "); dk_putdec((UW)kv_count);
    dk_puts("/"); dk_putdec(DKVA_CACHE_SIZE); dk_puts(" entries\r\n");
    dk_puts("[dkva] req_sent  : "); dk_putdec(stat_req_sent);  dk_puts("\r\n");
    dk_puts("[dkva] resp_got  : "); dk_putdec(stat_resp_got);  dk_puts("\r\n");
    dk_puts("[dkva] resp_sent : "); dk_putdec(stat_resp_sent); dk_puts("\r\n");
    dk_puts("[dkva] timeout   : "); dk_putdec(stat_timeout);   dk_puts("\r\n");
    dk_puts("[dkva] degraded  : "); dk_putdec(stat_degraded);  dk_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* シェルコマンド: どのノードからでも分散 attention を発行できる        */
/*                                                                     */
/*   dkva                  → 統計表示 (dkva_stat)                       */
/*   dkva infer [a b c d]  → 4 つの int8 値から決定論的に Q を合成し    */
/*                           dkva_infer を「このノードを起点に」実行    */
/*                                                                     */
/* dtr.c の FULL 経路は偶数 node id だけを requester にする heuristic   */
/* を持つが、このコマンドはノード ID に一切依存しない。同じ引数なら     */
/* どのノードから発行しても同じ問い (同じ Q) になるので、「起点が死ん   */
/* でも生き残りが同じ問いを発行して完遂できる」ことをそのまま示せる     */
/* (survival, wave 8 — 起点はただの呼び出し元であり特権ではない)。      */
/* ------------------------------------------------------------------ */

static UW dkva_cmd_seq = 0;   /* dtr_req_counter (小さい整数) と衝突しない
                               * よう 9,000,000 番台を使う           */

/* [*pp, end) から 10 進整数を 1 つ読む。読めたら 1。 */
static INT dk_parse_int(const UB **pp, const UB *end, INT *out)
{
    const UB *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    INT neg = 0;
    if (p < end && *p == '-') { neg = 1; p++; }
    if (p >= end || *p < '0' || *p > '9') return 0;
    INT v = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (INT)(*p - '0'); p++; }
    *out = neg ? -v : v;
    *pp  = p;
    return 1;
}

void dkva_cmd(const UB *args, UW len)
{
    const UB *p   = args;
    const UB *end = args + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;

    /* verb "test" — G13 同時集約プロパティ自己テスト (純ローカル, mesh 不要) */
    {
        static const char tverb[] = "test";
        INT ti = 0;
        while (tverb[ti] && p + ti < end && (char)p[ti] == tverb[ti]) ti++;
        if (tverb[ti] == '\0') { dkva_self_test(); return; }
    }

    /* verb "infer" 以外は統計表示 */
    static const char verb[] = "infer";
    INT vi = 0;
    while (verb[vi] && p + vi < end && (char)p[vi] == verb[vi]) vi++;
    if (verb[vi] != '\0') { dkva_stat(); return; }
    p += 5;

    if (drpc_my_node == 0xFF) {
        dk_puts("[dkva-cmd] mesh not up (run 'net' first)\r\n");
        return;
    }

    INT v[4] = { 40, 80, 30, 10 };        /* default demo sensor vector */
    for (INT i = 0; i < 4; i++) {
        INT x;
        if (!dk_parse_int(&p, end, &x)) break;
        if (x >  127) x =  127;
        if (x < -128) x = -128;
        v[i] = x;
    }

    /* Q を引数から決定論的に合成: 同じ引数 → 同じ問い。どのノードが
     * 発行しても同一になるよう、ノード ID は混ぜない。 */
    static float Q[DKVA_SEQ][DKVA_NH][DKVA_DH];
    for (INT t = 0; t < DKVA_SEQ; t++)
        for (INT h = 0; h < DKVA_NH; h++)
            for (INT d = 0; d < DKVA_DH; d++) {
                INT m = v[(t + h + d) & 3];
                Q[t][h][d] =
                    (float)((m * (t + 1) + 7 * h + 3 * d) % 23 - 11) / 11.0f;
            }

    /* W_o = 単位行列: attention 出力をそのまま観測する */
    static float W_o[DKVA_DM][DKVA_DM];
    for (INT i = 0; i < DKVA_DM; i++)
        for (INT j = 0; j < DKVA_DM; j++)
            W_o[i][j] = (i == j) ? 1.0f : 0.0f;

    UW req = 9000000UL + (UW)drpc_my_node * 10000UL + (++dkva_cmd_seq);

    dk_puts("[dkva-cmd] infer from node "); dk_putdec(drpc_my_node);
    dk_puts("  req="); dk_putdec(req); dk_puts("\r\n");

    static float out[DKVA_SEQ][DKVA_DM];
    ER er = dkva_infer(Q, W_o, out, req);
    if (er != E_OK) {
        dk_puts("[dkva-cmd] => FAILED (E_TMOUT: no remote contribution)  req=");
        dk_putdec(req); dk_puts("\r\n");
        return;
    }

    /* 出力指紋: Σ|out| を 100 倍した整数。完遂の機械検証用。 */
    float acc = 0.0f;
    for (INT t = 0; t < DKVA_SEQ; t++)
        for (INT m = 0; m < DKVA_DM; m++)
            acc += out[t][m] >= 0.0f ? out[t][m] : -out[t][m];
    dk_puts("[dkva-cmd] => OK  req="); dk_putdec(req);
    dk_puts("  fp="); dk_putdec((UW)(acc * 100.0f)); dk_puts("\r\n");
}
