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

#define DTR_EMBED_DIM   8    /* 埋め込みベクトル次元数 (d_model)      */
#define DTR_FFN_DIM    16    /* FFN 中間次元数                        */
#define DTR_OUT_DIM     3    /* 出力クラス数 (normal/alert/critical)  */

/* Transformer ハイパーパラメータ */
#define DTR_SEQ_LEN     4    /* トークン数 (センサー4ch = 4 tokens)   */
#define DTR_NUM_HEADS   2    /* Multi-Head Attention ヘッド数         */
#define DTR_D_HEAD      4    /* ヘッド次元 (DTR_EMBED_DIM/NUM_HEADS)  */

#define DTR_ACT_MAGIC    0x52545444UL   /* "DTTR" LE — Pipeline Parallel  */
#define DTR_RESULT_MAGIC 0x53455244UL   /* "DRES" LE — 推論結果           */
#define DTR_INPUT_MAGIC  0x4E495444UL   /* "DTIN" LE — raw input 共有     */
#define DTR_HEAD_MAGIC   0x44485444UL   /* "DTHD" LE — head 出力          */

#define DTR_TOPIC_L0      "dtr/l0"      /* Pipeline: Attn出力(node0→1)    */
#define DTR_TOPIC_RESULT  "dtr/result"  /* Pipeline: 最終結果(node1→0)    */
#define DTR_TOPIC_INPUT   "dtr/input"   /* TensorPar: raw input(node0→1)  */
#define DTR_TOPIC_HEAD1   "dtr/head1"   /* TensorPar: head1出力(node1→0)  */

#define DTR_INFER_TMO   800  /* 分散推論タイムアウト (ms) */

/* ------------------------------------------------------------------ */
/* パケット構造 (K-DDS topic data として転送)                         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Pipeline Parallel パケット (FULL mode / 後方互換)                 */
/* ------------------------------------------------------------------ */

/* Stage 0 出力 — MHSA 後の mean-pool ベクトル (44 bytes) */
typedef struct {
    UW    magic;                       /* DTR_ACT_MAGIC               */
    UW    req_id;                      /* 推論リクエスト ID           */
    UB    src_node;                    /* 送信ノード ID               */
    UB    layer;                       /* ステージ番号                */
    UH    _pad;
    float act[DTR_EMBED_DIM];          /* float[8] = 32B              */
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
/* Tensor Parallel パケット (REDUCED mode)                           */
/* ------------------------------------------------------------------ */

/* raw input 共有パケット (16 bytes) — Node0 → Node1 */
typedef struct {
    UW  magic;                         /* DTR_INPUT_MAGIC             */
    UW  req_id;
    UB  src_node;
    UB  _pad[3];
    B   input[DTR_SEQ_LEN];            /* センサー入力 int8[4]        */
} __attribute__((packed)) DTR_INPUT;  /* 4+4+1+3+4 = 16 bytes        */

/* head 出力パケット (76 bytes) — Node1 → Node0 */
typedef struct {
    UW    magic;                       /* DTR_HEAD_MAGIC              */
    UW    req_id;
    UB    src_node;
    UB    head_id;
    UH    _pad;
    float out[DTR_SEQ_LEN * DTR_D_HEAD]; /* [4][4] float = 64 bytes  */
} __attribute__((packed)) DTR_HEAD_ACT; /* 4+4+1+1+2+64 = 76 bytes   */

/* ------------------------------------------------------------------ */
/* 統計                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    UW  inferences;    /* 推論実行回数 (合計)              */
    UW  local;         /* ローカル実行回数                 */
    UW  distributed;   /* 分散実行完了回数                 */
    UW  timeouts;      /* タイムアウト回数                 */
    UW  layer0_runs;   /* Stage 0 実行回数                 */
    UW  layer1_runs;   /* Stage 1 (FFN) 実行回数           */
    UW  output_runs;   /* Stage 2 (OutputHead) 実行回数    */
    UW  attn_runs;     /* MHSA 実行回数                    */
    UW  tp_distributed;/* Tensor Parallel 分散完了回数     */
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

/* ------------------------------------------------------------------ */
/* Phase 14 — GA サポート API                                         */
/* ------------------------------------------------------------------ */

/* 全重みパラメータ数 (bias・LN 含む)
 *   W_emb(32)+b_emb(8)+W_q(64)+W_k(64)+W_v(64)+W_o(64)
 *   +ln1_g(8)+ln1_b(8)+ln2_g(8)+ln2_b(8)
 *   +W_ffn1(128)+b_ffn1(16)+W_ffn2(128)+b_ffn2(8)
 *   +W_cls(24)+b_cls(3) = 635                                        */
#define DTR_WEIGHT_FLOATS  635

/* 推論ログのリングバッファサイズ */
#define DTR_LOG_SIZE  16

/* 推論ログエントリ (8 bytes) */
typedef struct {
    B   input[DTR_SEQ_LEN];   /* センサー入力 int8[4]               */
    UB  class_id;             /* 推論結果クラス (0=normal/1=alert/2=critical) */
    UB  confidence_pct;       /* max softmax × 100 (0〜100)         */
    UH  _pad;
} DTR_LOG_ENTRY;

/* GA 実行中フラグ — セットされている間 dtr_infer() は -1 を返す */
extern volatile UB dtr_ga_busy;

/* 全重みを flat バッファにコピー (buf は DTR_WEIGHT_FLOATS 要素以上) */
void  dtr_weights_get(float *buf);

/* flat バッファから全重みをロード */
void  dtr_weights_set(const float *buf);

/* 推論ログの有効エントリ数を返す (0〜DTR_LOG_SIZE) */
UW    dtr_log_avail(void);

/* 推論ログエントリを取得 (idx=0 が最新) */
void  dtr_log_get_entry(UW idx, DTR_LOG_ENTRY *out);

/* GA 評価専用: 現在ロードされている重みで log エントリを再推論し
 * 平均 max-softmax (0.0〜1.0) を返す。dtr_ga_busy セット中に呼ぶこと */
float dtr_eval_confidence(void);
