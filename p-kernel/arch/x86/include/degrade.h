/*
 *  degrade.h (x86)
 *  縮退モード管理 — クラスタのノード数に応じて動作モードを自動切換え
 *
 *  レベル定義:
 *    DEGRADE_FULL    (≥3 ノード): 完全分散モード — DTR 分散推論・3 秒レプリカ
 *    DEGRADE_REDUCED (2  ノード): 縮退モード     — DTR パイプライン・2 秒レプリカ
 *    DEGRADE_SOLO    (1  ノード): 単独モード     — DTR ローカルのみ・1 秒レプリカ
 *                                                  SOLO 遷移時に全記憶を即時散布
 *
 *  統合方法:
 *    swim.c  → ALIVE/DEAD 遷移後に degrade_update() を呼ぶ
 *    replica.c → degrade_replica_interval() でスリープ時間を取得
 *    dtr.c   → SOLO なら分散パスをスキップ
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define DEGRADE_FULL     0   /* 3 ノード以上: 完全分散           */
#define DEGRADE_REDUCED  1   /* 2 ノード    : 縮退               */
#define DEGRADE_SOLO     2   /* 1 ノード    : 単独 (孤立)        */

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* usermain() の分散モードブロック内で drpc_init() の後に呼ぶ。 */
void degrade_init(void);

/*
 * ノードの ALIVE/DEAD/SUSPECT 状態が変わるたびに swim.c から呼ぶ。
 * 生存ノード数を数え直してレベルを更新する。
 * SOLO への遷移時は replica_scatter_all() を自動呼び出し。
 */
void degrade_update(void);

/* 現在のレベル (DEGRADE_FULL / REDUCED / SOLO) を返す。 */
UB   degrade_level(void);

/* replica_task のスリープ時間 (ms) を返す。 */
TMO  degrade_replica_interval(void);

/* shell `degrade` コマンド用: 現在のレベルと統計を表示。 */
void degrade_stat(void);
