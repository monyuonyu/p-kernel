/*
 *  dtr.c (x86)
 *  Phase 8/11 — Distributed Transformer Inference
 *
 *  本物の Transformer Block (MHSA + LayerNorm + FFN + LayerNorm) を
 *  クラスタ規模に応じた分散戦略で実行する。
 *
 *  モデル構造:
 *    Input int8[4]
 *    → Embed  : 各センサー値を 1 トークンとして float[4][8] に変換
 *    → MHSA   : Multi-Head Self-Attention (h=2, d_k=d_v=4)
 *               + 残差接続 + LayerNorm (LN1)
 *    → FFN    : Linear(8→16) + ReLU + Linear(16→8)
 *               + 残差接続 + LayerNorm (LN2)
 *    → Pool   : Mean Pooling float[4][8] → float[8]
 *    → Cls    : Linear(8→3) + Softmax → class [0,1,2]
 *
 *  分散戦略 (縮退モードと連携):
 *
 *    SOLO    (1 node):  全ステージをローカルで実行
 *
 *    REDUCED (2 nodes): Tensor Parallel — Attention ヘッドを分割
 *      Node 0 (even): head0 計算 → "dtr/input" pub, "dtr/head1" 待機
 *                     → gather → W_o → LN1 → FFN → LN2 → Pool → Cls
 *      Node 1 (odd) : "dtr/input" sub → head1 計算 → "dtr/head1" pub
 *
 *    FULL    (3+ nodes): Pipeline Parallel — ステージをノード間で分割
 *      Node 0 (even): Embed + MHSA(local) + mean-pool → "dtr/l0" pub
 *      Node 1 (odd) : "dtr/l0" sub → LN1 → FFN → LN2 → Cls → "dtr/result" pub
 */

#include "dtr.h"
#include "kdds.h"
#include "drpc.h"
#include "degrade.h"
#include "kernel.h"
#include <tmonitor.h>

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void dt_puts(const char *s) { tm_putstring((UB *)s); }

static void dt_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { dt_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    dt_puts(&buf[i]);
}

static void dt_putf2(float f)
{
    if (f < 0.0f) { dt_puts("-"); f = -f; }
    UW ii = (UW)f;
    UW fr = (UW)((f - (float)ii) * 100.0f);
    dt_putdec(ii); dt_puts(".");
    if (fr < 10) dt_puts("0");
    dt_putdec(fr);
}

/* ------------------------------------------------------------------ */
/* 数学ヘルパー (libc 不使用)                                         */
/* ------------------------------------------------------------------ */

static float dt_relu(float x) { return x > 0.0f ? x : 0.0f; }

/* exp(x): Horner 法 Taylor 展開 */
static float dt_exp(float x)
{
    if (x >  10.0f) return 22026.0f;
    if (x < -10.0f) return 0.0f;
    float r = 1.0f + x * (1.0f + x * (0.5f + x * (0.16667f +
              x * (0.04167f + x * (0.00833f + x * 0.00139f)))));
    return r < 1e-10f ? 1e-10f : r;
}

/* sqrt(x): Newton-Raphson 法 */
static float dt_sqrt(float x)
{
    if (x <= 0.0f) return 0.0f;
    float r = x > 1.0f ? x * 0.5f : 1.0f;
    r = (r + x / r) * 0.5f;
    r = (r + x / r) * 0.5f;
    r = (r + x / r) * 0.5f;
    r = (r + x / r) * 0.5f;
    return r;
}

/* y[M] = W[M×N] · x[N] + b[M] */
static void dt_linear(const float *W, const float *b,
                      const float *x, float *y, INT M, INT N)
{
    for (INT m = 0; m < M; m++) {
        float s = b ? b[m] : 0.0f;
        for (INT n = 0; n < N; n++) s += W[m * N + n] * x[n];
        y[m] = s;
    }
}

/* softmax in-place */
static void dt_softmax(float *x, INT n)
{
    float maxv = x[0];
    for (INT i = 1; i < n; i++) if (x[i] > maxv) maxv = x[i];
    float sum = 0.0f;
    for (INT i = 0; i < n; i++) { x[i] = dt_exp(x[i] - maxv); sum += x[i]; }
    if (sum < 1e-10f) sum = 1e-10f;
    for (INT i = 0; i < n; i++) x[i] /= sum;
}

/* Layer Normalization: (x - mean) / sqrt(var + eps) * gamma + beta */
static void dt_layernorm(float *x, const float *gamma, const float *beta, INT n)
{
    float mean = 0.0f, var = 0.0f;
    for (INT i = 0; i < n; i++) mean += x[i];
    mean /= (float)n;
    for (INT i = 0; i < n; i++) { float d = x[i] - mean; var += d * d; }
    var /= (float)n;
    float inv_std = 1.0f / dt_sqrt(var + 1e-5f);
    for (INT i = 0; i < n; i++)
        x[i] = (x[i] - mean) * inv_std * gamma[i] + beta[i];
}

/* ------------------------------------------------------------------ */
/* 次元定義 (旧モデルとの互換用エイリアスを残す)                     */
/* ------------------------------------------------------------------ */

#define SEQ  DTR_SEQ_LEN    /* 4  */
#define DM   DTR_EMBED_DIM  /* 8  */
#define DH   DTR_D_HEAD     /* 4  */
#define NH   DTR_NUM_HEADS  /* 2  */
#define DFFN DTR_FFN_DIM    /* 16 */
#define DOUT DTR_OUT_DIM    /* 3  */

/* 旧 Stage 定義 (Pipeline Parallel の dtr/l0 互換) */
#define EMB_IN   4
#define EMB_OUT  DM
#define L0_IN    DM
#define L0_OUT   DM
#define L1A_OUT  DFFN
#define L1B_OUT  DM
#define OUT_IN   DM
#define OUT_OUT  DOUT

/* ------------------------------------------------------------------ */
/* モデル重み                                                          */
/* ------------------------------------------------------------------ */

/* --- Embed (旧実装と同じ: 各センサー値をDMに投影) --- */
static float W_emb[EMB_OUT][EMB_IN];
static float b_emb[EMB_OUT];

/* --- Multi-Head Self-Attention 重み --- */
/* W_q/k/v[head][d_model → d_head]: 投影行列 */
static float W_q[NH][DM][DH];   /* 2×8×4 = 64 floats */
static float W_k[NH][DM][DH];   /* 2×8×4 = 64 floats */
static float W_v[NH][DM][DH];   /* 2×8×4 = 64 floats */
/* W_o: concat(heads) → d_model (DM×DM = 8×8 = 64 floats) */
static float W_o[DM][DM];

/* --- LayerNorm パラメータ (gamma=1, beta=0 で初期化) --- */
static float ln1_g[DM], ln1_b[DM];   /* MHSA 後 */
static float ln2_g[DM], ln2_b[DM];   /* FFN  後 */

/* --- FFN 重み --- */
static float W_ffn1[DFFN][DM];   /* 16×8 = 128 floats */
static float b_ffn1[DFFN];
static float W_ffn2[DM][DFFN];   /* 8×16 = 128 floats */
static float b_ffn2[DM];

/* --- 分類ヘッド --- */
static float W_cls[DOUT][DM];    /* 3×8 = 24 floats */
static float b_cls[DOUT];

/* ------------------------------------------------------------------ */
/* 重み初期化ヘルパー (LCG 疑似乱数 He 初期化)                       */
/* ------------------------------------------------------------------ */

static void init_weights(float *w, INT n, float scale, UW *seed)
{
    for (INT i = 0; i < n; i++) {
        *seed = *seed * 1664525UL + 1013904223UL;
        float v = (float)((*seed >> 9) & 0x7FFFFFU) / (float)(1 << 23) * 2.0f - 1.0f;
        w[i] = v * scale;
    }
}

static void init_const(float *w, INT n, float val)
{
    for (INT i = 0; i < n; i++) w[i] = val;
}

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

DTR_STATS dtr_stats;

static ID  dtr_result_sem  = -1;
static DTR_RESULT dtr_last_result;
static UW  dtr_req_counter = 0;

/* K-DDS ハンドル */
/* Pipeline Parallel 用 (FULL mode) */
static W h_l0_pub  = -1;
static W h_l0_sub  = -1;
static W h_res_pub = -1;
static W h_res_sub = -1;

/* Tensor Parallel 用 (REDUCED mode) */
static W h_input_pub  = -1;   /* Node0: "dtr/input" pub  */
static W h_input_sub  = -1;   /* Node1: "dtr/input" sub  */
static W h_head1_pub  = -1;   /* Node1: "dtr/head1" pub  */
static W h_head1_sub  = -1;   /* Node0: "dtr/head1" sub  */

/* ------------------------------------------------------------------ */
/* Transformer 計算関数                                               */
/* ------------------------------------------------------------------ */

/*
 *  Embed: int8[SEQ] → float[SEQ][DM]
 *  各トークンを W_emb で d_model 次元に投影し ReLU
 */
static void run_embed_seq(const B input[SEQ], float tok[SEQ][DM])
{
    for (INT t = 0; t < SEQ; t++) {
        float in_f = (float)input[t] / 127.0f;
        for (INT d = 0; d < DM; d++) {
            float s = b_emb[d];
            s += W_emb[d][0] * in_f;   /* 各センサー値は独立したトークン */
            /* 残り3入力は0 (パディング) */
            for (INT k = 1; k < EMB_IN; k++) s += W_emb[d][k] * 0.0f;
            tok[t][d] = dt_relu(s);
        }
    }
}

/*
 *  Scaled Dot-Product Attention (1 head):
 *    Q[SEQ][DH] = tok[SEQ][DM] · W_q[h]^T
 *    K[SEQ][DH] = tok[SEQ][DM] · W_k[h]^T
 *    V[SEQ][DH] = tok[SEQ][DM] · W_v[h]^T
 *    Attn = softmax(Q·K^T / sqrt(DH)) · V → out[SEQ][DH]
 */
static void run_attn_head(const float tok[SEQ][DM], INT h,
                          float out[SEQ][DH])
{
    float Q[SEQ][DH], K[SEQ][DH], V[SEQ][DH];
    float scale = 1.0f / dt_sqrt((float)DH);

    /* Q, K, V 投影 */
    for (INT t = 0; t < SEQ; t++) {
        dt_linear((float *)W_q[h], NULL, tok[t], Q[t], DH, DM);
        dt_linear((float *)W_k[h], NULL, tok[t], K[t], DH, DM);
        dt_linear((float *)W_v[h], NULL, tok[t], V[t], DH, DM);
    }

    /* Attention スコア: attn_w[SEQ][SEQ] */
    float attn_w[SEQ][SEQ];
    for (INT i = 0; i < SEQ; i++) {
        for (INT j = 0; j < SEQ; j++) {
            float s = 0.0f;
            for (INT d = 0; d < DH; d++) s += Q[i][d] * K[j][d];
            attn_w[i][j] = s * scale;
        }
        dt_softmax(attn_w[i], SEQ);
    }

    /* Attention 出力: out[SEQ][DH] = attn_w · V */
    for (INT i = 0; i < SEQ; i++) {
        for (INT d = 0; d < DH; d++) {
            float s = 0.0f;
            for (INT j = 0; j < SEQ; j++) s += attn_w[i][j] * V[j][d];
            out[i][d] = s;
        }
    }
}

/*
 *  Multi-Head Self-Attention (全ヘッドをローカルで計算):
 *    各ヘッドの出力 [SEQ][DH] を concat → [SEQ][DM]
 *    → W_o で投影 → mhsa_out[SEQ][DM]
 */
static void run_mhsa_local(const float tok[SEQ][DM],
                           float mhsa_out[SEQ][DM])
{
    float head_out[NH][SEQ][DH];

    for (INT h = 0; h < NH; h++)
        run_attn_head(tok, h, head_out[h]);

    /* concat(heads) = [SEQ][DM], W_o 投影 */
    for (INT t = 0; t < SEQ; t++) {
        float concat[DM];
        for (INT h = 0; h < NH; h++)
            for (INT d = 0; d < DH; d++)
                concat[h * DH + d] = head_out[h][t][d];
        dt_linear((float *)W_o, NULL, concat, mhsa_out[t], DM, DM);
    }

    dtr_stats.attn_runs++;
}

/*
 *  FFN on sequence: [SEQ][DM] → [SEQ][DM]
 *  各トークンに独立して適用
 */
static void run_ffn_seq(const float in[SEQ][DM], float out[SEQ][DM])
{
    for (INT t = 0; t < SEQ; t++) {
        float mid[DFFN];
        dt_linear((float *)W_ffn1, b_ffn1, in[t], mid, DFFN, DM);
        for (INT d = 0; d < DFFN; d++) mid[d] = dt_relu(mid[d]);
        dt_linear((float *)W_ffn2, b_ffn2, mid, out[t], DM, DFFN);
    }
    dtr_stats.layer1_runs++;
}

/*
 *  Mean Pooling: float[SEQ][DM] → float[DM]
 */
static void run_mean_pool(const float seq[SEQ][DM], float out[DM])
{
    for (INT d = 0; d < DM; d++) {
        float s = 0.0f;
        for (INT t = 0; t < SEQ; t++) s += seq[t][d];
        out[d] = s / (float)SEQ;
    }
}

/*
 *  分類ヘッド: float[DM] → class [0,1,2]
 */
static UB run_cls_head(const float vec[DM], float scores[DOUT])
{
    dt_linear((float *)W_cls, b_cls, vec, scores, DOUT, DM);
    dt_softmax(scores, DOUT);
    UB cls = 0;
    for (INT i = 1; i < DOUT; i++)
        if (scores[i] > scores[cls]) cls = (UB)i;
    dtr_stats.output_runs++;
    return cls;
}

/*
 *  Transformer Block をローカルで全実行 (SOLO モード)
 *    input[4] → class [0,1,2]
 */
static UB run_transformer_local(const B input[SEQ],
                                float scores_out[DOUT])
{
    /* 1. Embed */
    float tok[SEQ][DM];
    run_embed_seq(input, tok);
    dtr_stats.layer0_runs++;

    /* 2. MHSA + 残差 + LN1 */
    float mhsa[SEQ][DM];
    run_mhsa_local(tok, mhsa);
    for (INT t = 0; t < SEQ; t++) {
        for (INT d = 0; d < DM; d++) mhsa[t][d] += tok[t][d]; /* 残差 */
        dt_layernorm(mhsa[t], ln1_g, ln1_b, DM);
    }

    /* 3. FFN + 残差 + LN2 */
    float ffn[SEQ][DM];
    run_ffn_seq(mhsa, ffn);
    for (INT t = 0; t < SEQ; t++) {
        for (INT d = 0; d < DM; d++) ffn[t][d] += mhsa[t][d]; /* 残差 */
        dt_layernorm(ffn[t], ln2_g, ln2_b, DM);
    }

    /* 4. Mean Pool + Cls */
    float pool[DM];
    run_mean_pool(ffn, pool);
    return run_cls_head(pool, scores_out);
}

/* ------------------------------------------------------------------ */
/* 旧 Stage 互換関数 (Pipeline Parallel / dtr_stat 用)               */
/* ------------------------------------------------------------------ */

static void run_stage0(const B input[4], float out[L0_OUT])
{
    float tok[SEQ][DM];
    run_embed_seq(input, tok);
    dtr_stats.layer0_runs++;

    float mhsa[SEQ][DM];
    run_mhsa_local(tok, mhsa);
    for (INT t = 0; t < SEQ; t++) {
        for (INT d = 0; d < DM; d++) mhsa[t][d] += tok[t][d];
        dt_layernorm(mhsa[t], ln1_g, ln1_b, DM);
    }
    /* mean pool → [DM] として出力 */
    run_mean_pool(mhsa, out);
}

static UB run_stage12(const float in[DM], float scores[DOUT])
{
    /* in = mean-pooled MHSA 出力 → FFN (簡易: per-vector) + Cls */
    float mid[DFFN];
    dt_linear((float *)W_ffn1, b_ffn1, in, mid, DFFN, DM);
    for (INT d = 0; d < DFFN; d++) mid[d] = dt_relu(mid[d]);
    float ffn[DM];
    dt_linear((float *)W_ffn2, b_ffn2, mid, ffn, DM, DFFN);
    float ln[DM];
    for (INT d = 0; d < DM; d++) ln[d] = ffn[d] + in[d];
    dt_layernorm(ln, ln2_g, ln2_b, DM);
    dtr_stats.layer1_runs++;
    return run_cls_head(ln, scores);
}

/* ------------------------------------------------------------------ */
/* dtr_init                                                            */
/* ------------------------------------------------------------------ */

void dtr_init(void)
{
    UW seed = 0xDEAD8888UL;

    /* Embed */
    init_weights((float *)W_emb, EMB_OUT * EMB_IN, 0.707f, &seed);
    init_const  (b_emb, EMB_OUT, 0.0f);

    /* MHSA 重み (He 初期化, scale=1/sqrt(DM)) */
    float attn_scale = 0.354f;  /* 1/sqrt(8) ≈ 0.354 */
    for (INT h = 0; h < NH; h++) {
        init_weights((float *)W_q[h], DM * DH, attn_scale, &seed);
        init_weights((float *)W_k[h], DM * DH, attn_scale, &seed);
        init_weights((float *)W_v[h], DM * DH, attn_scale, &seed);
    }
    init_weights((float *)W_o, DM * DM, attn_scale, &seed);

    /* LayerNorm: gamma=1, beta=0 */
    init_const(ln1_g, DM, 1.0f); init_const(ln1_b, DM, 0.0f);
    init_const(ln2_g, DM, 1.0f); init_const(ln2_b, DM, 0.0f);

    /* FFN */
    init_weights((float *)W_ffn1, DFFN * DM,  0.500f, &seed);
    init_const  (b_ffn1, DFFN, 0.0f);
    init_weights((float *)W_ffn2, DM * DFFN,  0.354f, &seed);
    init_const  (b_ffn2, DM, 0.0f);

    /* 分類ヘッド */
    init_weights((float *)W_cls, DOUT * DM,   0.500f, &seed);
    init_const  (b_cls, DOUT, 0.0f);

    /* 分散推論セマフォ */
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
    dtr_result_sem = tk_cre_sem(&cs);

    INT total_params = EMB_OUT*EMB_IN + NH*(DM*DH*3) + DM*DM +
                       DFFN*DM + DM*DFFN + DOUT*DM;

    dt_puts("[dtr] Transformer initialized\r\n");
    dt_puts("[dtr]   arch  : Embed(4tok×8) + MHSA(h=2,dk=4) + FFN(16) + Cls(3)\r\n");
    dt_puts("[dtr]   params: "); dt_putdec((UW)total_params); dt_puts(" floats\r\n");
    dt_puts("[dtr]   dist  : SOLO=local / REDUCED=TensorPar / FULL=Pipeline\r\n");
}

/* ------------------------------------------------------------------ */
/* dtr_task — パイプライン & テンソル並列ワーカー                    */
/* ------------------------------------------------------------------ */

void dtr_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    if (drpc_my_node == 0xFF) {
        dt_puts("[dtr] task: single-node mode (SOLO)\r\n");
        return;
    }

    UB my_node  = drpc_my_node;
    BOOL is_n0  = (my_node % 2 == 0);

    if (is_n0) {
        /* ---- Node 0 (even) ---- */
        /* Pipeline Parallel 用ハンドル (FULL mode) */
        h_l0_pub  = kdds_open(DTR_TOPIC_L0,     KDDS_QOS_LATEST_ONLY);
        h_res_sub = kdds_open(DTR_TOPIC_RESULT,  KDDS_QOS_LATEST_ONLY);
        /* Tensor Parallel 用ハンドル (REDUCED mode) */
        h_input_pub  = kdds_open(DTR_TOPIC_INPUT,  KDDS_QOS_LATEST_ONLY);
        h_head1_sub  = kdds_open(DTR_TOPIC_HEAD1,  KDDS_QOS_LATEST_ONLY);

        dt_puts("[dtr] node "); dt_putdec((UW)my_node);
        dt_puts(": stage0/TP-requester ready\r\n");

        /* Node0 は dtr_infer() が呼ばれたときだけ動く。
         * ここでは FULL mode の result 受信ループのみ常駐。 */
        for (;;) {
            DTR_RESULT res;
            W r = kdds_sub(h_res_sub, &res, (W)sizeof(res), -1);
            if (r < (W)sizeof(DTR_RESULT)) continue;
            if (res.magic != DTR_RESULT_MAGIC) continue;
            dtr_last_result = res;
            tk_sig_sem(dtr_result_sem, 1);
            dtr_stats.distributed++;
        }

    } else {
        /* ---- Node 1 (odd) — 両モードに対応するワーカー ---- */
        h_l0_sub  = kdds_open(DTR_TOPIC_L0,     KDDS_QOS_LATEST_ONLY);
        h_res_pub = kdds_open(DTR_TOPIC_RESULT,  KDDS_QOS_LATEST_ONLY);
        h_input_sub  = kdds_open(DTR_TOPIC_INPUT,  KDDS_QOS_LATEST_ONLY);
        h_head1_pub  = kdds_open(DTR_TOPIC_HEAD1,  KDDS_QOS_LATEST_ONLY);

        dt_puts("[dtr] node "); dt_putdec((UW)my_node);
        dt_puts(": stage1+2/TP-worker ready\r\n");

        for (;;) {
            /* dtr/l0 と dtr/input を交互にポーリング */

            /* --- Pipeline Parallel: dtr/l0 受信 (FULL mode) --- */
            {
                DTR_ACT act;
                W r = kdds_sub(h_l0_sub, &act, (W)sizeof(act), 0);
                if (r >= (W)sizeof(DTR_ACT) && act.magic == DTR_ACT_MAGIC) {
                    /* LN1 → FFN → LN2 → Cls */
                    float scores[DOUT];
                    DTR_RESULT res;
                    res.magic    = DTR_RESULT_MAGIC;
                    res.req_id   = act.req_id;
                    res.src_node = my_node;
                    res._pad     = 0;
                    res.class_id = run_stage12(act.act, scores);
                    for (INT si = 0; si < DOUT; si++) res.scores[si] = scores[si];
                    kdds_pub(h_res_pub, &res, (W)sizeof(res));

                    static const char *cn[] = {"normal", "alert", "critical"};
                    dt_puts("[dtr] pipeline: req="); dt_putdec(res.req_id);
                    dt_puts(" -> "); dt_puts(cn[res.class_id < 3 ? res.class_id : 0]);
                    dt_puts("\r\n");
                }
            }

            /* --- Tensor Parallel: dtr/input 受信 (REDUCED mode) --- */
            {
                DTR_INPUT inp;
                W r = kdds_sub(h_input_sub, &inp, (W)sizeof(inp), 0);
                if (r >= (W)sizeof(DTR_INPUT) && inp.magic == DTR_INPUT_MAGIC) {
                    /* Embed → head1 計算 */
                    float tok[SEQ][DM];
                    run_embed_seq(inp.input, tok);

                    float head1_out[SEQ][DH];
                    run_attn_head(tok, 1, head1_out);

                    DTR_HEAD_ACT pkt;
                    pkt.magic    = DTR_HEAD_MAGIC;
                    pkt.req_id   = inp.req_id;
                    pkt.src_node = my_node;
                    pkt.head_id  = 1;
                    pkt._pad     = 0;
                    for (INT t = 0; t < SEQ; t++)
                        for (INT d = 0; d < DH; d++)
                            pkt.out[t * DH + d] = head1_out[t][d];
                    kdds_pub(h_head1_pub, &pkt, (W)sizeof(pkt));

                    dt_puts("[dtr] TP: head1 req="); dt_putdec(inp.req_id);
                    dt_puts(" done → head1 pub\r\n");
                }
            }

            tk_dly_tsk(5);   /* 過負荷防止 */
        }
    }
}

/* ------------------------------------------------------------------ */
/* dtr_infer — 縮退モード対応分散推論 API                            */
/* ------------------------------------------------------------------ */

W dtr_infer(const B input[4])
{
    dtr_stats.inferences++;
    UB lvl = degrade_level();

    /* ========================== SOLO ========================== */
    if (drpc_my_node == 0xFF || lvl == DEGRADE_SOLO) {
        float scores[DOUT];
        UB cls = run_transformer_local(input, scores);
        dtr_stats.local++;

        static const char *cn[] = {"normal", "alert", "critical"};
        dt_puts("[dtr] local(SOLO): class="); dt_putdec((UW)cls);
        dt_puts(" ("); dt_puts(cn[cls < 3 ? cls : 0]);
        dt_puts(") scores=[");
        dt_putf2(scores[0]); dt_puts(" ");
        dt_putf2(scores[1]); dt_puts(" ");
        dt_putf2(scores[2]); dt_puts("]\r\n");
        return (W)cls;
    }

    /* ======================= REDUCED: Tensor Parallel ===================== */
    if (lvl == DEGRADE_REDUCED && (drpc_my_node % 2 == 0) && h_input_pub >= 0) {
        /* Step 1: Node0 が head0 を計算 */
        float tok[SEQ][DM];
        run_embed_seq(input, tok);
        dtr_stats.layer0_runs++;

        float head0_out[SEQ][DH];
        run_attn_head(tok, 0, head0_out);

        /* Step 2: raw input を Node1 へ送信 */
        DTR_INPUT inp_pkt;
        inp_pkt.magic    = DTR_INPUT_MAGIC;
        inp_pkt.req_id   = ++dtr_req_counter;
        inp_pkt.src_node = drpc_my_node;
        inp_pkt._pad[0]  = inp_pkt._pad[1] = inp_pkt._pad[2] = 0;
        for (INT i = 0; i < SEQ; i++) inp_pkt.input[i] = input[i];
        kdds_pub(h_input_pub, &inp_pkt, (W)sizeof(inp_pkt));

        dt_puts("[dtr] TP: req="); dt_putdec(dtr_req_counter);
        dt_puts(" head0 done, waiting head1...\r\n");

        /* Step 3: head1 を待つ (50ms × 16 = 800ms) */
        DTR_HEAD_ACT head1_pkt;
        ER er = E_TMOUT;
        INT retry = (INT)(DTR_INFER_TMO / 50);
        while (retry-- > 0) {
            W r = kdds_sub(h_head1_sub, &head1_pkt, (W)sizeof(head1_pkt), 0);
            if (r >= (W)sizeof(DTR_HEAD_ACT) &&
                head1_pkt.magic == DTR_HEAD_MAGIC &&
                head1_pkt.req_id == dtr_req_counter) {
                er = E_OK; break;
            }
            tk_dly_tsk(50);
        }
        if (er != E_OK) {
            dt_puts("[dtr] TP: timeout waiting head1\r\n");
            dtr_stats.timeouts++;
            /* fallback: head1 を自分で計算 */
            float h1_fb[SEQ][DH];
            run_attn_head(tok, 1, h1_fb);
            for (INT t = 0; t < SEQ; t++)
                for (INT d = 0; d < DH; d++)
                    head1_pkt.out[t * DH + d] = h1_fb[t][d];
        }

        /* Step 4: head0 + head1 を concat → W_o → [SEQ][DM] */
        float mhsa[SEQ][DM];
        for (INT t = 0; t < SEQ; t++) {
            float concat[DM];
            for (INT d = 0; d < DH; d++) concat[d]      = head0_out[t][d];
            for (INT d = 0; d < DH; d++) concat[DH + d]  = head1_pkt.out[t * DH + d];
            dt_linear((float *)W_o, NULL, concat, mhsa[t], DM, DM);
        }

        /* Step 5: 残差 + LN1 + FFN + 残差 + LN2 + Pool + Cls */
        for (INT t = 0; t < SEQ; t++) {
            for (INT d = 0; d < DM; d++) mhsa[t][d] += tok[t][d];
            dt_layernorm(mhsa[t], ln1_g, ln1_b, DM);
        }
        float ffn[SEQ][DM];
        run_ffn_seq(mhsa, ffn);
        for (INT t = 0; t < SEQ; t++) {
            for (INT d = 0; d < DM; d++) ffn[t][d] += mhsa[t][d];
            dt_layernorm(ffn[t], ln2_g, ln2_b, DM);
        }
        float pool[DM];
        run_mean_pool(ffn, pool);
        float scores[DOUT];
        UB cls = run_cls_head(pool, scores);

        if (er == E_OK) dtr_stats.tp_distributed++;

        static const char *cn[] = {"normal", "alert", "critical"};
        dt_puts("[dtr] TP(REDUCED): class="); dt_putdec((UW)cls);
        dt_puts(" ("); dt_puts(cn[cls < 3 ? cls : 0]);
        dt_puts(") scores=[");
        dt_putf2(scores[0]); dt_puts(" ");
        dt_putf2(scores[1]); dt_puts(" ");
        dt_putf2(scores[2]); dt_puts("]\r\n");
        return (W)cls;
    }

    /* ======================= FULL: Pipeline Parallel ====================== */
    if (h_l0_pub >= 0) {
        /* Node0 のみ dtr_infer() を呼ぶ想定 */
        BOOL has_peers = FALSE;
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n != drpc_my_node && dnode_table[n].state == DNODE_ALIVE) {
                has_peers = TRUE; break;
            }
        }
        if (has_peers) {
            float l0_out[DM];
            run_stage0(input, l0_out);   /* Embed + MHSA + mean-pool */

            DTR_ACT act;
            act.magic    = DTR_ACT_MAGIC;
            act.req_id   = ++dtr_req_counter;
            act.src_node = drpc_my_node;
            act.layer    = 1;
            act._pad     = 0;
            for (INT i = 0; i < DM; i++) act.act[i] = l0_out[i];
            kdds_pub(h_l0_pub, &act, (W)sizeof(act));

            dt_puts("[dtr] Pipeline: req="); dt_putdec(dtr_req_counter);
            dt_puts(" Attn done, waiting LN+FFN+Cls...\r\n");

            ER er = tk_wai_sem(dtr_result_sem, 1, (TMO)DTR_INFER_TMO);
            if (er != E_OK) {
                dt_puts("[dtr] Pipeline: timeout\r\n");
                dtr_stats.timeouts++;
                return -1;
            }

            static const char *cn[] = {"normal", "alert", "critical"};
            UB cls = dtr_last_result.class_id;
            dt_puts("[dtr] Pipeline(FULL): class="); dt_putdec((UW)cls);
            dt_puts(" ("); dt_puts(cn[cls < 3 ? cls : 0]);
            dt_puts(") scores=[");
            dt_putf2(dtr_last_result.scores[0]); dt_puts(" ");
            dt_putf2(dtr_last_result.scores[1]); dt_puts(" ");
            dt_putf2(dtr_last_result.scores[2]); dt_puts("]\r\n");
            return (W)cls;
        }
    }

    /* フォールバック: ローカル実行 */
    float scores[DOUT];
    UB cls = run_transformer_local(input, scores);
    dtr_stats.local++;
    dt_puts("[dtr] fallback(local): class="); dt_putdec((UW)cls); dt_puts("\r\n");
    return (W)cls;
}

/* ------------------------------------------------------------------ */
/* dtr_stat — 統計表示                                                */
/* ------------------------------------------------------------------ */

void dtr_stat(void)
{
    static const char *mode_str[] = { "FULL/Pipeline", "REDUCED/TensorPar", "SOLO/Local" };
    UB lvl = degrade_level();

    dt_puts("[dtr] Distributed Transformer Stats:\r\n");
    dt_puts("  arch        : Transformer (MHSA h=2 + FFN + Cls)\r\n");
    dt_puts("  node        : ");
    if (drpc_my_node == 0xFF) dt_puts("single");
    else dt_putdec((UW)drpc_my_node);
    dt_puts("  mode: ");
    dt_puts(drpc_my_node == 0xFF ? "SOLO" : mode_str[lvl < 3 ? lvl : 0]);
    dt_puts("\r\n");

    dt_puts("  inferences  : "); dt_putdec(dtr_stats.inferences);   dt_puts("\r\n");
    dt_puts("    local     : "); dt_putdec(dtr_stats.local);         dt_puts("\r\n");
    dt_puts("    pipeline  : "); dt_putdec(dtr_stats.distributed);   dt_puts("\r\n");
    dt_puts("    tensor_par: "); dt_putdec(dtr_stats.tp_distributed); dt_puts("\r\n");
    dt_puts("    timeouts  : "); dt_putdec(dtr_stats.timeouts);      dt_puts("\r\n");
    dt_puts("  attn runs   : "); dt_putdec(dtr_stats.attn_runs);     dt_puts("\r\n");
    dt_puts("  ffn  runs   : "); dt_putdec(dtr_stats.layer1_runs);   dt_puts("\r\n");
    dt_puts("  cls  runs   : "); dt_putdec(dtr_stats.output_runs);   dt_puts("\r\n");

    if (dtr_last_result.magic == DTR_RESULT_MAGIC) {
        static const char *cn[] = {"normal", "alert", "critical"};
        UB cls = dtr_last_result.class_id;
        dt_puts("  last result : class="); dt_putdec((UW)cls);
        dt_puts(" ("); dt_puts(cn[cls < 3 ? cls : 0]);
        dt_puts(")  scores=[");
        dt_putf2(dtr_last_result.scores[0]); dt_puts("  ");
        dt_putf2(dtr_last_result.scores[1]); dt_puts("  ");
        dt_putf2(dtr_last_result.scores[2]); dt_puts("]\r\n");
    }
}
