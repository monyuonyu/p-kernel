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
 *    "dtr/dkva/q"          : Query ブロードキャスト (任意の起点 → 全ノード)
 *    "dtr/dkva/resp/<node>": 部分 Attention レスポンス (ノードごとに別トピック)
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
/* レスポンスは「送信元ノードごと」に別トピックへ pub する。
 * 単一トピック ("dtr/dkva/resp") を LATEST_ONLY で共有すると、複数の
 * responder が同じ 1 スロットを上書きし合い、requester が 1 ラウンドで
 * 1 peer 分しか集約できなかった。per-source topic
 * "dtr/dkva/resp/<node>" にすることで responder ごとに独立した
 * ラッチスロットを持たせ、全 peer の partial が確実に届くようにする。 */
#define DKVA_TOPIC_RESP_PFX "dtr/dkva/resp/"   /* + 1 桁ノード ID         */
/* region 要約トピック (GLOBAL スコープ)。coordinator が自 region の partial を
 * 集約した {分子 partial_out, 分母 attn_sum} を "dtr/dkva/rsum/<rid>" へ発行し、
 * requester が region 間で疎に畳む。階層集約で全体 attention を厳密復元する
 * (regions R2, Y — docs/architecture/regions.md)。rid = coordinator のノード ID。*/
#define DKVA_TOPIC_RSUM_PFX "dtr/dkva/rsum/"   /* + 1 桁 coordinator ID    */
#define DKVA_INFER_TMO    600   /* 分散 Attention タイムアウト (ms) */
#define DKVA_RSUM_WIN_MS  200   /* coordinator が region partial を集める窓 */

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

/* Query パケット: 起点ノード (どれでもよい) が全ノードにブロードキャスト */
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
 * dtr.c の FULL モードおよび dkva_cmd から、どのノードでも呼べる。
 *
 * 部分集約 (survival, wave 8):
 *   fan-out 時に SWIM が生存と見ていた peer が欠けても、resp_cnt >= 1
 *   なら部分結果で完遂し "degraded (k/n)" を明示出力する (黙って成功に
 *   しない)。SWIM が DEAD と判定済みのノードは最初から待たない。
 *   リモート寄与がゼロのときだけ E_TMOUT を返し、呼び出し側は
 *   ローカル MHSA にフォールバックする。
 */
ER dkva_infer(const float Q[DKVA_SEQ][DKVA_NH][DKVA_DH],
              const float W_o[DKVA_DM][DKVA_DM],
              float mhsa_out[DKVA_SEQ][DKVA_DM],
              UW req_id);

void dkva_stat(void);

/*
 * シェルコマンド "dkva [infer [a b c d]]"。
 * 引数から決定論的に Q を合成し、このノードを起点に dkva_infer を回す。
 * ノード ID に依存しない: 起点が死んでも、生き残りのどのノードからでも
 * 同じ問いを発行して完遂できる (survival, wave 8)。
 */
void dkva_cmd(const UB *args, UW len);
