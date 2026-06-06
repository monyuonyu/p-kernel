/*
 *  retrieval.h — Wave 8 ①: 記憶→思考の配線 (engram retrieval).
 *
 *  §9 への直接の答え:「記憶がなければ考えられない、のに dtr の forward
 *  は p-fs を一行も読まない」。dtr save/load はライフサイクル (重みの
 *  保存/復元) であって、思考中の記憶参照ではなかった。ここで初めて
 *  forward パスが p-fs の記憶を読む:
 *
 *    dtr remember
 *      -> 訓練セットから RET_ENGRAM_N 個のクラス均等な代表サンプル
 *         (埋め込みベクトル + ラベル) を「engram ブロック」として p-fs
 *         named ref "dtr/engrams" に保存。content-addressed なので P1
 *         レプリケーション + P2 ref gossip で群れ全体に伝わる。
 *
 *    forward (retrieval ON)
 *      -> 入力の埋め込みと engram の L2 距離で top-k (k=RET_TOPK) を
 *         引き、分類 logits に票を加算してから softmax:
 *             logits[label_i] += RET_ALPHA * gate * sim_i,
 *             sim_i = 1 / (1 + d2_i),  gate = (1 - p_max)^2
 *         (p_max は票を入れる前の softmax 最大値 — 重みが確信して
 *          いるほど記憶は控えめに、迷っているほど記憶が決める。
 *          測定に基づく選択: retrieval.c の blend ノート参照)
 *         engram は p-fs から読む (初回利用時にロード、キャッシュ可)。
 *         p-fs に "dtr/engrams" が無ければ retrieval は一切働かない —
 *         記憶が本当に源であることの保証。
 *
 *  埋め込みは重み非依存 (input/127 の正規化生入力)。意図的な設計:
 *  重みが乱数のままのノードでも、p-fs から受け取った engram だけで
 *  chance を大幅に超えて分類できる (= 記憶だけで考える)。重み依存の
 *  埋め込みにすると未学習ノードでは類似度空間が崩れ、§9 の核心である
 *  「群れの記憶で考える」が成立しない。
 *
 *  arch/common discipline: <string.h> 禁止、固定幅 wire 型 +
 *  _Static_assert、スクラッチは static、float math は dtr.c の
 *  libc-free ヘルパと同系 (ここでは加減乗除のみ)。
 */

#pragma once
#include "kernel.h"
#include "dtr.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define RET_ENGRAM_N   32              /* 保存する代表サンプル数        */
#define RET_TOPK        3              /* kNN の k                      */
#define RET_DIM         DTR_SEQ_LEN    /* 埋め込み次元 (=4, 重み非依存) */
#define RET_ALPHA       2.0f           /* logit ブレンド係数            */

#define RET_REF         "dtr/engrams"  /* p-fs named ref                */
#define RET_REF_LEN     11

#define RET_BLOB_MAGIC  0x4D455244UL   /* "DREM" LE                     */
#define RET_BLOB_VER    1

/* ------------------------------------------------------------------ */
/* engram ブロックレイアウト (p-fs 1 ブロックに収まる: 656 B << 4096)  */
/* ------------------------------------------------------------------ */

typedef struct {
    float e[RET_DIM];              /* 埋め込み = input/127, float[4]    */
    UB    label;                   /* 0=normal 1=alert 2=critical       */
    UB    _pad[3];
} __attribute__((packed)) RET_ENGRAM;          /* 16+1+3 = 20 B        */

typedef struct {
    UW magic;                      /* RET_BLOB_MAGIC                    */
    UW version;                    /* RET_BLOB_VER                      */
    UW count;                      /* 有効 engram 数 (<= RET_ENGRAM_N)  */
    UB dim;                        /* RET_DIM                           */
    UB out_dim;                    /* DTR_OUT_DIM                       */
    UB _pad[2];
} __attribute__((packed)) RET_BLOB_HDR;        /* 4+4+4+1+1+2 = 16 B   */

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* 入力 -> 重み非依存埋め込み (e[t] = input[t]/127) */
void ret_embed(const B input[DTR_SEQ_LEN], float e[RET_DIM]);

/* 訓練データ (X[i], y[i]) i<n からクラス均等・等間隔に RET_ENGRAM_N 個
 * 選び、engram ブロックとして p-fs "dtr/engrams" に保存する。
 * 戻り値: PFS_OK または負の PFS_E_*。シェルタスクから呼ぶこと。 */
INT  ret_remember(const B (*X)[DTR_SEQ_LEN], const UB *y, UW n);

/* ブレンド ON/OFF (デフォルト OFF)。前の値を返す。 */
UB   ret_set(UB on);
UB   ret_get(void);

/* engram キャッシュを破棄 — 次の利用時に p-fs から読み直す
 * (gossip で新しい版が届いた後に使う) */
void ret_drop(void);

/* engram を p-fs からロード試行 (キャッシュ済みなら即返る)。
 * 戻り値: 利用可能な engram 数 (0 = p-fs に無い → retrieval は不能)。
 * シェルタスクコンテキスト限定 (pfs_dag_read のスクラッチ規約)。 */
UW   ret_avail(void);

/* retrieval 票を logits[DTR_OUT_DIM] に加算する (softmax の前に呼ぶ)。
 * OFF か engram 未取得なら何もしない。戻り値: 投票した engram 数。 */
INT  ret_blend(const B input[DTR_SEQ_LEN], float logits[DTR_OUT_DIM]);

/* 状態表示 (`dtr ret`) */
void ret_stat(void);
