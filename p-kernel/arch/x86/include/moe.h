/*
 *  moe.h (x86)
 *  Phase 10 — Mixture of Experts (MoE) 推論ルーティング
 *
 *  各ノードが「得意なクラス」を持ち、入力センサーデータを
 *  最も得意なノードへ自動ルーティングする。
 *
 *  Expert スコア:
 *    各ノードは class [0,1,2] に対して accuracy[class] を持つ。
 *    ルーター (Gate) は入力特徴量から「どのクラスっぽいか」を予測し、
 *    そのクラスのスコアが最高のノードへ推論を委譲する。
 *
 *  K-DDS トピック "moe/score" でスコアを定期ブロードキャスト。
 *  受信ノードはピアのスコアテーブルを更新する。
 *
 *  ルーティング:
 *    1. Gate: 入力の平均値からクラスを予測 (軽量線形分類)
 *    2. 最も得意なノードを選択
 *    3. DRPC で推論を委譲 (DRPC_CALL_INFER)
 *    4. フォールバック: タイムアウト時はローカル推論
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define MOE_NUM_CLASSES    3    /* クラス数 */
#define MOE_SCORE_TOPIC    "moe/score"
#define MOE_BROADCAST_MS   5000 /* スコアブロードキャスト間隔 (ms) */

/* ------------------------------------------------------------------ */
/* スコアパケット (K-DDS 経由でブロードキャスト)                     */
/* ------------------------------------------------------------------ */

typedef struct {
    UB  node_id;
    UB  accuracy[MOE_NUM_CLASSES];   /* 各クラスの正答率 (0-100%) */
    UW  total_infer;                 /* 総推論回数                */
    UW  correct[MOE_NUM_CLASSES];    /* クラス別正解数            */
} MOE_SCORE;

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

void moe_init(void);
void moe_task(INT stacd, void *exinf);

/* 推論実行: 最適ノードを選んで推論し、クラスを返す */
UB   moe_infer(B temp, B hum, B press, B light);

/* 推論結果をフィードバック (正解ラベルを学習) */
void moe_feedback(UB pred_class, UB true_class);

/* ピアのスコアを更新 (K-DDS 受信コールバックから呼ぶ) */
void moe_update_peer(const MOE_SCORE *score);

/* 統計表示 */
void moe_stat(void);
