/*
 *  ga.h (x86)
 *  Phase 14 — Genetic Algorithm による Transformer 重みの自己改善
 *
 *  DMN がアイドル状態に入ったとき、バックグラウンドで実行される。
 *  「無意識の学習」— 推論 (意識) とは分離されたプロセス。
 *
 *  アルゴリズム:
 *    1. 現在の Transformer 重み (635 floats) を保存
 *    2. 直近の推論ログ (最大 DTR_LOG_SIZE 件) に対して
 *       現在重みの平均 confidence (baseline fitness) を計測
 *    3. GA_POP_SIZE-1 個の変異体を生成
 *       各重みに小さなランダムノイズ (LCG) を加算
 *    4. 各変異体の fitness を評価
 *    5. baseline より良い変異体があれば採用
 *    6. 重みを dtr に書き戻し、GA ロックを解除
 *
 *  Fitness 関数:
 *    直近推論ログを再推論したときの平均 max-softmax スコア。
 *    値が高いほどモデルが確信を持って分類できている。
 *
 *  注意:
 *    GA 実行中は dtr_ga_busy=1 がセットされ、
 *    dtr_infer() は -1 を返す (推論スキップ)。
 *    GA は DMN 最低優先度タスク内から呼ばれるため、
 *    通常の推論・通信処理を妨げない。
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define GA_POP_SIZE     4     /* 個体数: 現在重み + 変異体 (POP-1) 個  */
#define GA_MUTATE_SCALE 20    /* 変異強度: noise ≈ ±(1/SCALE) 程度     */
#define GA_LOG_MIN      4     /* このエントリ数未満なら GA をスキップ  */
#define GA_INTERVAL     10    /* dmn_idle_work の何回に 1 回 ga_step() */

/* ------------------------------------------------------------------ */
/* 統計                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    UW  ga_steps;           /* GA ステップ実行回数                     */
    UW  improvements;       /* 重み採用回数 (baseline より改善)        */
    UW  skipped;            /* ログ不足でスキップした回数              */
    UB  best_fitness_pct;   /* 直近の最良 fitness × 100               */
    UB  _pad[3];
} GA_STATS;

extern GA_STATS ga_stats;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* 初期化 (LCG seed 設定) */
void ga_init(void);

/* 1 世代の GA ステップを実行 (dmn_idle_work から呼ぶ) */
void ga_step(void);

/* 統計表示 (shell `ga` コマンド用) */
void ga_stat(void);
