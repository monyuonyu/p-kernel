/*
 *  dkva.h (x86)
 *  Phase 10 — Distributed KV Attention (分散 Key/Value Attention)
 *
 *  各ノードが直近の推論で得た K/V ペアをキャッシュとして保持し、
 *  他ノードからの Query に応答することで、クラスタ全体の「集合記憶」
 *  を Attention 計算に利用する。
 *
 *  原理:
 *    ローカル MHSA では Q・K^T を自分が見た入力だけで計算するが、
 *    DKVA では他ノードの KV キャッシュも取り込んで Attention を計算する。
 *    ノードが増えるほど KV コンテキストが広がり、推論精度が向上する。
 *
 *  プロトコル (K-DDS):
 *    "dtr/dkva/q"    : Query ブロードキャスト (node0 → 全ノード)
 *    "dtr/dkva/resp" : 部分 Attention レスポンス (各ノード → node0)
 *
 *  KV キャッシュ:
 *    各ノードが DKVA_CACHE_SIZE エントリを保持。
 *    新しい推論が来るたびに最新の K/V を更新 (ring buffer)。
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define DKVA_CACHE_SIZE   8     /* ノードあたりの KV キャッシュエントリ数 */
#define DKVA_TOPIC_Q      "dtr/dkva/q"
#define DKVA_TOPIC_RESP   "dtr/dkva/resp"
#define DKVA_INFER_TMO    600   /* 分散 Attention タイムアウト (ms) */

/* モデル次元 (dtr.h と合わせる) */
#define DKVA_SEQ   4   /* トークン数    */
#define DKVA_NH    2   /* Attention ヘッド数 */
#define DKVA_DH    4   /* ヘッド次元    */
#define DKVA_DM    8   /* 埋め込み次元  */

/* ------------------------------------------------------------------ */
/* パケット構造                                                        */
/* ------------------------------------------------------------------ */

#define DKVA_Q_MAGIC     0x51564B44UL   /* "DKVQ" LE */
#define DKVA_RESP_MAGIC  0x52564B44UL   /* "DKVR" LE */

/* Query パケット: node0 が全ノードにブロードキャスト */
typedef struct {
    UW    magic;          /* DKVA_Q_MAGIC                         */
    UW    req_id;         /* リクエスト ID                        */
    UB    src_node;       /* 送信元ノード ID                      */
    UB    n_cached;       /* 要求するキャッシュエントリ数 (最大)  */
    UH    _pad;
    float Q[DKVA_SEQ][DKVA_NH][DKVA_DH];   /* Query テンソル    */
} __attribute__((packed)) DKVA_Q_PKT;

/* Response パケット: 各ノードが部分 Attention を返す */
typedef struct {
    UW    magic;          /* DKVA_RESP_MAGIC                      */
    UW    req_id;
    UB    src_node;
    UB    n_entries;      /* 応答に含まれる KV エントリ数         */
    UH    _pad;
    /* 部分 Attention 出力: Σ(softmax(Q·K_i^T) * V_i) の分子と分母 */
    float partial_out[DKVA_SEQ][DKVA_NH][DKVA_DH];  /* 加重和   */
    float attn_sum   [DKVA_SEQ][DKVA_NH];            /* 正規化用 */
} __attribute__((packed)) DKVA_RESP_PKT;

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

void dkva_init(void);
void dkva_task(INT stacd, void *exinf);

/*
 * KV キャッシュに新しいエントリを追加する。
 * dtr.c が推論するたびに呼ぶ。
 * K[NH][DH], V[NH][DH] は Embed → head 投影後の値。
 */
void dkva_cache_update(const float K[DKVA_NH][DKVA_SEQ][DKVA_DH],
                       const float V[DKVA_NH][DKVA_SEQ][DKVA_DH]);

/*
 * 分散 KV Attention を実行し、mhsa_out[SEQ][DM] を返す。
 * dtr.c の FULL モードから呼ぶ。
 * 失敗 (タイムアウト等) の場合は E_TMOUT を返し、
 * ローカル MHSA にフォールバックする。
 */
ER dkva_infer(const float Q[DKVA_SEQ][DKVA_NH][DKVA_DH],
              const float W_o[DKVA_DM][DKVA_DM],
              float mhsa_out[DKVA_SEQ][DKVA_DM],
              UW req_id);

void dkva_stat(void);
