/*
 *  ga.c (x86)
 *  Phase 14 — Genetic Algorithm による Transformer 重みの自己改善
 *
 *  DMN アイドル時に dmn_idle_work() から ga_step() が呼ばれる。
 *  重みを変異させて confidence (max softmax) が上がる方向に進化させる。
 *
 *  静的バッファを使用 (スタック圧迫を避けるため):
 *    ga_best_weights[635]: 現在の最良重みのコピー
 *    ga_cand_weights[635]: 候補 (変異体) の重みバッファ
 */

#include "ga.h"
#include "dtr.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* ユーティリティ                                                      */
/* ------------------------------------------------------------------ */

static void ga_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void ga_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { ga_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    ga_puts(&buf[i]);
}

static void ga_putf2(float f)
{
    if (f < 0.0f) { ga_puts("-"); f = -f; }
    UW ii = (UW)f;
    UW fr = (UW)((f - (float)ii) * 100.0f);
    ga_putdec(ii); ga_puts(".");
    if (fr < 10) ga_puts("0");
    ga_putdec(fr);
}

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

GA_STATS ga_stats;

/* 静的重みバッファ (スタックオーバーフロー防止) */
static float ga_best_weights[DTR_WEIGHT_FLOATS];
static float ga_cand_weights[DTR_WEIGHT_FLOATS];

/* LCG 疑似乱数シード */
static UW ga_lcg_seed = 0xCA4E8A11UL;

/* ------------------------------------------------------------------ */
/* LCG 乱数ユーティリティ                                             */
/* ------------------------------------------------------------------ */

static float ga_rand_noise(void)
{
    ga_lcg_seed = ga_lcg_seed * 1664525UL + 1013904223UL;
    /* [0, 1) の一様乱数 */
    float u = (float)((ga_lcg_seed >> 9) & 0x7FFFFFU) / (float)(1 << 23);
    /* [-1, +1] にマッピングして GA_MUTATE_SCALE で縮小 */
    return (u * 2.0f - 1.0f) / (float)GA_MUTATE_SCALE;
}

/* ------------------------------------------------------------------ */
/* 変異: src の各重みに小さなノイズを加えて dst に書く               */
/* ------------------------------------------------------------------ */

static void ga_mutate(const float *src, float *dst)
{
    for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++)
        dst[i] = src[i] + ga_rand_noise();
}

/* ------------------------------------------------------------------ */
/* ga_step — 1 世代の進化                                            */
/* ------------------------------------------------------------------ */

void ga_step(void)
{
    /* ログが少なすぎる場合はスキップ */
    if (dtr_log_avail() < GA_LOG_MIN) {
        ga_stats.skipped++;
        return;
    }

    ga_stats.ga_steps++;

    /* 1. 現在の重みを best として保存 */
    dtr_weights_get(ga_best_weights);

    /* 2. 推論をブロック (dtr_infer は -1 を返すようになる) */
    dtr_ga_busy = 1;

    /* 3. baseline fitness: 現在の重みで推論ログを再評価 */
    float best_fit = dtr_eval_confidence();

    /* 4. 変異体を試す */
    for (INT k = 0; k < GA_POP_SIZE - 1; k++) {
        ga_mutate(ga_best_weights, ga_cand_weights);
        dtr_weights_set(ga_cand_weights);
        float fit = dtr_eval_confidence();

        /* baseline より有意に改善している場合のみ採用 */
        if (fit > best_fit + 0.005f) {
            best_fit = fit;
            for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++)
                ga_best_weights[i] = ga_cand_weights[i];
            ga_stats.improvements++;
        }
    }

    /* 5. 最良重みを書き戻す */
    dtr_weights_set(ga_best_weights);

    /* 6. 推論ロック解除 */
    dtr_ga_busy = 0;

    /* fitness を記録 */
    ga_stats.best_fitness_pct = (UB)(best_fit * 100.0f);

    ga_puts("[ga]  step="); ga_putdec(ga_stats.ga_steps);
    ga_puts(" fitness=");   ga_putf2(best_fit);
    ga_puts(" impr=");      ga_putdec(ga_stats.improvements);
    ga_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* ga_init                                                            */
/* ------------------------------------------------------------------ */

void ga_init(void)
{
    ga_stats.ga_steps         = 0;
    ga_stats.improvements     = 0;
    ga_stats.skipped          = 0;
    ga_stats.best_fitness_pct = 0;

    ga_lcg_seed = 0xCA4E8A11UL;

    ga_puts("[ga]  initialized  pop=");
    ga_putdec(GA_POP_SIZE);
    ga_puts("  mutate_scale=1/");
    ga_putdec(GA_MUTATE_SCALE);
    ga_puts("  log_min=");
    ga_putdec(GA_LOG_MIN);
    ga_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* ga_stat                                                            */
/* ------------------------------------------------------------------ */

void ga_stat(void)
{
    ga_puts("[ga]  steps         : "); ga_putdec(ga_stats.ga_steps);         ga_puts("\r\n");
    ga_puts("[ga]  improvements  : "); ga_putdec(ga_stats.improvements);     ga_puts("\r\n");
    ga_puts("[ga]  skipped       : "); ga_putdec(ga_stats.skipped);          ga_puts("\r\n");
    ga_puts("[ga]  best fitness  : "); ga_putdec(ga_stats.best_fitness_pct); ga_puts("%\r\n");
    ga_puts("[ga]  log entries   : "); ga_putdec(dtr_log_avail());           ga_puts("/");
    ga_putdec(DTR_LOG_SIZE); ga_puts("\r\n");
    ga_puts("[ga]  pop size      : "); ga_putdec(GA_POP_SIZE);               ga_puts("\r\n");
    ga_puts("[ga]  interval      : every ");
    ga_putdec(GA_INTERVAL);
    ga_puts(" idle runs\r\n");
}
