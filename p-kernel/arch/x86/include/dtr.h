/*
 *  dtr.h (x86)
 *  Phase 8 — Distributed Transformer Inference (Pipeline Parallelism)
 *
 *  1 ノードに載らない大規模モデルを、クラスタ全体で動かすためのフレームワーク。
 *  K-DDS トピックを使ってノード間で中間活性化テンソルを転送する。
 *
 *  モデル構造 (3 ステージ MLP):
 *    Stage 0: Embed(4→8) + Layer0(8→8, Linear+ReLU)
 *    Stage 1: FFN(8→16→8, expand+contract)
 *    Stage 2: OutputHead(8→3) + Softmax → class [0,1,2]
 *
 *  パイプライン割り当て (2 ノード):
 *    Node 0 (even): Stage 0  — "dtr/l0" へ pub → "dtr/result" を sub
 *    Node 1 (odd) : Stage 1+2 — "dtr/l0" を sub → "dtr/result" へ pub
 *
 *  単一ノードモード (drpc_my_node == 0xFF):
 *    全ステージをローカルで実行 (K-DDS 不使用)
 *
 *  K-DDS トピック:
 *    "dtr/l0"     : Stage 0 出力 (DTR_ACT, 44 bytes)
 *    "dtr/result" : 最終推論結果 (DTR_RESULT, 24 bytes)
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define DTR_EMBED_DIM   8    /* 埋め込みベクトル次元数                */
#define DTR_FFN_DIM    16    /* FFN 中間次元数                        */
#define DTR_OUT_DIM     3    /* 出力クラス数 (normal/alert/critical)  */

#define DTR_ACT_MAGIC    0x52545444UL   /* "DTTR" LE */
#define DTR_RESULT_MAGIC 0x53455244UL   /* "DRES" LE */

#define DTR_TOPIC_L0      "dtr/l0"      /* Stage 0 出力: node 0→1 */
#define DTR_TOPIC_RESULT  "dtr/result"  /* 最終結果:   node 1→0 */

#define DTR_INFER_TMO   500  /* 分散推論タイムアウト (ms) */

/* ------------------------------------------------------------------ */
/* パケット構造 (K-DDS topic data として転送)                         */
/* ------------------------------------------------------------------ */

/* Stage 0 出力 — Layer0 後の活性化テンソル (44 bytes) */
typedef struct {
    UW    magic;                       /* DTR_ACT_MAGIC               */
    UW    req_id;                      /* 推論リクエスト ID           */
    UB    src_node;                    /* 送信ノード ID               */
    UB    layer;                       /* ステージ番号 (0=l0out)      */
    UH    _pad;
    float act[DTR_EMBED_DIM];          /* 活性化ベクトル float[8]=32B */
} __attribute__((packed)) DTR_ACT;    /* 4+4+1+1+2+32 = 44 bytes     */

/* 最終結果 (24 bytes) */
typedef struct {
    UW    magic;                       /* DTR_RESULT_MAGIC            */
    UW    req_id;                      /* 対応する推論リクエスト ID  */
    UB    class_id;                    /* 0=normal 1=alert 2=critical */
    UB    src_node;                    /* 送信ノード ID               */
    UH    _pad;
    float scores[DTR_OUT_DIM];         /* softmax 確率 float[3]=12B  */
} __attribute__((packed)) DTR_RESULT; /* 4+4+1+1+2+12 = 24 bytes     */

/* ------------------------------------------------------------------ */
/* 統計                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    UW  inferences;    /* 推論実行回数 (合計)           */
    UW  local;         /* ローカル実行回数              */
    UW  distributed;   /* 分散実行完了回数              */
    UW  timeouts;      /* タイムアウト回数              */
    UW  layer0_runs;   /* Stage 0 実行回数              */
    UW  layer1_runs;   /* Stage 1 (FFN) 実行回数        */
    UW  output_runs;   /* Stage 2 (OutputHead) 実行回数 */
} DTR_STATS;

extern DTR_STATS dtr_stats;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* 初期化 (重みを LCG で生成, セマフォ作成) */
void dtr_init(void);

/* パイプラインタスク
 *   Node 0: "dtr/result" を subscribe し、dtr_infer() のセマフォを signal
 *   Node 1: "dtr/l0" を subscribe し、Stage1+2 を計算して "dtr/result" を pub */
void dtr_task(INT stacd, void *exinf);

/* 推論実行 (シェルタスクから呼ぶ, ブロッキング)
 *   input[4]: int8 センサ値 (temp, humidity, pressure, light)
 *   戻り値: class [0,1,2] または -1 (エラー/タイムアウト) */
W    dtr_infer(const B input[4]);

/* 統計表示 */
void dtr_stat(void);
