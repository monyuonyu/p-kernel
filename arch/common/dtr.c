/*
 *  dtr.c (x86)
 *  Phase 8/10/11 — Distributed Transformer Inference
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
 *    FULL    (3+ nodes): Distributed KV Attention (Phase 10) + Pipeline
 *      Phase 10 DKVA:
 *        各ノードが KV キャッシュを保持し、Q をブロードキャストして
 *        全ノードの KV を Attention に組み込む (集合記憶 Attention)。
 *      従来 Pipeline Parallel (フォールバック):
 *        Node 0 (even): Embed + MHSA(local) + mean-pool → "dtr/l0" pub
 *        Node 1 (odd) : "dtr/l0" sub → LN1 → FFN → LN2 → Cls → "dtr/result" pub
 */

#include "dtr.h"
#include "retrieval.h"
#include "dkva.h"
#include "kdds.h"
#include "drpc.h"
#include "degrade.h"
#include "dmn.h"
#include "reflex.h"   /* §8 反射層: 推論完了 → 局所即時防御アクション (配線②) */
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

/* ring3-core Wave C (III.3a): the kernel-compute counter — see dtr.h.
 * Defined here so both compute entries below can bump it; the dual-
 * compiled user ELF gets its own private copy of this very definition. */
volatile UW kernel_infer_count = 0;

float dt_relu(float x) { return x > 0.0f ? x : 0.0f; }

/* IEEE754 binary32 bit-punning (2^k construction / mantissa split).
 * Every supported target (i686 bare, aarch64 bare, aarch64-linux,
 * x86_64-linux) is little-endian IEEE754; pinned here so a port to
 * anything exotic fails loudly instead of mis-training. */
_Static_assert(sizeof(float) == 4, "float must be IEEE754 binary32");
typedef union { float f; UW u; } DT_F32;

/*
 *  exp(x), libc-free — exported as dtr_expf for fedlearn/ai_job.
 *
 *  HONESTY NOTE (R3a): the previous implementation was a raw 7-term
 *  Taylor series clamped at |x|=10. That is fine near 0 but garbage
 *  past |x|~3 (it returned 848.0 for exp(-10) instead of 4.5e-5), so
 *  every softmax with a logit gap over a few units produced silently
 *  wrong probabilities. Cross-entropy training needs real
 *  probabilities, hence: range reduction e^x = 2^k * e^r with
 *  k = round(x/ln2), r in [-ln2/2, +ln2/2], 7-term Taylor on r
 *  (rel. err ~1e-7), then scale by 2^k built from exponent bits.
 */
float dtr_expf(float x)
{
    if (x >  88.0f) return 3.0e38f;
    if (x < -87.0f) return 0.0f;
    float t = x * 1.4426950f;                       /* x / ln2        */
    INT  k = (INT)(t + (t >= 0.0f ? 0.5f : -0.5f)); /* round          */
    float r = x - (float)k * 0.69314718f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f +
              r * (0.041666667f + r * (0.0083333333f +
              r * 0.0013888889f)))));
    DT_F32 s; s.u = (UW)(k + 127) << 23;            /* 2^k            */
    return p * s.f;
}

/*
 *  ln(x), libc-free — needed for the cross-entropy loss (R3a).
 *  x = m * 2^e with m in [1,2); ln(m) = 2*atanh((m-1)/(m+1)) via 5 odd
 *  terms (rel. err < 1e-7 on [1,2)).
 */
float dtr_logf(float x)
{
    if (x < 1e-30f) return -69.0f;                  /* ln(1e-30) floor */
    DT_F32 v; v.f = x;
    INT e = (INT)((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFFU) | 0x3F800000U;        /* m in [1,2)      */
    float m  = v.f;
    float z  = (m - 1.0f) / (m + 1.0f);
    float z2 = z * z;
    float l  = 2.0f * z * (1.0f + z2 * (0.33333333f + z2 * (0.2f +
               z2 * (0.14285714f + z2 * 0.11111111f))));
    return l + (float)e * 0.69314718f;
}

static float dt_exp(float x) { return dtr_expf(x); }

/* sqrt(x): Newton-Raphson 法 */
float dt_sqrt(float x)
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
void dt_linear(const float *W, const float *b,
                      const float *x, float *y, INT M, INT N)
{
    for (INT m = 0; m < M; m++) {
        float s = b ? b[m] : 0.0f;
        for (INT n = 0; n < N; n++) s += W[m * N + n] * x[n];
        y[m] = s;
    }
}

/* softmax in-place */
void dt_softmax(float *x, INT n)
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

/* ------------------------------------------------------------------ */
/* Phase 14 — 推論ログ + GA サポート                                  */
/* ------------------------------------------------------------------ */

/* 推論ログ リングバッファ */
static DTR_LOG_ENTRY dtr_log[DTR_LOG_SIZE];
static UW dtr_log_head  = 0;   /* 次回書き込み位置 (mod DTR_LOG_SIZE) */
static UW dtr_log_count = 0;   /* 有効エントリ数 (0〜DTR_LOG_SIZE)    */

/* GA 実行中フラグ */
volatile UB dtr_ga_busy = 0;

/* ログに推論結果を追記 (内部用) */
static void dtr_log_push(const B input[SEQ], UB class_id, UB conf_pct)
{
    UW idx = dtr_log_head;
    for (INT i = 0; i < SEQ; i++) dtr_log[idx].input[i] = input[i];
    dtr_log[idx].class_id       = class_id;
    dtr_log[idx].confidence_pct = conf_pct;
    dtr_log[idx]._pad           = 0;
    dtr_log_head = (dtr_log_head + 1) % DTR_LOG_SIZE;
    if (dtr_log_count < DTR_LOG_SIZE) dtr_log_count++;
    /* 配線②: 推論結果を虚空に消さず、§8 反射層へ繋ぐ (推論完了点フック)。
     * class を脅威レベルと解釈し、reflex がアクション表で局所防御に変換する。 */
    reflex_on_inference(class_id, conf_pct, drpc_my_node);
}

/* ---- 公開 API ---- */

UW dtr_log_avail(void) { return dtr_log_count; }

void dtr_log_get_entry(UW idx, DTR_LOG_ENTRY *out)
{
    if (idx >= dtr_log_count) { out->class_id = 0xFF; return; }
    /* idx=0 が最新エントリ (head-1), idx=1 がその前, … */
    UW slot = (dtr_log_head + DTR_LOG_SIZE - 1 - idx) % DTR_LOG_SIZE;
    *out = dtr_log[slot];
}

/* 重みコピーユーティリティ */
static void dtr_cpy(float *dst, const float *src, INT n)
{
    for (INT i = 0; i < n; i++) dst[i] = src[i];
}

void dtr_weights_get(float *buf)
{
    INT off = 0;
#define WG(arr, n)  dtr_cpy(buf + off, (const float *)(arr), (n)); off += (n)
    WG(W_emb,  EMB_OUT * EMB_IN);   /*  32 */
    WG(b_emb,  EMB_OUT);            /*   8 */
    WG(W_q,    NH * DM * DH);       /*  64 */
    WG(W_k,    NH * DM * DH);       /*  64 */
    WG(W_v,    NH * DM * DH);       /*  64 */
    WG(W_o,    DM * DM);            /*  64 */
    WG(ln1_g,  DM);                 /*   8 */
    WG(ln1_b,  DM);                 /*   8 */
    WG(ln2_g,  DM);                 /*   8 */
    WG(ln2_b,  DM);                 /*   8 */
    WG(W_ffn1, DFFN * DM);          /* 128 */
    WG(b_ffn1, DFFN);               /*  16 */
    WG(W_ffn2, DM * DFFN);          /* 128 */
    WG(b_ffn2, DM);                 /*   8 */
    WG(W_cls,  DOUT * DM);          /*  24 */
    WG(b_cls,  DOUT);               /*   3 */
#undef WG
    /* off == DTR_WEIGHT_FLOATS (635) */
}

void dtr_weights_set(const float *buf)
{
    INT off = 0;
#define WS(arr, n)  dtr_cpy((float *)(arr), buf + off, (n)); off += (n)
    WS(W_emb,  EMB_OUT * EMB_IN);
    WS(b_emb,  EMB_OUT);
    WS(W_q,    NH * DM * DH);
    WS(W_k,    NH * DM * DH);
    WS(W_v,    NH * DM * DH);
    WS(W_o,    DM * DM);
    WS(ln1_g,  DM);
    WS(ln1_b,  DM);
    WS(ln2_g,  DM);
    WS(ln2_b,  DM);
    WS(W_ffn1, DFFN * DM);
    WS(b_ffn1, DFFN);
    WS(W_ffn2, DM * DFFN);
    WS(b_ffn2, DM);
    WS(W_cls,  DOUT * DM);
    WS(b_cls,  DOUT);
#undef WS
}

/* 前方宣言 (run_transformer_local は後方で定義) */
static UB run_transformer_local(const B input[SEQ], float scores_out[DOUT]);

float dtr_eval_confidence(void)
{
    UW n = dtr_log_count;
    if (n == 0) return 0.0f;
    float total = 0.0f;
    for (UW i = 0; i < n; i++) {
        DTR_LOG_ENTRY e;
        dtr_log_get_entry(i, &e);
        float scores[DOUT];
        run_transformer_local(e.input, scores);
        /* max softmax score */
        float mx = scores[0];
        for (INT c = 1; c < DOUT; c++) if (scores[c] > mx) mx = scores[c];
        total += mx;
    }
    return total / (float)n;
}

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
    kernel_infer_count++;   /* ring3-core III.3a: kernel-side compute */
    for (INT t = 0; t < SEQ; t++) {
        float in_f = (float)input[t] / 127.0f;
        for (INT d = 0; d < DM; d++) {
            float s = b_emb[d];
            s += W_emb[d][0] * in_f;   /* 各センサー値は独立したトークン */
            /* R3a: W_emb columns 1..3 used to be multiplied by a
             * literal 0.0f — 24 dead parameters. They are repurposed
             * as learned POSITIONAL embeddings (one per token slot
             * t=1..3; t=0 is covered by b_emb). Without them the
             * pipeline (shared embed -> permutation-equivariant
             * attention -> mean pool) cannot tell WHICH sensor channel
             * a value came from, and a supervised task whose label is
             * driven by channel 0 (temperature) would be ill-posed. */
            if (t > 0) s += W_emb[d][t];
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

    float K_all[NH][SEQ][DH], V_all[NH][SEQ][DH];
    float scale = 1.0f / dt_sqrt((float)DH);

    for (INT h = 0; h < NH; h++) {
        run_attn_head(tok, h, head_out[h]);

        /* KV キャッシュ用に K/V を保存 (DKVA 用) */
        for (INT t = 0; t < SEQ; t++) {
            dt_linear((float *)W_k[h], NULL, tok[t], K_all[h][t], DH, DM);
            dt_linear((float *)W_v[h], NULL, tok[t], V_all[h][t], DH, DM);
            (void)scale;
        }
    }

    /* KV キャッシュを更新 (分散 Attention 用) */
    dkva_cache_update(K_all, V_all);

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
 *  DKVA 用 KV キャッシュ warmup (Phase 10, follow-up #2)。
 *
 *  新規 FULL クラスタではどのノードも一度もローカル推論をしていないため
 *  各ノードの kv_cache が空 → compute_partial が全ゼロを返し、集約された
 *  Attention が自明 (entries=0) になってしまう。
 *
 *  そこで各ノードが自分のノード ID から導いた「ノード固有の」合成入力で
 *  ローカル MHSA を数回回し、kv_cache を seed する (run_mhsa_local が
 *  dkva_cache_update を呼ぶ)。ノードごとに異なる入力 → 異なる K/V を持つので、
 *  requester が複数 peer の partial を集約したときに初めて「クラスタの集合
 *  記憶」を Attention に取り込んだ非自明な結果になる。
 */
void dtr_seed_kv_cache(UB node)
{
    for (INT s = 0; s < DTR_KV_SEED_N; s++) {
        B input[SEQ];
        for (INT t = 0; t < SEQ; t++)
            input[t] = (B)(17 * (node + 1) + 31 * s + 13 * t);
        float tok[SEQ][DM];
        run_embed_seq(input, tok);
        float mhsa[SEQ][DM];
        run_mhsa_local(tok, mhsa);   /* dkva_cache_update を内部で呼ぶ */
    }
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
 *
 *  Wave 8 ①: softmax の直前に retrieval の票を logits に加算する。
 *  input が NULL のパス (pipeline stage12 — 生入力が手元に無い) では
 *  retrieval は掛からない。ret_blend は OFF / engram 未取得なら no-op。
 */
static UB run_cls_head(const float vec[DM], const B *input,
                       float scores[DOUT])
{
    dt_linear((float *)W_cls, b_cls, vec, scores, DOUT, DM);
    if (input) ret_blend(input, scores);   /* 記憶→思考 (p-fs engram) */
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

    /* 4. Mean Pool + Cls (+ retrieval blend, ON のとき) */
    float pool[DM];
    run_mean_pool(ffn, pool);
    return run_cls_head(pool, input, scores_out);
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
    /* pipeline worker は生入力を持たない → retrieval なし (NULL) */
    return run_cls_head(ln, NULL, scores);
}

/* ------------------------------------------------------------------ */
/* R3a — 本物の学習 (supervised training, analytic backprop)          */
/*                                                                     */
/* PR #3 の批判への直接の答え:「LCG乱数で初期化されたまま一度も学習   */
/* されていません … 乱数のままなら『AI』と呼ぶのをやめる」。          */
/* ここは本物である: cross-entropy 損失 + この 635 パラメータグラフ   */
/* 全体 (embed/pos, W_q/k/v/o, LN1/2 γβ, FFN, cls) を解析的に逆伝播   */
/* する full-batch SGD。有限差分ではない (勾配検証用の dtr_grad_check */
/* だけが有限差分を使い、解析勾配と突き合わせる)。                    */
/*                                                                     */
/* 勾配バッファ g_grad は dtr_weights_get/set と同じ flat 並びを使う:  */
/*   W_emb(32) b_emb(8) W_q(64) W_k(64) W_v(64) W_o(64)                */
/*   ln1_g(8) ln1_b(8) ln2_g(8) ln2_b(8)                               */
/*   W_ffn1(128) b_ffn1(16) W_ffn2(128) b_ffn2(8) W_cls(24) b_cls(3)   */
/* ------------------------------------------------------------------ */

/* flat-buffer offsets (must mirror dtr_weights_get order) */
#define G_W_EMB    0
#define G_B_EMB   32
#define G_W_Q     40
#define G_W_K    104
#define G_W_V    168
#define G_W_O    232
#define G_LN1_G  296
#define G_LN1_B  304
#define G_LN2_G  312
#define G_LN2_B  320
#define G_W_FFN1 328
#define G_B_FFN1 456
#define G_W_FFN2 472
#define G_B_FFN2 600
#define G_W_CLS  608
#define G_B_CLS  632
_Static_assert(G_B_CLS + DOUT == DTR_WEIGHT_FLOATS,
               "flat gradient layout must cover exactly 635 params");

/* forward activations cached for backprop. Static, never task-stack
 * locals (feedback_hosted_relay_stack_overflow). Training is driven
 * from the shell task only; no concurrent use. */
typedef struct {
    float in_f[SEQ];                 /* input[t]/127                  */
    float tok[SEQ][DM];              /* post-ReLU embed               */
    float Q[NH][SEQ][DH];
    float K[NH][SEQ][DH];
    float V[NH][SEQ][DH];
    float attn[NH][SEQ][SEQ];        /* softmax(QK^T/sqrt(DH))        */
    float concat[SEQ][DM];           /* concat(heads)                 */
    float r1[SEQ][DM];               /* mhsa + tok   (pre-LN1)        */
    float xh1[SEQ][DM];              /* LN1 normalized x-hat          */
    float istd1[SEQ];                /* LN1 1/sqrt(var+eps)           */
    float y1[SEQ][DM];               /* post-LN1                      */
    float mid[SEQ][DFFN];            /* post-ReLU FFN hidden          */
    float r2[SEQ][DM];               /* ffn + y1     (pre-LN2)        */
    float xh2[SEQ][DM];
    float istd2[SEQ];
    float y2[SEQ][DM];               /* post-LN2                      */
    float pool[DM];
    float probs[DOUT];               /* softmax output                */
} DT_TCACHE;

static DT_TCACHE tc;
static float g_grad[DTR_WEIGHT_FLOATS];
static float g_flatw[DTR_WEIGHT_FLOATS];

/* LayerNorm forward that caches x-hat and 1/std for the backward.
 * Width n is a parameter so the sensor brain (n==DM) and the R3 in-context
 * harness (n==its own d_model) call the SAME kernel — never a fork
 * (docs/architecture/r3-nontrivial-thought.md, anti-fork rule). */
void dtr_ln_fwd_cache(const float *x, const float *g, const float *b,
                      float *xh, float *istd_out, float *y, INT n)
{
    float mean = 0.0f, var = 0.0f;
    for (INT i = 0; i < n; i++) mean += x[i];
    mean /= (float)n;
    for (INT i = 0; i < n; i++) { float d = x[i] - mean; var += d * d; }
    var /= (float)n;
    float istd = 1.0f / dt_sqrt(var + 1e-5f);
    *istd_out = istd;
    for (INT i = 0; i < n; i++) {
        xh[i] = (x[i] - mean) * istd;
        y[i]  = xh[i] * g[i] + b[i];
    }
}

/* LayerNorm backward: y = xh*g + b, xh = (x-mean)*istd.
 *   dg += dy*xh ; db += dy ; dxh = dy*g
 *   dx = istd * (dxh - mean(dxh) - xh * mean(dxh*xh))                 */
void dtr_ln_bwd(const float *dy, const float *xh, float istd,
                const float *g, float *dgam, float *dbet, float *dx, INT n)
{
    float dxh[DTR_LN_MAXW], m1 = 0.0f, m2 = 0.0f;
    for (INT i = 0; i < n; i++) {
        dgam[i] += dy[i] * xh[i];
        dbet[i] += dy[i];
        dxh[i]   = dy[i] * g[i];
        m1 += dxh[i];
        m2 += dxh[i] * xh[i];
    }
    m1 /= (float)n; m2 /= (float)n;
    for (INT i = 0; i < n; i++)
        dx[i] = (dxh[i] - m1 - xh[i] * m2) * istd;
}

/* Side-effect-free forward (no stats, no DKVA cache update, no log),
 * caching every activation backprop needs. Returns the cross-entropy
 * loss -ln p[label]; fills tc. */
static float train_forward(const B input[SEQ], UB label)
{
    kernel_infer_count++;   /* ring3-core III.3a: kernel-side compute */
    float scale = 1.0f / dt_sqrt((float)DH);

    /* embed (+ positional cols, mirrors run_embed_seq) */
    for (INT t = 0; t < SEQ; t++) {
        tc.in_f[t] = (float)input[t] / 127.0f;
        for (INT d = 0; d < DM; d++) {
            float s = b_emb[d] + W_emb[d][0] * tc.in_f[t];
            if (t > 0) s += W_emb[d][t];
            tc.tok[t][d] = dt_relu(s);
        }
    }

    /* MHSA */
    for (INT h = 0; h < NH; h++) {
        for (INT t = 0; t < SEQ; t++) {
            dt_linear((float *)W_q[h], NULL, tc.tok[t], tc.Q[h][t], DH, DM);
            dt_linear((float *)W_k[h], NULL, tc.tok[t], tc.K[h][t], DH, DM);
            dt_linear((float *)W_v[h], NULL, tc.tok[t], tc.V[h][t], DH, DM);
        }
        for (INT i = 0; i < SEQ; i++) {
            for (INT j = 0; j < SEQ; j++) {
                float s = 0.0f;
                for (INT d = 0; d < DH; d++) s += tc.Q[h][i][d] * tc.K[h][j][d];
                tc.attn[h][i][j] = s * scale;
            }
            dt_softmax(tc.attn[h][i], SEQ);
        }
        for (INT i = 0; i < SEQ; i++) {
            for (INT d = 0; d < DH; d++) {
                float s = 0.0f;
                for (INT j = 0; j < SEQ; j++)
                    s += tc.attn[h][i][j] * tc.V[h][j][d];
                tc.concat[i][h * DH + d] = s;
            }
        }
    }

    /* W_o + residual + LN1 */
    for (INT t = 0; t < SEQ; t++) {
        float m[DM];
        dt_linear((float *)W_o, NULL, tc.concat[t], m, DM, DM);
        for (INT d = 0; d < DM; d++) tc.r1[t][d] = m[d] + tc.tok[t][d];
        dtr_ln_fwd_cache(tc.r1[t], ln1_g, ln1_b, tc.xh1[t], &tc.istd1[t], tc.y1[t], DM);
    }

    /* FFN + residual + LN2 */
    for (INT t = 0; t < SEQ; t++) {
        dt_linear((float *)W_ffn1, b_ffn1, tc.y1[t], tc.mid[t], DFFN, DM);
        for (INT k = 0; k < DFFN; k++) tc.mid[t][k] = dt_relu(tc.mid[t][k]);
        float f[DM];
        dt_linear((float *)W_ffn2, b_ffn2, tc.mid[t], f, DM, DFFN);
        for (INT d = 0; d < DM; d++) tc.r2[t][d] = f[d] + tc.y1[t][d];
        dtr_ln_fwd_cache(tc.r2[t], ln2_g, ln2_b, tc.xh2[t], &tc.istd2[t], tc.y2[t], DM);
    }

    /* mean pool + cls softmax */
    for (INT d = 0; d < DM; d++) {
        float s = 0.0f;
        for (INT t = 0; t < SEQ; t++) s += tc.y2[t][d];
        tc.pool[d] = s / (float)SEQ;
    }
    dt_linear((float *)W_cls, b_cls, tc.pool, tc.probs, DOUT, DM);
    /* Wave 8 ①: eval 経路でも softmax 前に記憶の票を加算する。訓練と
     * 勾配検証は dtr_train_batch / dtr_grad_check が retrieval を一時
     * OFF にするので、学習は素の重みのまま (記憶は松葉杖にしない)。
     * 数学的注意: 票は重みに対して定数なので、仮に ON のままでも
     * dlogits = p - onehot の解析勾配は正確なまま。 */
    ret_blend(input, tc.probs);
    dt_softmax(tc.probs, DOUT);

    float pl = tc.probs[label];
    if (pl < 1e-7f) pl = 1e-7f;
    return -dtr_logf(pl);
}

/* Analytic backprop through the whole graph; accumulates into g_grad.
 * Must run immediately after train_forward on the same sample. */
static void train_backward(UB label)
{
    float scale = 1.0f / dt_sqrt((float)DH);

    /* static backward scratch (stack discipline) */
    static float dy2[SEQ][DM], dr2[SEQ][DM], dy1[SEQ][DM];
    static float dr1[SEQ][DM], dtok[SEQ][DM], dconcat[SEQ][DM];
    static float dQ[SEQ][DH], dK[SEQ][DH], dV[SEQ][DH];

    /* cls: dlogits = p - onehot */
    float dlog[DOUT], dpool[DM];
    for (INT c = 0; c < DOUT; c++)
        dlog[c] = tc.probs[c] - (c == (INT)label ? 1.0f : 0.0f);
    for (INT c = 0; c < DOUT; c++) {
        g_grad[G_B_CLS + c] += dlog[c];
        for (INT d = 0; d < DM; d++)
            g_grad[G_W_CLS + c * DM + d] += dlog[c] * tc.pool[d];
    }
    for (INT d = 0; d < DM; d++) {
        float s = 0.0f;
        for (INT c = 0; c < DOUT; c++) s += W_cls[c][d] * dlog[c];
        dpool[d] = s;
    }

    /* mean pool */
    for (INT t = 0; t < SEQ; t++)
        for (INT d = 0; d < DM; d++)
            dy2[t][d] = dpool[d] / (float)SEQ;

    /* LN2 -> residual -> FFN -> (residual into dy1) */
    for (INT t = 0; t < SEQ; t++) {
        dtr_ln_bwd(dy2[t], tc.xh2[t], tc.istd2[t], ln2_g,
               &g_grad[G_LN2_G], &g_grad[G_LN2_B], dr2[t], DM);
        for (INT d = 0; d < DM; d++) dy1[t][d] = dr2[t][d];  /* residual */

        /* f = W_ffn2 * mid + b_ffn2  (df = dr2) */
        float dmid[DFFN];
        for (INT k = 0; k < DFFN; k++) dmid[k] = 0.0f;
        for (INT d = 0; d < DM; d++) {
            g_grad[G_B_FFN2 + d] += dr2[t][d];
            for (INT k = 0; k < DFFN; k++) {
                g_grad[G_W_FFN2 + d * DFFN + k] += dr2[t][d] * tc.mid[t][k];
                dmid[k] += W_ffn2[d][k] * dr2[t][d];
            }
        }
        /* mid = relu(W_ffn1 * y1 + b_ffn1) */
        for (INT k = 0; k < DFFN; k++) {
            if (tc.mid[t][k] <= 0.0f) { dmid[k] = 0.0f; continue; }
            g_grad[G_B_FFN1 + k] += dmid[k];
            for (INT d = 0; d < DM; d++) {
                g_grad[G_W_FFN1 + k * DM + d] += dmid[k] * tc.y1[t][d];
                dy1[t][d] += W_ffn1[k][d] * dmid[k];
            }
        }
    }

    /* LN1 -> residual -> W_o */
    for (INT t = 0; t < SEQ; t++) {
        dtr_ln_bwd(dy1[t], tc.xh1[t], tc.istd1[t], ln1_g,
               &g_grad[G_LN1_G], &g_grad[G_LN1_B], dr1[t], DM);
        for (INT d = 0; d < DM; d++) dtok[t][d] = dr1[t][d];  /* residual */

        /* m = W_o * concat  (dm = dr1) */
        for (INT n = 0; n < DM; n++) dconcat[t][n] = 0.0f;
        for (INT d = 0; d < DM; d++) {
            for (INT n = 0; n < DM; n++) {
                g_grad[G_W_O + d * DM + n] += dr1[t][d] * tc.concat[t][n];
                dconcat[t][n] += W_o[d][n] * dr1[t][d];
            }
        }
    }

    /* attention backward, per head */
    for (INT h = 0; h < NH; h++) {
        for (INT t = 0; t < SEQ; t++)
            for (INT d = 0; d < DH; d++)
                dQ[t][d] = dK[t][d] = dV[t][d] = 0.0f;

        for (INT i = 0; i < SEQ; i++) {
            /* dout for this head = slice of dconcat */
            const float *dout = &dconcat[i][h * DH];
            float da[SEQ], dots = 0.0f;
            for (INT j = 0; j < SEQ; j++) {
                float s = 0.0f;
                for (INT d = 0; d < DH; d++) {
                    dV[j][d] += tc.attn[h][i][j] * dout[d];
                    s += dout[d] * tc.V[h][j][d];
                }
                da[j] = s;
                dots += da[j] * tc.attn[h][i][j];
            }
            /* softmax backward + scaled dot-product backward */
            for (INT j = 0; j < SEQ; j++) {
                float ds = tc.attn[h][i][j] * (da[j] - dots) * scale;
                for (INT d = 0; d < DH; d++) {
                    dQ[i][d] += ds * tc.K[h][j][d];
                    dK[j][d] += ds * tc.Q[h][i][d];
                }
            }
        }

        /* Q/K/V projections: P[d] = sum_n Wp[d*DM+n] * tok[n] */
        const float *wq = (const float *)W_q[h];
        const float *wk = (const float *)W_k[h];
        const float *wv = (const float *)W_v[h];
        UW oq = (UW)(G_W_Q + h * DM * DH);
        UW ok = (UW)(G_W_K + h * DM * DH);
        UW ov = (UW)(G_W_V + h * DM * DH);
        for (INT t = 0; t < SEQ; t++) {
            for (INT d = 0; d < DH; d++) {
                for (INT n = 0; n < DM; n++) {
                    g_grad[oq + (UW)(d * DM + n)] += dQ[t][d] * tc.tok[t][n];
                    g_grad[ok + (UW)(d * DM + n)] += dK[t][d] * tc.tok[t][n];
                    g_grad[ov + (UW)(d * DM + n)] += dV[t][d] * tc.tok[t][n];
                    dtok[t][n] += wq[d * DM + n] * dQ[t][d]
                                + wk[d * DM + n] * dK[t][d]
                                + wv[d * DM + n] * dV[t][d];
                }
            }
        }
    }

    /* embed backward (ReLU mask: tok>0 <=> pre-activation>0) */
    for (INT t = 0; t < SEQ; t++) {
        for (INT d = 0; d < DM; d++) {
            if (tc.tok[t][d] <= 0.0f) continue;
            float dz = dtok[t][d];
            g_grad[G_B_EMB + d] += dz;
            g_grad[G_W_EMB + d * EMB_IN + 0] += dz * tc.in_f[t];
            if (t > 0) g_grad[G_W_EMB + d * EMB_IN + t] += dz;
        }
    }
}

/* ---- exported training API (used by dtr_train.c) ------------------ */

/* One full-batch SGD step over (X[i], y[i]), i < n.
 * Returns the mean cross-entropy loss at the CURRENT weights. */
float dtr_train_batch(const B (*X)[DTR_SEQ_LEN], const UB *y, UW n, float lr)
{
    if (n == 0) return 0.0f;
    for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) g_grad[i] = 0.0f;

    /* 学習中は retrieval を切る — 重みは自力で学ぶ (記憶ブレンドの
     * 上に重みを最適化すると、記憶なしでは立てない重みになる) */
    UB rprev = ret_set(0);

    float loss = 0.0f;
    for (UW s = 0; s < n; s++) {
        loss += train_forward(X[s], y[s]);
        train_backward(y[s]);
    }
    ret_set(rprev);

    dtr_weights_get(g_flatw);
    float step = lr / (float)n;
    for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++)
        g_flatw[i] -= step * g_grad[i];
    dtr_weights_set(g_flatw);

    return loss / (float)n;
}

/* Mean CE loss + accuracy on (X, y) without touching the weights. */
float dtr_eval_batch(const B (*X)[DTR_SEQ_LEN], const UB *y, UW n,
                     UW *correct_out)
{
    if (n == 0) { if (correct_out) *correct_out = 0; return 0.0f; }
    float loss = 0.0f; UW correct = 0;
    for (UW s = 0; s < n; s++) {
        loss += train_forward(X[s], y[s]);
        UB cls = 0;
        for (INT c = 1; c < DOUT; c++)
            if (tc.probs[c] > tc.probs[cls]) cls = (UB)c;
        if (cls == y[s]) correct++;
    }
    if (correct_out) *correct_out = correct;
    return loss / (float)n;
}

/* Side-effect-free class probabilities for one input under the CURRENTLY
 * loaded weights (retrieval forced OFF so the result reflects only the
 * weights — R3b spec.c routes/ensembles raw specialist outputs). No
 * stats, no DKVA cache, no log, no reflex. */
void dtr_forward_probs(const B input[DTR_SEQ_LEN], float out[DTR_OUT_DIM])
{
    UB rprev = ret_set(0);
    (void)train_forward(input, 0);   /* label irrelevant; fills tc.probs */
    ret_set(rprev);
    for (INT c = 0; c < DOUT; c++) out[c] = tc.probs[c];
}

/* ONE BRAIN (wave 18): the learned-model argmax for one input. The live
 * inference path (moe_infer local + drpc DRPC_CALL_INFER remote) routes,
 * returns, and guards through THIS forward — replacing ai_job.c's hand-
 * written-constant mlp_forward in the live path. Uses all 4 sensor tokens
 * (DTR_SEQ_LEN==4). See docs/review-2026-06-three-brains.md. */
UB dtr_classify(const B input[DTR_SEQ_LEN])
{
    float p[DTR_OUT_DIM];
    dtr_forward_probs(input, p);
    UB cls = 0;
    for (UB c = 1; c < (UB)DTR_OUT_DIM; c++) if (p[c] > p[cls]) cls = c;
    return cls;
}

/* ------------------------------------------------------------------ */
/* FP-determinism golden-logit cert ("one mind, one math")             */
/*                                                                     */
/* Reseeds the weights with a FIXED seed (no dependence on training    */
/* history), runs the REAL production forward (train_forward via       */
/* dtr_forward_probs — the same dt_linear multiply-accumulate the live */
/* inference path uses) over a fixed bank of inputs, and folds the     */
/* RAW 32-bit IEEE-754 bit patterns of every output logit into one     */
/* FNV-1a hash.  Hashing the bit patterns (not the float values) is    */
/* the whole point: a 1-ULP difference from FMA contraction flips low  */
/* mantissa bits and changes the hash, so a clang build that contracts */
/* a*b+c into an fmadd produces a DIFFERENT hash than a gcc build that */
/* rounds the multiply and the add separately.  -ffp-contract=off on   */
/* every target makes them agree — that agreement is what this cert    */
/* asserts.  Pure: saves and restores the live weights, so calling it  */
/* from the shell does not disturb a trained mind.                     */

#define FPDET_SEED   0x0F9DE7A1UL   /* fixed reseed — history-independent */

/* FNV-1a over a 32-bit word's 4 bytes (LE order, deterministic). */
static UW fpdet_fold(UW h, UW word)
{
    for (INT k = 0; k < 4; k++) {
        h ^= (word >> (k * 8)) & 0xFFu;
        h *= 16777619UL;
        h &= 0xFFFFFFFFUL;
    }
    return h;
}

UW dtr_fpdet_hash(void)
{
    /* save the live (possibly trained) weights so the cert is pure */
    dtr_weights_get(g_flatw);

    /* deterministic, history-independent weights */
    UB rprev = ret_set(0);
    dtr_reinit_weights(FPDET_SEED);

    /* fixed input bank — spans the int8 sensor range so every linear
     * row sees non-trivial magnitudes (more bits => stronger guard). */
    static const B bank[8][DTR_SEQ_LEN] = {
        {   0,   0,   0,   0 },
        {  10,  20,  30,  40 },
        { -40, -30, -20, -10 },
        { 127, 127, 127, 127 },
        {-128,-128,-128,-128 },
        {  17, -53,  91, -11 },
        { -99,  42,   7, 120 },
        {  63, -64,  31, -32 },
    };

    UW h = 2166136261UL;             /* FNV offset basis */
    for (INT i = 0; i < 8; i++) {
        float p[DTR_OUT_DIM];
        (void)train_forward(bank[i], 0);   /* REAL forward, fills tc.probs */
        for (INT c = 0; c < DOUT; c++) p[c] = tc.probs[c];
        for (INT c = 0; c < DOUT; c++) {
            UW word;
            /* type-pun the float to its raw IEEE-754 bits */
            __builtin_memcpy(&word, &p[c], 4);
            h = fpdet_fold(h, word);
        }
    }

    /* restore the live weights and retrieval state */
    dtr_weights_set(g_flatw);
    ret_set(rprev);
    return h;
}

/* Gradient check: compares the analytic gradient against central
 * finite differences on a spread of parameter indices for one sample.
 * Returns the max relative error — the proof that train_backward is
 * the real derivative of train_forward, not a plausible-looking one. */
float dtr_grad_check(const B input[DTR_SEQ_LEN], UB label)
{
    UB rprev = ret_set(0);     /* 素の重みの勾配を検証する */
    for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) g_grad[i] = 0.0f;
    (void)train_forward(input, label);
    train_backward(label);

    dtr_weights_get(g_flatw);
    const float eps = 2e-3f;
    float worst = 0.0f;
    /* stride 13 covers every weight family incl. LN params */
    for (INT i = 0; i < DTR_WEIGHT_FLOATS; i += 13) {
        float orig = g_flatw[i];
        g_flatw[i] = orig + eps; dtr_weights_set(g_flatw);
        float lp = train_forward(input, label);
        g_flatw[i] = orig - eps; dtr_weights_set(g_flatw);
        float lm = train_forward(input, label);
        g_flatw[i] = orig;       dtr_weights_set(g_flatw);
        float fd  = (lp - lm) / (2.0f * eps);
        float ref = fd < 0.0f ? -fd : fd;
        if (ref < 0.05f) ref = 0.05f;        /* absolute floor for tiny grads */
        float diff = g_grad[i] - fd;
        if (diff < 0.0f) diff = -diff;
        float rel = diff / ref;
        if (rel > worst) worst = rel;
    }
    ret_set(rprev);
    return worst;
}

/* ------------------------------------------------------------------ */
/* dtr_init                                                            */
/* ------------------------------------------------------------------ */

/*
 *  Roll all weights from a given LCG seed (He-style init), gamma=1/beta=0
 *  LayerNorm. Extracted from dtr_init so R3b's specialization harness
 *  (spec.c) can reinitialize a fresh expert with a per-expert seed —
 *  "異なる初期化＋ローカル学習" (survival-network.md 道B). dtr_init()
 *  keeps using the original 0xDEAD8888 seed so the existing single-node
 *  train/eval demo numbers are byte-for-byte unchanged.
 */
void dtr_reinit_weights(UW seed)
{
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
}

void dtr_init(void)
{
    dtr_reinit_weights(0xDEAD8888UL);

    /* 分散推論セマフォ */
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
    dtr_result_sem = tk_cre_sem(&cs);

    /* count EVERYTHING (weights + biases + LN gamma/beta) so the
     * banner matches DTR_WEIGHT_FLOATS — the old count said 568 and
     * silently dropped the 67 bias/LN params */
    INT total_params = DTR_WEIGHT_FLOATS;

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

        /* K-DDS LATEST_ONLY QoS re-delivers the latched value on every
         * poll, so remember the last req_id we served per topic and skip
         * duplicates — otherwise the worker re-publishes the same answer
         * dozens of times per request and floods the relay. */
        UW last_l0_req    = 0;
        UW last_input_req = 0;

        for (;;) {
            /* dtr/l0 と dtr/input を交互にポーリング */

            /* --- Pipeline Parallel: dtr/l0 受信 (FULL mode) --- */
            {
                DTR_ACT act;
                W r = kdds_sub(h_l0_sub, &act, (W)sizeof(act), 0);
                if (r >= (W)sizeof(DTR_ACT) && act.magic == DTR_ACT_MAGIC &&
                    act.req_id != last_l0_req) {
                    last_l0_req = act.req_id;
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
                if (r >= (W)sizeof(DTR_INPUT) && inp.magic == DTR_INPUT_MAGIC &&
                    inp.req_id != last_input_req) {
                    last_input_req = inp.req_id;
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
    /* GA 評価中は推論をスキップ */
    if (dtr_ga_busy) return -1;

    dtr_stats.inferences++;
    dmn_trigger();   /* 推論リクエスト = 外部刺激 → DMN を ACTIVE に */
    UB lvl = degrade_level();

    /* ========================== SOLO ========================== */
    if (drpc_my_node == 0xFF || lvl == DEGRADE_SOLO) {
        float scores[DOUT];
        UB cls = run_transformer_local(input, scores);
        dtr_stats.local++;

        /* max softmax → confidence_pct */
        float mx = scores[0];
        for (INT c = 1; c < DOUT; c++) if (scores[c] > mx) mx = scores[c];
        dtr_log_push(input, cls, (UB)(mx * 100.0f));

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
        UB cls = run_cls_head(pool, input, scores);

        if (er == E_OK) dtr_stats.tp_distributed++;

        static const char *cn[] = {"normal", "alert", "critical"};
        dt_puts("[dtr] TP(REDUCED): class="); dt_putdec((UW)cls);
        dt_puts(" ("); dt_puts(cn[cls < 3 ? cls : 0]);
        dt_puts(") scores=[");
        dt_putf2(scores[0]); dt_puts(" ");
        dt_putf2(scores[1]); dt_puts(" ");
        dt_putf2(scores[2]); dt_puts("]\r\n");
        /* 配線②: REDUCED/TP 完了点も §8 反射層へ繋ぐ (この経路は dtr_log_push
         * を通らないので、推論完了点で明示フックする)。 */
        { float mx = scores[0];
          for (INT c = 1; c < DOUT; c++) if (scores[c] > mx) mx = scores[c];
          reflex_on_inference(cls, (UB)(mx * 100.0f), drpc_my_node); }
        return (W)cls;
    }

    /* ======================= FULL: Distributed KV Attention (Phase 10) === */
    if (lvl == DEGRADE_FULL && h_l0_pub >= 0) {
        BOOL has_peers = FALSE;
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n != drpc_my_node && dnode_table[n].state == DNODE_ALIVE) {
                has_peers = TRUE; break;
            }
        }
        if (has_peers && drpc_my_node % 2 == 0) {
            /* --- Phase 10: DKVA で Attention を計算 --- */
            float tok[SEQ][DM];
            run_embed_seq(input, tok);
            dtr_stats.layer0_runs++;

            /* Q を計算 */
            float Q[SEQ][NH][DH];
            for (INT h = 0; h < NH; h++)
                for (INT t = 0; t < SEQ; t++)
                    dt_linear((float *)W_q[h], NULL, tok[t], Q[t][h], DH, DM);

            /* 分散 KV Attention 試行 */
            float mhsa_dkva[SEQ][DM];
            ER dkva_er = dkva_infer(Q, W_o, mhsa_dkva, ++dtr_req_counter);

            float mhsa[SEQ][DM];
            if (dkva_er == E_OK) {
                /* DKVA 成功: 結果を使用 */
                for (INT t = 0; t < SEQ; t++)
                    for (INT d = 0; d < DM; d++)
                        mhsa[t][d] = mhsa_dkva[t][d];
                dtr_stats.distributed++;
                dt_puts("[dtr] DKVA(FULL): Attention from cluster\r\n");
            } else {
                /* フォールバック: ローカル MHSA */
                run_mhsa_local(tok, mhsa);
                dt_puts("[dtr] DKVA fallback to local MHSA\r\n");
            }

            /* 残差 + LN1 + FFN + 残差 + LN2 + Pool + Cls */
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
            float scores_dkva[DOUT];
            UB cls_dkva = run_cls_head(pool, input, scores_dkva);

            float mx = scores_dkva[0];
            for (INT c = 1; c < DOUT; c++) if (scores_dkva[c] > mx) mx = scores_dkva[c];
            dtr_log_push(input, cls_dkva, (UB)(mx * 100.0f));

            static const char *cn_dkva[] = {"normal", "alert", "critical"};
            dt_puts("[dtr] DKVA class="); dt_putdec((UW)cls_dkva);
            dt_puts(" ("); dt_puts(cn_dkva[cls_dkva < 3 ? cls_dkva : 0]);
            dt_puts(") scores=[");
            dt_putf2(scores_dkva[0]); dt_puts(" ");
            dt_putf2(scores_dkva[1]); dt_puts(" ");
            dt_putf2(scores_dkva[2]); dt_puts("]\r\n");
            return (W)cls_dkva;
        }
    }

    /* ======================= FULL: Pipeline Parallel (フォールバック) == */
    if (h_l0_pub >= 0) {
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
