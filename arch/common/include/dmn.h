/*
 *  dmn.h (x86)
 *  Phase 13 — Default Mode Network
 *
 *  人間の脳のデフォルトモードネットワーク (DMN) をモデルにした
 *  カーネル内蔵の自律活性化・アイドル整理機構。
 *
 *  状態遷移:
 *    ACTIVE : 外部刺激受信中。推論・ネットワーク処理を優先。
 *    IDLE   : 一定時間刺激なし。バックグラウンドで記憶整理を実行。
 *
 *  ハートビート:
 *    T-Kernel cyclic handler が DMN_PULSE_MS ごとに発火。
 *    dmn_task がパルスを受け取り状態を評価する。
 *
 *  外部刺激の通知元:
 *    dtr.c  : dtr_infer() 呼び出し時
 *    swim.c : ノード ALIVE/DEAD 遷移時
 *
 *  アイドル整理タスク:
 *    1. dtr 推論統計のダイジェスト出力
 *    2. replica の同期状態チェック
 *    3. 縮退レベルの確認・ログ
 *    4. (将来) fedlearn 勾配集約・KVキャッシュ LRU 再構築
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define DMN_ACTIVE          0   /* 外部刺激に反応中                   */
#define DMN_IDLE            1   /* アイドル — 記憶整理中              */

#define DMN_PULSE_MS                1000   /* ハートビート周期 (ms)              */
#define DMN_IDLE_THRESHOLD_DEFAULT  5      /* デフォルト: N パルス刺激なし → IDLE */
#define DMN_LOG_INTERVAL_DEFAULT    30     /* デフォルト: ログ出力間隔 (パルス数) */

/* ------------------------------------------------------------------ */
/* 実行時可変パラメータ (GA/RL から動的調整可能)                      */
/* ------------------------------------------------------------------ */

/* IDLE 遷移閾値 — dmn_trigger() のないパルス数でアイドルへ           */
extern volatile UW  dmn_idle_threshold;
/* アイドルログ出力間隔 (idle_runs の周期)                            */
extern volatile UW  dmn_log_interval;

/* ------------------------------------------------------------------ */
/* 統計                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    UW  pulses;          /* ハートビート総数                          */
    UW  triggers;        /* 外部刺激受信回数                          */
    UW  idle_runs;       /* アイドル整理実行回数                      */
    UW  active_to_idle;  /* ACTIVE → IDLE 遷移回数                   */
    UW  idle_to_active;  /* IDLE → ACTIVE 遷移回数                   */
} DMN_STATS;

extern DMN_STATS dmn_stats;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* 初期化 (cyclic handler 生成、セマフォ生成) */
void dmn_init(void);

/* DMN タスク本体 (usermain から起動) */
void dmn_task(INT stacd, void *exinf);

/* 外部刺激を通知する — dtr.c / swim.c から呼ぶ */
void dmn_trigger(void);

/* 現在の状態を返す (DMN_ACTIVE / DMN_IDLE) */
UB   dmn_state_get(void);

/* LM-6 (living-mind.md Part VII): lifetime count of R3 idle rounds run
 * by THE dmn.c idle-hook call site (the only ++ site) — the
 * attribution counter behind `mind wait`'s [teach-live] (VII.5). */
UW   dmn_r3_rounds(void);

/* 統計表示 (shell `dmn` コマンド用) */
void dmn_stat(void);

/* 実行時パラメータ変更 (GA/RL / shell から呼ぶ) */
void dmn_set_idle_threshold(UW v);
void dmn_set_log_interval(UW v);

/* Step ④ proof (wave-dmn-student-distill): drive the REAL sleep path
 * (dmn_idle_work) N times and show the resident NS-1 baby's held-out loss drop
 * across the sleeps, plus confirm the R3 living-mind track still runs. Shell:
 * `dmn distill [N]`. A no-op-honest cert on a baby-less / PFS-less node. */
void dmn_student_distill_test(UW n);

/* ── interoception mind-body coupling (interoception.md §3.2/§3.5) ──────────
 * The S_n stress bus modulates the DMN's effective idle threshold. These expose
 * the SAME production values the live dmn_task loop steers on, for the
 * [intero-tick] acceptance cert (production symbols, not a sim). */
UW   dmn_intero_effective_threshold(void);  /* re-reads S_n, applies deadband  */
UB   dmn_intero_held_sn(void);              /* the deadband-held S_n           */

/* `intero` shell verb cert (interoception.md §3.5 [intero-tick]): sweeps S_n
 * low->high and proves the effective idle threshold falls MONOTONICALLY while
 * the deadband suppresses oscillation under a flat S_n. Prints
 * "[intero-tick] PASS/FAIL". Returns 0 = PASS. Calls intero_self_test first. */
INT  dmn_intero_modulation_test(void);
