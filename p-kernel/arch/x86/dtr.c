/*
 *  dtr.c (x86)
 *  Phase 8 — Distributed Transformer Inference (Pipeline Parallelism)
 *
 *  LLM・大規模モデルをノード間で分割して動かすための実装。
 *  K-DDS トピックが中間活性化テンソルの転送バスとして機能する。
 *
 *  同じコードを全ノードで動かす。drpc_my_node の偶奇で役割が決まる。
 *
 *     ┌─────────────────┐  dtr/l0  ┌──────────────────────┐
 *     │ Node 0 (even)   │ ───────► │ Node 1 (odd)         │
 *     │  dtr_infer()    │          │                      │
 *     │  Stage 0:       │          │  Stage 1: FFN        │
 *     │  Embed(4→8)     │          │  8→16→8              │
 *     │  Layer0(8→8)    │ ◄─────── │  Stage 2: OutputHead │
 *     │  [wait result]  │ dtr/result│  8→3 + Softmax      │
 *     └─────────────────┘          └──────────────────────┘
 *
 *  モデルの重みは全ノードで同じ LCG シードから初期化するため同一。
 *  学習 (Phase 8 以降) で重みを更新した場合は fedlearn で同期する。
 */

#include "dtr.h"
#include "kdds.h"
#include "drpc.h"
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

/* exp(x): Taylor 展開, |x| <= 5 で十分な精度 */
static float dt_exp(float x)
{
    if (x >  10.0f) return 22026.0f;
    if (x < -10.0f) return 0.0f;
    /* Horner 法: 1 + x + x²/2! + x³/3! + x⁴/4! + x⁵/5! + x⁶/6! */
    float r = 1.0f + x * (1.0f + x * (0.5f + x * (0.16667f +
              x * (0.04167f + x * (0.00833f + x * 0.00139f)))));
    return r < 1e-10f ? 1e-10f : r;
}

/* y[M] = W[M×N] · x[N] + b[M]  (row-major) */
static void dt_linear(const float *W, const float *b, const float *x,
                      float *y, INT M, INT N)
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

/* ------------------------------------------------------------------ */
/* モデル重み (全ノードで同一の LCG シードから初期化)                 */
/* ------------------------------------------------------------------ */

/* 次元定義 */
#define EMB_IN    4   /* センサ入力次元 */
#define EMB_OUT   8   /* 埋め込み次元   */
#define L0_IN     8
#define L0_OUT    8
#define L1A_OUT  16   /* FFN 展開       */
#define L1B_OUT   8   /* FFN 収縮       */
#define OUT_IN    8
#define OUT_OUT   3

/* 重みテーブル */
static float W_emb [EMB_OUT][EMB_IN ];   /* 32 floats   */
static float b_emb [EMB_OUT          ];  /*  8 floats   */
static float W_l0  [L0_OUT ][L0_IN  ];  /* 64 floats   */
static float b_l0  [L0_OUT           ];  /*  8 floats   */
static float W_l1a [L1A_OUT][L1B_OUT];   /* 128 floats  */
static float b_l1a [L1A_OUT          ];  /* 16 floats   */
static float W_l1b [L1B_OUT][L1A_OUT];   /* 128 floats  */
static float b_l1b [L1B_OUT          ];  /*  8 floats   */
static float W_out [OUT_OUT][OUT_IN  ];  /* 24 floats   */
static float b_out [OUT_OUT          ];  /*  3 floats   */

/* LCG 疑似乱数による重み初期化 (He 初期化 スケール) */
static void init_weights(float *w, INT n, float scale, UW *seed)
{
    for (INT i = 0; i < n; i++) {
        *seed = *seed * 1664525UL + 1013904223UL;
        /* [0, 2^23) → [0,1) → [-1, 1) */
        float v = (float)((*seed >> 9) & 0x7FFFFFU) / (float)(1 << 23) * 2.0f - 1.0f;
        w[i] = v * scale;
    }
}

static void init_zeros(float *w, INT n)
{
    for (INT i = 0; i < n; i++) w[i] = 0.0f;
}

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

DTR_STATS dtr_stats;

static ID  dtr_result_sem  = -1;   /* dtr_infer() が結果待ちに使う  */
static DTR_RESULT dtr_last_result; /* dtr_task が書き込む最終結果   */
static UW  dtr_req_counter = 0;    /* リクエスト ID カウンタ        */

/* K-DDS ハンドル (dtr_task で設定) */
static W h_l0_pub  = -1;   /* node 0: "dtr/l0" へ pub     */
static W h_l0_sub  = -1;   /* node 1: "dtr/l0" を sub     */
static W h_res_pub = -1;   /* node 1: "dtr/result" へ pub */
static W h_res_sub = -1;   /* node 0: "dtr/result" を sub */

/* ------------------------------------------------------------------ */
/* ステージ計算関数                                                    */
/* ------------------------------------------------------------------ */

/*
 *  Stage 0: Embed(int8[4]→float[8]) + Layer0(8→8, Linear+ReLU)
 *  Node 0 が担当。結果を "dtr/l0" へ pub する。
 */
static void run_stage0(const B input[4], float out[L0_OUT])
{
    /* Embed: int8 → [-1, 1] に正規化してから線形変換 */
    float in_f[EMB_IN];
    for (INT i = 0; i < EMB_IN; i++) in_f[i] = (float)input[i] / 127.0f;

    float emb[EMB_OUT];
    dt_linear((float *)W_emb, b_emb, in_f, emb, EMB_OUT, EMB_IN);
    for (INT i = 0; i < EMB_OUT; i++) emb[i] = dt_relu(emb[i]);

    /* Layer 0: Linear(8→8) + ReLU */
    dt_linear((float *)W_l0, b_l0, emb, out, L0_OUT, L0_IN);
    for (INT i = 0; i < L0_OUT; i++) out[i] = dt_relu(out[i]);

    dtr_stats.layer0_runs++;
}

/*
 *  Stage 1: FFN(8→16→8, Linear+ReLU×2)
 *  Node 1 が担当。Stage 0 の出力を受け取り FFN を計算。
 */
static void run_stage1(const float in[L0_OUT], float out[L1B_OUT])
{
    float mid[L1A_OUT];

    /* FFN expand: 8→16 */
    dt_linear((float *)W_l1a, b_l1a, in, mid, L1A_OUT, L0_OUT);
    for (INT i = 0; i < L1A_OUT; i++) mid[i] = dt_relu(mid[i]);

    /* FFN contract: 16→8 */
    dt_linear((float *)W_l1b, b_l1b, mid, out, L1B_OUT, L1A_OUT);
    for (INT i = 0; i < L1B_OUT; i++) out[i] = dt_relu(out[i]);

    dtr_stats.layer1_runs++;
}

/*
 *  Stage 2: OutputHead(8→3) + Softmax → class [0,1,2]
 *  Node 1 が担当 (2ノード構成)。結果を "dtr/result" へ pub。
 */
static UB run_stage2(const float in[OUT_IN], float scores[OUT_OUT])
{
    dt_linear((float *)W_out, b_out, in, scores, OUT_OUT, OUT_IN);
    dt_softmax(scores, OUT_OUT);

    UB cls = 0;
    for (INT i = 1; i < OUT_OUT; i++)
        if (scores[i] > scores[cls]) cls = (UB)i;

    dtr_stats.output_runs++;
    return cls;
}

/* ------------------------------------------------------------------ */
/* dtr_init                                                            */
/* ------------------------------------------------------------------ */

void dtr_init(void)
{
    /* 全ノードで同一の固定シードを使い、同じ重みを生成する */
    UW seed = 0xDEAD8888UL;
    init_weights((float *)W_emb,  EMB_OUT * EMB_IN,  0.707f, &seed);
    init_zeros  (b_emb,  EMB_OUT);
    init_weights((float *)W_l0,   L0_OUT  * L0_IN,   0.500f, &seed);
    init_zeros  (b_l0,   L0_OUT);
    init_weights((float *)W_l1a,  L1A_OUT * L1B_OUT, 0.500f, &seed);
    init_zeros  (b_l1a,  L1A_OUT);
    init_weights((float *)W_l1b,  L1B_OUT * L1A_OUT, 0.354f, &seed);
    init_zeros  (b_l1b,  L1B_OUT);
    init_weights((float *)W_out,  OUT_OUT * OUT_IN,   0.500f, &seed);
    init_zeros  (b_out,  OUT_OUT);

    /* 分散モードで dtr_infer() がブロックするセマフォ */
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
    dtr_result_sem = tk_cre_sem(&cs);

    dt_puts("[dtr] initialized  "
            "embed=4->8  l0=8->8  ffn=8->16->8  out=8->3\r\n");
    dt_puts("[dtr] params=");
    dt_putdec((UW)(EMB_OUT*EMB_IN + L0_OUT*L0_IN +
                   L1A_OUT*L1B_OUT + L1B_OUT*L1A_OUT +
                   OUT_OUT*OUT_IN));
    dt_puts(" floats\r\n");
}

/* ------------------------------------------------------------------ */
/* dtr_task — パイプラインタスク (全ノードで同じ関数を起動)           */
/* ------------------------------------------------------------------ */

void dtr_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    /* 単一ノードモード: K-DDS 不要、dtr_infer() がローカル実行 */
    if (drpc_my_node == 0xFF) {
        dt_puts("[dtr] task: single-node mode, all stages local\r\n");
        return;
    }

    UB my_node = drpc_my_node;
    /* 偶数ノード = Stage 0 担当 (Embed + Layer0, 結果収集)   */
    /* 奇数ノード = Stage 1+2 担当 (FFN + OutputHead, 結果送信) */
    BOOL is_stage0 = (my_node % 2 == 0);

    if (is_stage0) {
        /* ---- Node 0 (even): Stage 0 + 結果収集 ------------------- */
        h_l0_pub  = kdds_open(DTR_TOPIC_L0,     KDDS_QOS_LATEST_ONLY);
        h_res_sub = kdds_open(DTR_TOPIC_RESULT,  KDDS_QOS_LATEST_ONLY);

        dt_puts("[dtr] node ");
        dt_putdec((UW)my_node);
        dt_puts(": stage0 (embed+l0+output-collect)\r\n");

        for (;;) {
            DTR_RESULT res;
            W r = kdds_sub(h_res_sub, &res, (W)sizeof(res), -1);
            if (r < (W)sizeof(DTR_RESULT)) continue;
            if (res.magic != DTR_RESULT_MAGIC) continue;

            /* 結果を保存して dtr_infer() のブロックを解除 */
            dtr_last_result = res;
            tk_sig_sem(dtr_result_sem, 1);
            dtr_stats.distributed++;
        }
    } else {
        /* ---- Node 1 (odd): Stage 1+2 (FFN + OutputHead) ---------- */
        h_l0_sub  = kdds_open(DTR_TOPIC_L0,     KDDS_QOS_LATEST_ONLY);
        h_res_pub = kdds_open(DTR_TOPIC_RESULT,  KDDS_QOS_LATEST_ONLY);

        dt_puts("[dtr] node ");
        dt_putdec((UW)my_node);
        dt_puts(": stage1+2 (ffn+output-head), listening dtr/l0\r\n");

        for (;;) {
            DTR_ACT act;
            W r = kdds_sub(h_l0_sub, &act, (W)sizeof(act), -1);
            if (r < (W)sizeof(DTR_ACT)) continue;
            if (act.magic != DTR_ACT_MAGIC) continue;

            /* Stage 1: FFN 8→16→8 (packed member → ローカルコピーに展開) */
            float act_copy[DTR_EMBED_DIM];
            for (INT ai = 0; ai < DTR_EMBED_DIM; ai++) act_copy[ai] = act.act[ai];
            float ffn_out[L1B_OUT];
            run_stage1(act_copy, ffn_out);

            /* Stage 2: OutputHead 8→3 + Softmax */
            float scores_copy[OUT_OUT];
            DTR_RESULT res;
            res.magic    = DTR_RESULT_MAGIC;
            res.req_id   = act.req_id;
            res.src_node = my_node;
            res._pad     = 0;
            res.class_id = run_stage2(ffn_out, scores_copy);
            for (INT si = 0; si < OUT_OUT; si++) res.scores[si] = scores_copy[si];

            kdds_pub(h_res_pub, &res, (W)sizeof(res));

            static const char *cls_name[] = {"normal", "alert", "critical"};
            dt_puts("[dtr] stage1+2: req=");
            dt_putdec(res.req_id);
            dt_puts(" -> class=");
            dt_putdec(res.class_id);
            dt_puts(" (");
            dt_puts(cls_name[res.class_id < 3 ? res.class_id : 0]);
            dt_puts(") scores=[");
            dt_putf2(res.scores[0]); dt_puts(" ");
            dt_putf2(res.scores[1]); dt_puts(" ");
            dt_putf2(res.scores[2]);
            dt_puts("]\r\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/* dtr_infer — シェルタスクから呼び出す推論 API                       */
/* ------------------------------------------------------------------ */

W dtr_infer(const B input[4])
{
    dtr_stats.inferences++;

    /* 分散モードかつ Stage 0 ハンドルが開いているか確認 */
    BOOL has_peers = FALSE;
    if (drpc_my_node != 0xFF && h_l0_pub >= 0) {
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n != drpc_my_node && dnode_table[n].state == DNODE_ALIVE) {
                has_peers = TRUE; break;
            }
        }
    }

    if (!has_peers) {
        /* ---- ローカル実行: 全ステージをこのノードで処理 ---------- */
        float l0_out[L0_OUT], l1_out[L1B_OUT], scores[OUT_OUT];
        run_stage0(input, l0_out);
        run_stage1(l0_out, l1_out);
        UB cls = run_stage2(l1_out, scores);
        dtr_stats.local++;

        static const char *cname[] = {"normal", "alert", "critical"};
        dt_puts("[dtr] local: class=");
        dt_putdec((UW)cls);
        dt_puts(" (");
        dt_puts(cname[cls < 3 ? cls : 0]);
        dt_puts(") scores=[");
        dt_putf2(scores[0]); dt_puts(" ");
        dt_putf2(scores[1]); dt_puts(" ");
        dt_putf2(scores[2]);
        dt_puts("]\r\n");
        return (W)cls;
    }

    /* ---- 分散実行: Stage 0 → K-DDS → Stage 1+2 → 結果待ち ------ */
    float l0_out[L0_OUT];
    run_stage0(input, l0_out);

    /* DTR_ACT を組み立てて "dtr/l0" へ pub */
    DTR_ACT act;
    act.magic    = DTR_ACT_MAGIC;
    act.req_id   = ++dtr_req_counter;
    act.src_node = drpc_my_node;
    act.layer    = 1;
    act._pad     = 0;
    for (INT i = 0; i < L0_OUT; i++) act.act[i] = l0_out[i];

    dt_puts("[dtr] -> node1: req=");
    dt_putdec(act.req_id);
    dt_puts(" stage0 done, waiting result...\r\n");

    kdds_pub(h_l0_pub, &act, (W)sizeof(act));

    /* 結果待ち (DTR_INFER_TMO ms) */
    ER er = tk_wai_sem(dtr_result_sem, 1, (TMO)DTR_INFER_TMO);
    if (er != E_OK) {
        dt_puts("[dtr] timeout: node1 did not respond\r\n");
        dtr_stats.timeouts++;
        return -1;
    }

    static const char *cname[] = {"normal", "alert", "critical"};
    UB cls = dtr_last_result.class_id;
    dt_puts("[dtr] result from node");
    dt_putdec((UW)dtr_last_result.src_node);
    dt_puts(": class=");
    dt_putdec((UW)cls);
    dt_puts(" (");
    dt_puts(cname[cls < 3 ? cls : 0]);
    dt_puts(") scores=[");
    dt_putf2(dtr_last_result.scores[0]); dt_puts(" ");
    dt_putf2(dtr_last_result.scores[1]); dt_puts(" ");
    dt_putf2(dtr_last_result.scores[2]);
    dt_puts("]\r\n");

    return (W)cls;
}

/* ------------------------------------------------------------------ */
/* dtr_stat — 統計表示                                                */
/* ------------------------------------------------------------------ */

void dtr_stat(void)
{
    dt_puts("[dtr] Pipeline Parallelism Stats:\r\n");

    dt_puts("  node        : ");
    if (drpc_my_node == 0xFF) dt_puts("single");
    else dt_putdec((UW)drpc_my_node);
    dt_puts("  role: ");
    if (drpc_my_node == 0xFF)
        dt_puts("local-only");
    else if (drpc_my_node % 2 == 0)
        dt_puts("stage0 (embed+l0)");
    else
        dt_puts("stage1+2 (ffn+output)");
    dt_puts("\r\n");

    dt_puts("  inferences  : "); dt_putdec(dtr_stats.inferences);  dt_puts("\r\n");
    dt_puts("    local     : "); dt_putdec(dtr_stats.local);        dt_puts("\r\n");
    dt_puts("    distributed:"); dt_putdec(dtr_stats.distributed);  dt_puts("\r\n");
    dt_puts("    timeouts  : "); dt_putdec(dtr_stats.timeouts);     dt_puts("\r\n");
    dt_puts("  layer0 runs : "); dt_putdec(dtr_stats.layer0_runs);  dt_puts("\r\n");
    dt_puts("  layer1 runs : "); dt_putdec(dtr_stats.layer1_runs);  dt_puts("\r\n");
    dt_puts("  output runs : "); dt_putdec(dtr_stats.output_runs);  dt_puts("\r\n");

    if (dtr_last_result.magic == DTR_RESULT_MAGIC) {
        static const char *cname[] = {"normal", "alert", "critical"};
        UB cls = dtr_last_result.class_id;
        dt_puts("  last result : class=");
        dt_putdec((UW)cls);
        dt_puts(" (");
        dt_puts(cname[cls < 3 ? cls : 0]);
        dt_puts(")  scores=[");
        dt_putf2(dtr_last_result.scores[0]); dt_puts("  ");
        dt_putf2(dtr_last_result.scores[1]); dt_puts("  ");
        dt_putf2(dtr_last_result.scores[2]);
        dt_puts("]\r\n");
    }
}
