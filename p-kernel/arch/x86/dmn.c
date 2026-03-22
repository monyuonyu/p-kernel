/*
 *  dmn.c (x86)
 *  Phase 13 — Default Mode Network
 *
 *  外部刺激で発火し、アイドル時に記憶を整理する。
 *  人間の脳のデフォルトモードネットワークをカーネルに組み込んだもの。
 *
 *  ハートビート (cyclic handler, 1000ms):
 *    → dmn_sem を signal → dmn_task が起床
 *    → 直近の刺激を確認して ACTIVE/IDLE を判定
 *
 *  ACTIVE: 外部刺激 (推論リクエスト / ノード状態変化) を受信中
 *  IDLE  : 刺激なしが DMN_IDLE_THRESHOLD パルス継続 → 整理処理を実行
 *
 *  整理処理 (dmn_idle_work):
 *    1. 推論統計のダイジェスト出力 (dtr_stat)
 *    2. 縮退レベルの確認 (degrade_level)
 *    3. (将来) fedlearn 勾配集約 / KV キャッシュ再構築
 */

#include "dmn.h"
#include "dtr.h"
#include "degrade.h"
#include "ga.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* ユーティリティ                                                      */
/* ------------------------------------------------------------------ */

static void dmn_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void dmn_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { dmn_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    dmn_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

DMN_STATS dmn_stats;

static volatile UB  dmn_state        = DMN_ACTIVE;
static volatile UW  dmn_pulse_count  = 0;   /* ハートビート総数        */
static volatile UW  dmn_last_trigger = 0;   /* 最後に刺激を受けたパルス */
static ID           dmn_sem;               /* cyclic handler → task   */
static ID           dmn_cyc;               /* cyclic handler ID        */

/* ------------------------------------------------------------------ */
/* Cyclic handler — タスク独立コンテキスト (割り込みレベル)           */
/* ------------------------------------------------------------------ */

static void dmn_pulse_handler(VP exinf)
{
    (void)exinf;
    dmn_pulse_count++;
    dmn_stats.pulses++;
    tk_sig_sem(dmn_sem, 1);
}

/* ------------------------------------------------------------------ */
/* 外部刺激通知 — dtr.c / swim.c から呼ぶ                            */
/* ------------------------------------------------------------------ */

void dmn_trigger(void)
{
    dmn_last_trigger = dmn_pulse_count;
    dmn_stats.triggers++;

    if (dmn_state == DMN_IDLE) {
        dmn_state = DMN_ACTIVE;
        dmn_stats.idle_to_active++;
    }
}

/* ------------------------------------------------------------------ */
/* アイドル整理処理                                                    */
/* ------------------------------------------------------------------ */

static void dmn_idle_work(void)
{
    dmn_stats.idle_runs++;

    /* Phase 14: GA による重み自己改善 (GA_INTERVAL アイドルに 1 回) */
    if (dmn_stats.idle_runs % GA_INTERVAL == 1)
        ga_step();

    /* DMN_LOG_INTERVAL パルスに 1 回だけ詳細ログを出す */
    if (dmn_stats.idle_runs % DMN_LOG_INTERVAL != 1) return;

    static const char *lname[] = { "FULL", "REDUCED", "SOLO" };
    UB lv = degrade_level();

    dmn_puts("[dmn] idle digest ---\r\n");

    dmn_puts("[dmn]   pulses      : "); dmn_putdec(dmn_stats.pulses);    dmn_puts("\r\n");
    dmn_puts("[dmn]   triggers    : "); dmn_putdec(dmn_stats.triggers);  dmn_puts("\r\n");
    dmn_puts("[dmn]   idle runs   : "); dmn_putdec(dmn_stats.idle_runs); dmn_puts("\r\n");
    dmn_puts("[dmn]   degrade     : ");
    dmn_puts(lv < 3 ? lname[lv] : "?");
    dmn_puts("\r\n");

    /* 推論統計を出力 */
    dtr_stat();
}

/* ------------------------------------------------------------------ */
/* DMN タスク本体                                                      */
/* ------------------------------------------------------------------ */

void dmn_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    dmn_puts("[dmn] task started  pulse=");
    dmn_putdec(DMN_PULSE_MS);
    dmn_puts("ms  idle_threshold=");
    dmn_putdec(DMN_IDLE_THRESHOLD);
    dmn_puts("s\r\n");

    for (;;) {
        /* ハートビートを待つ */
        tk_wai_sem(dmn_sem, 1, TMO_FEVR);

        UW idle_for = dmn_pulse_count - dmn_last_trigger;

        if (idle_for >= DMN_IDLE_THRESHOLD) {
            /* ACTIVE → IDLE 遷移 */
            if (dmn_state == DMN_ACTIVE) {
                dmn_state = DMN_IDLE;
                dmn_stats.active_to_idle++;
                dmn_puts("[dmn] -> IDLE  (no stimulus for ");
                dmn_putdec(idle_for);
                dmn_puts("s)\r\n");
            }
            /* アイドル整理 */
            dmn_idle_work();
        } else {
            /* IDLE → ACTIVE 遷移はすでに dmn_trigger() で処理済み */
        }
    }
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void dmn_init(void)
{
    dmn_stats.pulses        = 0;
    dmn_stats.triggers      = 0;
    dmn_stats.idle_runs     = 0;
    dmn_stats.active_to_idle = 0;
    dmn_stats.idle_to_active = 0;

    dmn_state        = DMN_ACTIVE;
    dmn_pulse_count  = 0;
    dmn_last_trigger = 0;

    /* セマフォ生成 */
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 64 };
    dmn_sem = tk_cre_sem(&cs);

    /* Cyclic handler 生成 */
    T_CCYC cc;
    cc.exinf   = NULL;
    cc.cycatr  = TA_HLNG | TA_STA;           /* 即時開始               */
    cc.cychdr  = (FP)dmn_pulse_handler;
    cc.cyctim  = (RELTIM)DMN_PULSE_MS;
    cc.cycphs  = 0;
    dmn_cyc    = tk_cre_cyc(&cc);

    dmn_puts("[dmn] initialized  heartbeat=");
    dmn_putdec(DMN_PULSE_MS);
    dmn_puts("ms  idle_threshold=");
    dmn_putdec(DMN_IDLE_THRESHOLD);
    dmn_puts("s\r\n");
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

UB dmn_state_get(void) { return dmn_state; }

void dmn_stat(void)
{
    static const char *sname[] = { "ACTIVE", "IDLE" };
    UB st = dmn_state < 2 ? dmn_state : 0;

    dmn_puts("[dmn] state         : "); dmn_puts(sname[st]); dmn_puts("\r\n");
    dmn_puts("[dmn] pulses        : "); dmn_putdec(dmn_stats.pulses);         dmn_puts("\r\n");
    dmn_puts("[dmn] triggers      : "); dmn_putdec(dmn_stats.triggers);       dmn_puts("\r\n");
    dmn_puts("[dmn] idle runs     : "); dmn_putdec(dmn_stats.idle_runs);      dmn_puts("\r\n");
    dmn_puts("[dmn] active->idle  : "); dmn_putdec(dmn_stats.active_to_idle); dmn_puts("\r\n");
    dmn_puts("[dmn] idle->active  : "); dmn_putdec(dmn_stats.idle_to_active); dmn_puts("\r\n");
    dmn_puts("[dmn] idle threshold: "); dmn_putdec(DMN_IDLE_THRESHOLD);       dmn_puts("s\r\n");
    dmn_puts("[dmn] heartbeat     : "); dmn_putdec(DMN_PULSE_MS);             dmn_puts("ms\r\n");
}
