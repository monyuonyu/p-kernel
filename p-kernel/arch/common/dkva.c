/*
 *  dkva.c (x86)
 *  Phase 10 — Distributed KV Attention
 *
 *  【動作フロー】
 *
 *  Requester (node0, FULL mode, dkva_infer から):
 *    1. DKVA_Q_PKT を "dtr/dkva/q" へ pub
 *    2. 各ノードの "dtr/dkva/resp/<node>" を順にポーリングして
 *       DKVA_RESP_PKT を収集 (per-source topic なので peer ごとに独立)
 *    3. partial_out を集約して mhsa_out を計算
 *
 *  Responder (node1,2,..., dkva_task から):
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

static float dk_exp(float x)
{
    if (x >  10.0f) return 22026.0f;
    if (x < -10.0f) return 0.0f;
    float r = 1.0f + x * (1.0f + x * (0.5f + x * (0.16667f +
              x * (0.04167f + x * (0.00833f + x * 0.00139f)))));
    return r < 1e-10f ? 1e-10f : r;
}

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

/* ------------------------------------------------------------------ */
/* K-DDS ハンドル                                                     */
/* ------------------------------------------------------------------ */

static W h_q_pub    = -1;
static W h_q_sub    = -1;
/* per-source response topics: h_resp_pub[n] / h_resp_sub[n] は
 * "dtr/dkva/resp/<n>" を指す。responder は自ノード (drpc_my_node) の
 * pub ハンドルへ発行し、requester は自分以外の全 sub ハンドルから収集する。
 * dkva_init は drpc_my_node が確定する前 (boot 時) に走るので、全 DNODE_MAX
 * 分のトピックを開いておき、実行時にノード ID で選ぶ。 */
static W h_resp_pub[DNODE_MAX];
static W h_resp_sub[DNODE_MAX];

/* "dtr/dkva/resp/<node>" を out に組み立てる (node は 0..DNODE_MAX-1, 1 桁) */
static void resp_topic_name(char *out, UB node)
{
    const char *p = DKVA_TOPIC_RESP_PFX;
    INT i = 0;
    while (p[i]) { out[i] = p[i]; i++; }
    out[i++] = (char)('0' + node);
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

/* ------------------------------------------------------------------ */
/* Requester: 各ノードの partial を集約して mhsa_out を計算          */
/* ------------------------------------------------------------------ */

ER dkva_infer(const float Q[DKVA_SEQ][DKVA_NH][DKVA_DH],
              const float W_o[DKVA_DM][DKVA_DM],
              float mhsa_out[DKVA_SEQ][DKVA_DM],
              UW req_id)
{
    if (h_q_pub < 0) return E_NOEXS;

    /* Query パケット送信 */
    DKVA_Q_PKT qpkt = { 0 };
    qpkt.magic    = DKVA_Q_MAGIC;
    qpkt.req_id   = req_id;
    qpkt.src_node = drpc_my_node;
    qpkt.n_cached = DKVA_CACHE_SIZE;
    for (INT t = 0; t < DKVA_SEQ; t++)
        for (INT h = 0; h < DKVA_NH; h++)
            for (INT d = 0; d < DKVA_DH; d++)
                qpkt.Q[t][h][d] = Q[t][h][d];

    kdds_pub(h_q_pub, &qpkt, (W)sizeof(qpkt));
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

    /* 自分のキャッシュも含める */
    {
        DKVA_RESP_PKT self_resp = { 0 };
        self_resp.req_id = req_id;
        compute_partial(&qpkt, &self_resp);
        for (INT t = 0; t < DKVA_SEQ; t++)
            for (INT h = 0; h < DKVA_NH; h++) {
                total_sum[t][h] += self_resp.attn_sum[t][h];
                for (INT d = 0; d < DKVA_DH; d++)
                    total_out[t][h][d] += self_resp.partial_out[t][h][d];
            }
    }

    /* 他ノードからのレスポンスを per-source トピックから収集。
     * 各ノードは "dtr/dkva/resp/<node>" という専用トピックへ pub するので、
     * requester はノードごとの sub ハンドルを順に poll するだけでよい。
     * K-DDS LATEST_ONLY はラッチした最新値を毎ポール再配信するため、
     * 一度集約したノードは got[] でスキップして重複加算を防ぐ。 */
    INT  tmo_left = DKVA_INFER_TMO;
    INT  resp_cnt = 0;
    UB   got[DNODE_MAX];
    for (UB i = 0; i < DNODE_MAX; i++) got[i] = 0;
    while (tmo_left > 0) {
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n == drpc_my_node || got[n] || h_resp_sub[n] < 0) continue;

            DKVA_RESP_PKT rp = { 0 };
            W r = kdds_sub(h_resp_sub[n], &rp, (W)sizeof(rp), 0);
            if (r >= (W)sizeof(DKVA_RESP_PKT) &&
                rp.magic == DKVA_RESP_MAGIC && rp.req_id == req_id &&
                rp.src_node == n) {
                got[n] = 1;
                resp_cnt++;
                stat_resp_got++;
                for (INT t = 0; t < DKVA_SEQ; t++)
                    for (INT h = 0; h < DKVA_NH; h++) {
                        total_sum[t][h] += rp.attn_sum[t][h];
                        for (INT d = 0; d < DKVA_DH; d++)
                            total_out[t][h][d] += rp.partial_out[t][h][d];
                    }
                dk_puts("[dkva] resp from node "); dk_putdec(rp.src_node);
                dk_puts("  entries="); dk_putdec(rp.n_entries); dk_puts("\r\n");
            }
        }
        tk_dly_tsk(20);
        tmo_left -= 20;
    }

    if (resp_cnt == 0) {
        stat_timeout++;
        dk_puts("[dkva] timeout: no remote resp\r\n");
        return E_TMOUT;
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

    dk_puts("[dkva] aggregated "); dk_putdec((UW)resp_cnt);
    dk_puts(" peers  req="); dk_putdec(req_id); dk_puts("\r\n");
    return E_OK;
}

/* ------------------------------------------------------------------ */
/* Responder タスク: dtr_task とは別タスクで回す                      */
/* ------------------------------------------------------------------ */

void dkva_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    if (drpc_my_node == 0xFF) return;

    /* node0 は Requester なので Responder は不要 (兼任も可) */
    /* 全ノードが Responder として動作する */

    dk_puts("[dkva] responder task started  node=");
    dk_putdec(drpc_my_node); dk_puts("\r\n");

    /* KV キャッシュ warmup: 新規 FULL クラスタでは全ノードのキャッシュが空で
     * partial Attention が自明 (entries=0) になるため、自ノード固有の合成入力で
     * seed しておく。dkva_task は drpc_my_node 確定後に走るので node 固有値が使える。 */
    dtr_seed_kv_cache(drpc_my_node);
    dk_puts("[dkva] kv-cache seeded for node="); dk_putdec(drpc_my_node);
    dk_puts("\r\n");

    /* Last req_id answered per requesting node. K-DDS LATEST_ONLY QoS
     * re-delivers the latched Q on every poll; respond only to a new
     * req_id, else dkva_infer (which SUMS responses) would double-count. */
    static UW last_resp_req[DNODE_MAX];
    for (UB i = 0; i < DNODE_MAX; i++) last_resp_req[i] = 0;

    for (;;) {
        DKVA_Q_PKT qpkt = { 0 };
        W r = kdds_sub(h_q_sub, &qpkt, (W)sizeof(qpkt), 0);

        if (r >= (W)sizeof(DKVA_Q_PKT) && qpkt.magic == DKVA_Q_MAGIC) {
            /* 自分宛ではない (自分が送ったもの) / 範囲外はスキップ */
            if (qpkt.src_node == drpc_my_node ||
                qpkt.src_node >= DNODE_MAX) {
                tk_dly_tsk(10);
                continue;
            }
            /* 同じ要求への重複応答を抑止 (集約の多重カウント防止) */
            if (qpkt.req_id == last_resp_req[qpkt.src_node]) {
                tk_dly_tsk(10);
                continue;
            }
            last_resp_req[qpkt.src_node] = qpkt.req_id;

            DKVA_RESP_PKT resp = { 0 };
            resp.magic    = DKVA_RESP_MAGIC;
            resp.req_id   = qpkt.req_id;
            resp.src_node = drpc_my_node;

            compute_partial(&qpkt, &resp);
            if (drpc_my_node < DNODE_MAX && h_resp_pub[drpc_my_node] >= 0) {
                kdds_pub(h_resp_pub[drpc_my_node], &resp, (W)sizeof(resp));
                stat_resp_sent++;
            }

            dk_puts("[dkva] responded to node "); dk_putdec(qpkt.src_node);
            dk_puts("  req="); dk_putdec(qpkt.req_id);
            dk_puts("  entries="); dk_putdec(resp.n_entries); dk_puts("\r\n");
        }

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
    for (UB n = 0; n < DNODE_MAX; n++) { h_resp_pub[n] = -1; h_resp_sub[n] = -1; }

    /* DKVA トピックは REGION スコープ (regions R2 入口)。Q ブロードキャストも
     * per-source レスポンスも自 region 内に閉じるので、分散 Attention は
     * 「近いノード群 = 1つの脳半球」の集合記憶で計算され、遠い region の
     * partial を待たずに済む (region 間は将来 coordinator が疎に再集約)。 */
    h_q_pub    = kdds_open_scoped(DKVA_TOPIC_Q, KDDS_QOS_LATEST_ONLY,
                                  KDDS_SCOPE_REGION);
    h_q_sub    = kdds_open_scoped(DKVA_TOPIC_Q, KDDS_QOS_LATEST_ONLY,
                                  KDDS_SCOPE_REGION);

    /* per-source レスポンストピックを全ノード分開く (drpc_my_node が未確定の
     * 段階なのでどれが自分のものか決め打ちできない)。responder/requester は
     * 実行時にノード ID で pub/sub ハンドルを選ぶ。 */
    for (UB n = 0; n < DNODE_MAX; n++) {
        char tn[KDDS_NAME_MAX];
        resp_topic_name(tn, n);
        h_resp_pub[n] = kdds_open_scoped(tn, KDDS_QOS_LATEST_ONLY,
                                         KDDS_SCOPE_REGION);
        h_resp_sub[n] = kdds_open_scoped(tn, KDDS_QOS_LATEST_ONLY,
                                         KDDS_SCOPE_REGION);
    }

    dk_puts("[dkva] initialized (region-scoped)  cache="); dk_putdec(DKVA_CACHE_SIZE);
    dk_puts("  topics: "); dk_puts(DKVA_TOPIC_Q);
    dk_puts(" / "); dk_puts(DKVA_TOPIC_RESP_PFX); dk_puts("<node> x");
    dk_putdec((UW)DNODE_MAX); dk_puts("\r\n");
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
}
