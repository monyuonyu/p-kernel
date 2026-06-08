/*
 *  spec.c — R3b "呼吸するパラメータ" (breathing parameters)
 *
 *  監査 §4 への直接の答え。これまで MoE の各エキスパート (= ノード) は全員が
 *  同じ 635 パラメータを持っていた = 「混合」ではなく「同一コピーが N 個」。
 *  本モジュールは各エキスパートを別々に専門分化させ、参加が増えると賢く
 *  なり (join)、減ると優雅に劣化する (leave) ことを数で示す。
 *
 *  専門分化の機構 (採用理由は r3b-breathing-params.md):
 *    (a) データシャード specialization — 専門家 c は担当クラス c を強く
 *        オーバーサンプルした訓練集合で学習し、そのクラスの検出器になる。
 *    (b) 別 seed 初期化 (dtr_reinit_weights) — 各専門家は別の初期重みから
 *        学習し、自然に別解へ収束する。
 *  ルーティングは「訓練バンド (= 温度で定義される入力領域)」判定 (sp_band)。
 *  入力の領域に対応する専門家だけを疎に発火させる (専門外は発火しない)。専門家が
 *  不在ならジェネラリストへ迂回 = それ自体が縮退 (道B のルーター迂回)。
 *
 *  ── ONE BRAIN (wave 18) honest note ──────────────────────────────────
 *  spec のゲートは *バンド (入力領域)* を当てる関数で、出力クラス y を当てる
 *  分類器ではない (sp_ds_init: y=(base+band)%3 ≠ band)。live な moe ゲート
 *  (moe_gate_predict) は wave 18 で学習 dtr の argmax = *クラス* 予測になった。
 *  spec をその dtr ゲートに載せると「予測したいクラスで専門家をルーティング
 *  する」循環になり、しかも専門家学習中は dtr 重みが churn して領域分割が
 *  壊れる。よって spec は専用のバンド分割器 sp_band を持つ (両者は別関数で
 *  あることが正しい)。docs/review-2026-06-three-brains.md。
 *
 *  arch/common 規律: <string.h> 不使用、固定幅型、大ローカルを task stack に
 *  置かない (専門家重み・データ集合はすべて static)、出力は sio_send_frame。
 *  純ローカル計算 (net/kdds 不使用) なのでベアメタルでもコンパイル・実行可。
 */

#include "spec.h"
#include "dtr.h"
#include "moe.h"
#include "retrieval.h"
#include "pfs_dag.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* 出力ヘルパー (dtr_train.c と同形)                                    */
/* ------------------------------------------------------------------ */

static void sp_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void sp_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { sp_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    sp_puts(&buf[i]);
}

/* xx.x — 正答率 (%) 表示 */
static void sp_putf1(float f)
{
    if (f < 0.0f) { sp_puts("-"); f = -f; }
    UW whole = (UW)f;
    UW frac  = (UW)((f - (float)whole) * 10.0f + 0.5f);
    if (frac >= 10) { whole++; frac = 0; }
    sp_putdec(whole); sp_puts("."); sp_putdec(frac);
}

/* 右詰め整数 (列を揃える) */
static void sp_putdec_w(UW v, INT width)
{
    UW t = v; INT digits = 1;
    while (t >= 10) { t /= 10; digits++; }
    for (INT i = digits; i < width; i++) sp_puts(" ");
    sp_putdec(v);
}

/* ------------------------------------------------------------------ */
/* データ集合 — 専門分化が *本物* になるよう設計 (玩具・決定的・ABI 不変) */
/*                                                                     */
/* 設計の肝 (canonical MoE): 入力空間を温度で 3 つの「専門領域」(band) に */
/* 分け、各 band では真ラベルが *互いに両立しない規則* で決まる。        */
/*   全 band 共通: 連続特徴 = 光量(light)。base = band3(light)。          */
/*   ラベル y = (base + band) % 3  ← band ごとに出力を巡回シフト。        */
/* → 同じ光量でも温度帯が違えばクラスが変わる。単一の generalist は       */
/*   温度で内部分岐して 3 つの巡回を 1 つの 635 パラメータで覚えねばならず */
/*   妥協する (どの領域でも中途半端)。各 band 専門家は自分の巡回 1 つだけ */
/*   学べばよい (= dtr の素タスク同等の易しさ) ので、その領域で必ず勝つ。 */
/* この *対称* な設計で 3 専門家すべてが generalist を上回り、join が単調に */
/* 伸びる (= 監査 §4「混合 > 同一コピー」を数で成立)。                    */
/* 領域ゲート (sp_band = 温度帯) はほぼ正確になるよう温度ノイズは小。 */
/* 規則には特徴ノイズを乗せ完全分離はしない (Bayes 誤差 > 0 = 正直)。     */
/* ------------------------------------------------------------------ */

#define SP_CLASSES   3               /* 出力クラス 0/1/2 (= dtr 3-way head) */
#define SP_BANDS     3               /* 専門領域 = 温度帯 (gate と一致)     */
#define SP_N         360
#define SP_TRAIN     270
#define SP_TEST      (SP_N - SP_TRAIN)
#define SP_SEED      0xB1EA7720UL    /* "breathe" 用固定 seed             */

static B  sp_x[SP_N][DTR_SEQ_LEN];
static UB sp_y[SP_N];
static UB sp_ready = 0;

static UW sp_rng;
static UW sp_rand(void)
{
    sp_rng = sp_rng * 1664525UL + 1013904223UL;
    return (sp_rng >> 16) & 0x7FFF;
}
static INT sp_uniform(INT lo, INT hi) { return lo + (INT)(sp_rand() % (UW)(hi - lo + 1)); }
static INT sp_noise(INT s)            /* approx-gaussian: 4 uniforms 合算 */
{
    INT v = 0;
    for (INT i = 0; i < 4; i++) v += sp_uniform(-s, s);
    return v / 2;
}
static B sp_clamp(INT v)
{
    if (v >  127) v =  127;
    if (v < -128) v = -128;
    return (B)v;
}

/* しきい値 3 分割 (ノイズ込みの値 q を 3 クラスへ) */
static UB sp_band3(INT q, INT lo, INT hi)
{
    if (q < lo) return 0;
    if (q < hi) return 1;
    return 2;
}

/* spec の領域ゲート: 入力を「訓練バンド (= 温度で定義される入力領域)」へ
 * 分割する。sp_ds_init で band は温度で構築される (band0 <20 / band1 20-34 /
 * band2 >=35) ので、温度しきい値での分割が *この合成データの領域構造に対して
 * 正しい* (= 情報の破棄ではない)。これは live のクラス分類ゲート (moe_infer の
 * 学習 dtr) とは別関数 — 冒頭 honest note 参照。 */
static UB sp_band(B temp, B hum, B press, B light)
{
    (void)hum; (void)press; (void)light;   /* band は温度で定義される領域ラベル */
    if (temp < 20) return 0;
    if (temp < 35) return 1;
    return 2;
}

static void sp_ds_init(void)
{
    if (sp_ready) return;
    sp_rng = SP_SEED;
    for (INT i = 0; i < SP_N; i++) {
        UB band = (UB)(i % SP_BANDS);         /* band-balanced 両 split    */

        /* 温度: band の中心 + 小ノイズ → gate(温度) はほぼ band に一致。 */
        INT temp;
        if (band == 0)      temp = sp_uniform(  0, 15) + sp_noise(3);   /* <20  */
        else if (band == 1) temp = sp_uniform( 23, 31) + sp_noise(2);   /* 20-34*/
        else                temp = sp_uniform( 42, 70) + sp_noise(3);   /* >=35 */

        /* 連続特徴 light が判別情報、hum/press は distractor (band 横断で
         * 同分布 = 温度では分けられない)。 */
        INT light = sp_uniform(-90, 90);
        INT hum   = sp_uniform(-90, 90);
        INT press = sp_uniform(-40, 90);      /* 純ノイズ (distractor)     */

        /* base を light のしきい値で 3 分割し、band で巡回シフト。同じ light
         * でも band が違えばクラスが変わる = generalist には両立不能。 */
        UB base = sp_band3(light + sp_noise(16), -30, 30);
        UB y    = (UB)((base + band) % SP_CLASSES);

        sp_x[i][0] = sp_clamp(temp);
        sp_x[i][1] = sp_clamp(hum);
        sp_x[i][2] = sp_clamp(press);
        sp_x[i][3] = sp_clamp(light);
        sp_y[i]    = y;
    }
    sp_ready = 1;
}

/* ------------------------------------------------------------------ */
/* エキスパート (= ニューロン群 / ノード) と専門分化                    */
/*                                                                     */
/*   expert 0 : generalist     (specialty 0xFF) — 全 band を 1 個で学習  */
/*   expert 1 : specialist band 0 (cold;  ラベル=湿度規則)               */
/*   expert 2 : specialist band 1 (warm;  ラベル=光量規則)               */
/*   expert 3 : specialist band 2 (hot;   ラベル=湿度+光量規則)          */
/* 各専門家は別 seed (異なる初期化) + 自 band の routed shard だけで学習。 */
/* 専門家は自分の band の規則だけを学べばよい = その領域で generalist 超え。*/
/* ------------------------------------------------------------------ */

#define SP_MAXEXP    4
#define SP_EPOCHS    260

static float sp_ew[SP_MAXEXP][DTR_WEIGHT_FLOATS];   /* 専門家ごとの重み   */
static UB    sp_specialty[SP_MAXEXP] = { 0xFF, 0, 1, 2 };   /* band or 0xFF */
static UW    sp_seed[SP_MAXEXP]      = { 0x11111111UL, 0x22222222UL,
                                         0x33333333UL, 0x44444444UL };
static UB    sp_trained = 0;

/* 専門家ごとの routed-shard 訓練集合バッファ — static (task stack を汚さない) */
static B  sp_trx[SP_TRAIN][DTR_SEQ_LEN];
static UB sp_try[SP_TRAIN];

/* specialty (0xFF=全 band) の routed shard を組み立て、長さを返す。
 * routing は spec の領域ゲート sp_band — 専門家は自分が実際に発火させられる
 * 入力領域 (温度バンド) の分布だけで学ぶ (canonical MoE)。 */
static UW sp_build_trainset(UB specialty)
{
    UW m = 0;
    for (INT i = 0; i < SP_TRAIN; i++) {
        if (specialty != 0xFF) {
            UB g = sp_band(sp_x[i][0], sp_x[i][1], sp_x[i][2], sp_x[i][3]);
            if (g != specialty) continue;
        }
        for (INT t = 0; t < DTR_SEQ_LEN; t++) sp_trx[m][t] = sp_x[i][t];
        sp_try[m] = sp_y[i];
        m++;
    }
    return m;
}

/* 1 専門家を一から学習し、その 635 パラメータを sp_ew[e] へ凍結する。 */
static void sp_train_expert(INT e)
{
    dtr_reinit_weights(sp_seed[e]);              /* (b) 別 seed 初期化     */
    UW n = sp_build_trainset(sp_specialty[e]);   /* (a) routed データシャード */

    UB rprev = ret_set(0);                        /* 学習は素の重みで       */
    for (UW ep = 1; ep <= SP_EPOCHS; ep++) {
        float lr = (ep <= SP_EPOCHS / 2) ? 0.10f : 0.05f;
        (void)dtr_train_batch(sp_trx, sp_try, n, lr);
    }
    ret_set(rprev);

    dtr_weights_get(sp_ew[e]);
}

/* ------------------------------------------------------------------ */
/* ルーティング (§7 ゲートで疎に発火) と評価                            */
/* ------------------------------------------------------------------ */

/* active = 参加中エキスパートのビットマスク。gate band に専門家が居れば
 * それを発火、不在ならジェネラリストへ迂回、それも無ければ最小の active。
 * 「専門家不在 → 迂回」が道B のルーター迂回 = 縮退の局在化。 */
static INT sp_route(UW active, UB gate)
{
    for (INT e = 0; e < SP_MAXEXP; e++)
        if ((active & (1u << e)) && sp_specialty[e] == gate) return e;
    for (INT e = 0; e < SP_MAXEXP; e++)
        if ((active & (1u << e)) && sp_specialty[e] == 0xFF) return e;
    for (INT e = 0; e < SP_MAXEXP; e++)
        if (active & (1u << e)) return e;
    return 0;
}

/* test split を active チームで評価。per-band (= 専門領域) 正解/総数を埋め、
 * 全体 acc (%) を返す。専門家へ実際に発火 (dtr_weights_set + forward)。 */
static float sp_eval_team(UW active, UW dom_correct[SP_BANDS],
                          UW dom_total[SP_BANDS])
{
    for (INT b = 0; b < SP_BANDS; b++) { dom_correct[b] = 0; dom_total[b] = 0; }
    UW correct = 0;
    INT last_e = -1;
    for (INT i = SP_TRAIN; i < SP_N; i++) {
        const B *x = sp_x[i];
        UB gate = sp_band(x[0], x[1], x[2], x[3]);   /* spec 領域ゲート */
        INT e   = sp_route(active, gate);
        if (e != last_e) { dtr_weights_set(sp_ew[e]); last_e = e; }
        float probs[DTR_OUT_DIM];
        dtr_forward_probs(x, probs);
        UB pred = 0;
        for (INT c = 1; c < DTR_OUT_DIM; c++) if (probs[c] > probs[pred]) pred = (UB)c;
        UB dom = gate < SP_BANDS ? gate : 0;
        dom_total[dom]++;
        if (pred == sp_y[i]) { dom_correct[dom]++; correct++; }
    }
    return (float)correct * 100.0f / (float)SP_TEST;
}

static float sp_team_acc(UW active)
{
    UW dc[SP_BANDS], dt[SP_BANDS];
    return sp_eval_team(active, dc, dt);
}

/* per-domain (専門領域) 行を表で出す */
static void sp_print_row(const char *label, float acc,
                         const UW dc[SP_BANDS], const UW dt[SP_BANDS])
{
    sp_puts(label);
    sp_puts("  overall ");
    sp_putf1(acc); sp_puts("%   per-domain[");
    for (INT b = 0; b < SP_BANDS; b++) {
        float a = dt[b] ? (float)dc[b] * 100.0f / (float)dt[b] : 0.0f;
        sp_puts("d"); sp_putdec((UW)b); sp_puts("=");
        sp_putf1(a); sp_puts("%");
        if (b < SP_BANDS - 1) sp_puts(" ");
    }
    sp_puts("]\r\n");
}

/* ------------------------------------------------------------------ */
/* 直近実行の結果 (breathe stat 用)                                     */
/* ------------------------------------------------------------------ */

static float r_join[SP_MAXEXP + 1];     /* r_join[N] = N 専門家での acc   */
static float r_copies = 0.0f;           /* generalist ×4 コピーの acc     */
static float r_full = 0.0f, r_leave = 0.0f;
static UB    r_have = 0;

/* ------------------------------------------------------------------ */
/* 本体: 学習 → join 上昇 → leave 優雅劣化 → self-check                  */
/* ------------------------------------------------------------------ */

static INT sp_run(void)
{
    INT fails = 0;
    sp_ds_init();

    /* 既存 dtr デモを壊さないよう、現在の重みを退避し最後に復元する。 */
    static float saved[DTR_WEIGHT_FLOATS];
    dtr_weights_get(saved);
    dtr_ga_busy = 1;                      /* 学習中は dtr_infer を止める   */

    sp_puts("[breathe] R3b breathing parameters — expert specialization\r\n");
    sp_puts("[breathe] dataset: "); sp_putdec(SP_N);
    sp_puts(" hard samples ("); sp_putdec(SP_TRAIN); sp_puts(" train / ");
    sp_putdec(SP_TEST); sp_puts(" held-out), 3 classes\r\n");
    sp_puts("[breathe] training 1 generalist + 3 band-specialists"
            " (per-expert seed + routed-shard learning)...\r\n");

    SYSTIM t0, t1; tk_get_tim(&t0);
    for (INT e = 0; e < SP_MAXEXP; e++) sp_train_expert(e);
    tk_get_tim(&t1);
    sp_trained = 1;
    sp_puts("[breathe] trained "); sp_putdec(SP_MAXEXP);
    sp_puts(" experts in "); sp_putdec((UW)(t1.lo - t0.lo)); sp_puts(" ms\r\n");

    /* --- CONTROL: 専門分化が本物である証拠 (= 同一コピーではない) ---
     * generalist を 4 個並べても (= 全ノード同一重み = 監査 §4 の旧状態)
     * 1 個と同じ accuracy にしかならない。これが join で動かないこと自体が
     * 「コピーでは賢くならない」帰無仮説。 */
    r_copies = sp_team_acc(0x1);          /* {gen} 単体 = コピー基準       */
    sp_puts("[breathe] control (all-same-weights, audit §4 old state):"
            " N copies == 1 -> acc fixed at ");
    sp_putf1(r_copies); sp_puts("%\r\n");

    /* --- JOIN: 参加が増えると賢くなる --- */
    sp_puts("[breathe] === JOIN (add band specialists) ===\r\n");
    /* 専門家を追加する順序。generalist は自分が偶然得意な領域 (= 巡回 0 の
     * band) を既に高精度でこなすので、その専門家は冗長 (追加しても平ら)。
     * headroom の大きい領域の専門家から足すと毎段が賢くなる。
     * expert: 0=gen 1=d0 2=d1 3=d2 → 追加順 d1,d2,d0。 */
    UW teams[SP_MAXEXP + 1] = {
        0,                         /* (unused)                              */
        0x1,                       /* N=1: {gen}                            */
        0x1 | (1u << 2),           /* N=2: {gen, d1-spec}                   */
        0x1 | (1u << 2) | (1u << 3),/* N=3: {gen, d1, d2}                   */
        0xF                        /* N=4: {gen, d1, d2, d0}                */
    };
    UW dc[SP_BANDS], dt[SP_BANDS];
    for (INT N = 1; N <= SP_MAXEXP; N++) {
        float acc = sp_eval_team(teams[N], dc, dt);
        r_join[N] = acc;
        sp_puts("  N="); sp_putdec((UW)N); sp_puts(" experts");
        sp_print_row("", acc, dc, dt);
    }
    r_full = r_join[SP_MAXEXP];

    /* --- LEAVE: 1 ノード退場で優雅に劣化 (崖落ちでなく) --- */
    sp_puts("[breathe] === LEAVE (kill 1 of 4: specialist for domain 1) ===\r\n");
    UW full = 0xF, killed = full & ~(1u << 2);   /* expert 2 = domain1 専門 */
    UW fcc[SP_BANDS], fct[SP_BANDS], kcc[SP_BANDS], kct[SP_BANDS];
    (void)sp_eval_team(full,   fcc, fct);
    r_leave = sp_eval_team(killed, kcc, kct);
    sp_print_row("  before (4 experts)", r_full, fcc, fct);
    sp_print_row("  after  (killed d1) ", r_leave, kcc, kct);

    /* ============================ self-check ============================ */
    sp_puts("[breathe] ---- self-check ----\r\n");

    /* (1) join 単調非減少 + 「1 < 2 < 4」(専門分化が効いている)。 */
    INT mono = 1;
    for (INT N = 2; N <= SP_MAXEXP; N++)
        if (r_join[N] + 0.05f < r_join[N - 1]) mono = 0;
    float gain = r_full - r_join[1];
    sp_puts("  join: 1->"); sp_putf1(r_join[1]);
    sp_puts("%  2->"); sp_putf1(r_join[2]);
    sp_puts("%  4->"); sp_putf1(r_full);
    sp_puts("%  gain="); sp_putf1(gain); sp_puts("pt");
    sp_puts(mono ? "  monotone" : "  NON-MONOTONE"); sp_puts("\r\n");
    if (!mono)                       { sp_puts("  FAIL join not monotone\r\n"); fails++; }
    if (r_join[2] <= r_join[1] + 0.05f) { sp_puts("  FAIL join 1<2 not strict (2nd expert added nothing)\r\n"); fails++; }
    if (r_full   <= r_join[2] + 0.05f)  { sp_puts("  FAIL join 2<4 not strict (more experts added nothing)\r\n"); fails++; }
    if (gain < 5.0f)                 { sp_puts("  FAIL join gain < 5pt (no real mixture)\r\n"); fails++; }

    /* (2) コピーは賢くならない: control(=1 個) と 4 個コピーは同じはず。
     * ここでは control == r_join[1] を再掲し、4 専門家がそれを上回ることで
     * 「コピーでなく分化」を主張する (gain>0 で既に担保)。 */
    if (r_full <= r_copies + 0.05f) {
        sp_puts("  FAIL 4 experts no better than copies (no specialization)\r\n");
        fails++;
    }

    /* (3) leave 優雅劣化: 殺した専門の領域 (d1) は落ち、他領域は保たれ、
     * 全体は崖落ちしない (control 基準=N コピー より上を維持)。 */
    float d1_before = fct[1] ? (float)fcc[1] * 100.0f / (float)fct[1] : 0.0f;
    float d1_after  = kct[1] ? (float)kcc[1] * 100.0f / (float)kct[1] : 0.0f;
    float d0_before = fct[0] ? (float)fcc[0] * 100.0f / (float)fct[0] : 0.0f;
    float d0_after  = kct[0] ? (float)kcc[0] * 100.0f / (float)kct[0] : 0.0f;
    float d2_before = fct[2] ? (float)fcc[2] * 100.0f / (float)fct[2] : 0.0f;
    float d2_after  = kct[2] ? (float)kcc[2] * 100.0f / (float)kct[2] : 0.0f;
    sp_puts("  leave: d1 "); sp_putf1(d1_before); sp_puts("%->");
    sp_putf1(d1_after); sp_puts("%  (others held: d0 ");
    sp_putf1(d0_before); sp_puts("->"); sp_putf1(d0_after);
    sp_puts(" d2 "); sp_putf1(d2_before); sp_puts("->"); sp_putf1(d2_after);
    sp_puts(")\r\n");
    /* 殺した領域は劣化する (それ自体が縮退の局在性)。 */
    if (d1_after >= d1_before) {
        sp_puts("  FAIL killed domain did not degrade (router not routing?)\r\n");
        fails++;
    }
    /* 他領域は保たれる (劣化が局在 = 優雅; 数 pt の揺れは許容)。 */
    if (d0_after + 8.0f < d0_before || d2_after + 8.0f < d2_before) {
        sp_puts("  FAIL non-killed domains collapsed (not localized)\r\n");
        fails++;
    }
    /* 全体は崖落ちしない: コピー基準より上を維持 (= まだ価値がある)。 */
    if (r_leave + 0.05f < r_copies) {
        sp_puts("  FAIL overall fell below single-copy baseline (cliff)\r\n");
        fails++;
    }

    if (fails == 0) sp_puts("[breathe] PASS (join smarter; leave graceful)\r\n");
    else { sp_puts("[breathe] FAIL count="); sp_putdec((UW)fails); sp_puts("\r\n"); }

    r_have = 1;

    /* 復元: 既存 dtr デモ (train/eval/save/load) の数値を壊さない。 */
    dtr_weights_set(saved);
    dtr_ga_busy = 0;
    return fails;
}

/* ------------------------------------------------------------------ */
/* breathe save — 専門家を p-fs の named ref として永続化 (器が増える)   */
/* ------------------------------------------------------------------ */

static struct __attribute__((packed, aligned(4))) {
    DTR_WBLOB_HDR h;
    float         w[DTR_WEIGHT_FLOATS];
} sp_blob;

_Static_assert(sizeof(sp_blob) == 20 + DTR_WEIGHT_FLOATS * 4,
               "expert blob = header + 635 packed float32");

static void sp_save_experts(void)
{
    if (!sp_trained) { sp_puts("[breathe] save: run `breathe` first\r\n"); return; }
    /* ref = "dtr/expert/<k>" — エキスパートが p-fs 上の content-addressed
     * ブロックとして増える = 「呼吸するパラメータ」の器。 */
    char ref[16] = { 'd','t','r','/','e','x','p','e','r','t','/','0',0,0,0,0 };
    for (INT e = 0; e < SP_MAXEXP; e++) {
        ref[11] = (char)('0' + e);
        sp_blob.h.magic    = DTR_WBLOB_MAGIC;
        sp_blob.h.version  = DTR_WBLOB_VER;
        sp_blob.h.n_params = DTR_WEIGHT_FLOATS;
        sp_blob.h.d_model  = DTR_EMBED_DIM;
        sp_blob.h.n_heads  = DTR_NUM_HEADS;
        sp_blob.h.seq_len  = DTR_SEQ_LEN;
        sp_blob.h.ffn_dim  = DTR_FFN_DIM;
        sp_blob.h.out_dim  = DTR_OUT_DIM;
        sp_blob.h._pad[0] = sp_blob.h._pad[1] = sp_blob.h._pad[2] = 0;
        for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) sp_blob.w[i] = sp_ew[e][i];

        INT r = pfs_dag_save((const UB *)ref, 12, &sp_blob, (UW)sizeof(sp_blob));
        sp_puts("[breathe] expert "); sp_putdec((UW)e);
        sp_puts(" (specialty ");
        if (sp_specialty[e] == 0xFF) sp_puts("generalist");
        else { sp_puts("domain "); sp_putdec(sp_specialty[e]); }
        sp_puts(") -> '"); sp_puts(ref); sp_puts("' ");
        sp_puts(r == PFS_OK ? "saved\r\n" : "FAILED\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

INT breathe_self_test(void) { return sp_run(); }

static void sp_stat(void)
{
    if (!r_have) { sp_puts("[breathe] no run yet — type `breathe`\r\n"); return; }
    sp_puts("[breathe] last run: copies="); sp_putf1(r_copies);
    sp_puts("%  join 1.."); sp_putdec(SP_MAXEXP); sp_puts("=[");
    for (INT N = 1; N <= SP_MAXEXP; N++) {
        sp_putf1(r_join[N]); sp_puts(N < SP_MAXEXP ? " " : "");
    }
    sp_puts("]%  leave(kill d1)="); sp_putf1(r_leave); sp_puts("%\r\n");
    (void)sp_putdec_w;
}

static INT sp_tok(const UB *p, const UB *end, const char *kw)
{
    INT i = 0;
    while (kw[i]) { if (p + i >= end || p[i] != (UB)kw[i]) return 0; i++; }
    return (p + i == end || p[i] == ' ' || p[i] == '\t');
}

void breathe_cmd(const UB *args, UW len)
{
    const UB *end = args + len;
    const UB *p   = args;
    while (p < end && (*p == ' ' || *p == '\t')) p++;

    if (p >= end || sp_tok(p, end, "run")) { sp_run();       return; }
    if (sp_tok(p, end, "save"))            { sp_save_experts(); return; }
    if (sp_tok(p, end, "stat"))            { sp_stat();      return; }

    sp_puts("usage: breathe [run] | breathe save | breathe stat\r\n");
}
