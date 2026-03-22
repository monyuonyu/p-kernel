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

#define DMN_PULSE_MS        1000   /* ハートビート周期 (ms)           */
#define DMN_IDLE_THRESHOLD  5      /* N パルス刺激なし → IDLE 遷移    */
#define DMN_LOG_INTERVAL    30     /* アイドルログ出力間隔 (パルス数)  */

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

/* 統計表示 (shell `dmn` コマンド用) */
void dmn_stat(void);
