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
/* Query は「起点ノードごと」に別トピックへ pub する (G1, wave 10)。
 * 旧実装は単一共有トピック "dtr/dkva/q" を LATEST_ONLY で全網が上書きし
 * 合っていた → 網全体で in-flight な問いが常に 1 つだけという隠れた直列化点
 * (§5「同時多発」の真逆)。per-origin topic "dtr/dkva/q/<origin>" にすると
 * 起点ごとに独立したラッチスロットを持ち、複数ノードが同時に問いを発行しても
 * 互いを潰さない。responder は全 origin の q/<o> を購読する。 */
#define DKVA_TOPIC_Q_PFX  "dtr/dkva/q/"        /* + 1〜2 桁の起点ノード ID  */
/* レスポンスは「送信元(=応答した)ノードごと」に別トピックへ pub する。
 * 単一トピック ("dtr/dkva/resp") を LATEST_ONLY で共有すると、複数の
 * responder が同じ 1 スロットを上書きし合い、requester が 1 ラウンドで
 * 1 peer 分しか集約できなかった。per-source topic
 * "dtr/dkva/resp/<node>" にすることで responder ごとに独立した
 * ラッチスロットを持たせ、全 peer の partial が確実に届くようにする。
 *
 * 同時多発 (G1, wave 10): 1 つの responder の resp/<node> スロットも、
 * 複数の起点が同時に問うと起点間で上書きし合う。これを解くため
 *   (1) RESP_PKT に origin フィールドを持たせ、requester は自分宛
 *       (origin==自ノード) の応答だけ受理する → 取り違えゼロ。
 *       req_id が起点間で衝突しても origin で曖昧さが消える。
 *   (2) responder は pending な各 origin の応答をラウンドロビンで
 *       resp/<node> へ再発行する (時間多重) → どの起点の応答も
 *       自分のポーリング窓内で必ずスロットに現れる。
 * これで「網全体で同時に多数の問い」が本当に成立する。 */
#define DKVA_TOPIC_RESP_PFX "dtr/dkva/resp/"   /* + 1〜2 桁ノード ID       */
/* responder が pending な応答をラウンドロビン再発行し続ける反復回数
 * (1 反復 ≒ 10ms)。requester の窓 (DKVA_INFER_TMO) より十分長くして、
 * 同時起点が ≤ 数個でも全起点の応答がそれぞれの窓内でスロットに現れる
 * ことを保証する。 */
#define DKVA_ANSWER_ITERS 100
/* region 要約トピック (GLOBAL スコープ)。coordinator が自 region の partial を
 * 集約した {分子 partial_out, 分母 attn_sum} を "dtr/dkva/rsum/<rid>" へ発行し、
 * requester が region 間で疎に畳む。階層集約で全体 attention を厳密復元する
 * (regions R2, Y — docs/architecture/regions.md)。rid = coordinator のノード ID。*/
#define DKVA_TOPIC_RSUM_PFX "dtr/dkva/rsum/"   /* + 1 桁 coordinator ID    */
#define DKVA_INFER_TMO    600   /* 分散 Attention タイムアウト (ms) */
#define DKVA_RSUM_WIN_MS  200   /* (deprecated) 旧: coordinator が region partial を
                                 * 集める単一同期窓。G13 で撤去 — 下記参照。 */
/* G13 (§5「同時多発・並行分散」, COMPUTE 軸): 旧実装は coordinator が問いを
 * 受けるたび coordinator_aggregate() を responder ループ内から *同期呼び* し、
 * DKVA_RSUM_WIN_MS (=200ms) のあいだ tk_dly_tsk で region partial を集め続けた。
 * その 200ms 間 responder のラウンドロビン応答 (時間多重) が凍り、複数 origin が
 * region をまたいで同時に問うと「1 問ずつ 200ms」に再直列化されていた (G1 が
 * 単一 region で解いた同時性が multi-region で破れる)。
 *
 * 新実装: coordinator の region 集約を「origin ごとの event-driven 状態機械」に
 * する。各 origin の fan-in は responder ループの毎反復で *少しずつ* 進み、何も
 * ブロックしない。完了 (region メンバの partial が出揃う) か per-origin 締切で
 * rsum を確定し、resp と同じ round-robin 時間多重で rsum/<me> へ再発行する。
 * これで「同時に多数の問いが region をまたいで並行に集約される」(NO 中央窓・
 * NO グローバル順序)。締切はループ周期 (10ms) 単位の反復回数で持つ。 */
/* per-origin 集約の straggler 安全キャップ (×10ms ループ ≒ 200ms 上限)。
 * ★これは通常完了経路ではない: 集約は quorum_core() が真になった瞬間 (期待した
 * region メンバの partial が出揃った瞬間) に確定する (event/arrival 駆動)。この
 * キャップは「生存メンバが永遠に黙る」異常時にだけ発火する保険 (liveness)。
 * [g13-arrival] 自己テストが「通常経路は arrival 発火・窓には padding されない」を
 * 数で証明する (FAST: N 集約が ≪ N×CAP で完了; CAP: never-arrive のみが窓で確定)。*/
#define DKVA_RSUM_WIN_ITERS 20

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
    UB    src_node;       /* 応答した(=この partial を計算した)ノード */
    UB    n_entries;      /* 応答に含まれる KV エントリ数         */
    UB    origin;         /* この応答の宛先 = 問いの起点ノード (G1) */
    UB    _pad;
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
 * 純ローカル (network/kdds 非依存) の in-process プロパティ自己テスト (G13)。
 * 「per-origin の集約は順序非依存・origin 間で相互非汚染・単一共有窓なし」を
 * 数で守る。shell `dkva test` から呼び、CI が "[g13-parallel] PASS" を grep する。
 * 戻り値 = 失敗数 (0 で全 PASS)。
 */
INT dkva_self_test(void);

/*
 * G13 distinguishing self-test (純ローカル): coordinator の region 集約は固定窓を
 * 待ち切らず、期待寄与が ARRIVE した瞬間に確定する (event/quorum 駆動) ことを数で
 * 証明する。固定窓 (DKVA_RSUM_WIN_ITERS) は never-arrive な straggler 用の安全
 * キャップであり通常経路ではない、を A1/A2/A3 で区別する。shell `dkva test` から
 * dkva_self_test と並べて呼び、CI が "[g13-arrival] PASS" を grep する。
 * 戻り値 = 失敗数 (0 で全 PASS)。
 */
INT dkva_arrival_test(void);

/*
 * [fed-2cluster] + [coord-crash] federation R0 self-test (純ローカル, in-proc).
 * docs/architecture/federation-r0-plan.md §3.1 Arm A + §3.2。合成 2-region 収束
 * ビューで「2 つの distinct region が在り、cross-region 期待は O(#region) (=1
 * coordinator) であって O(N) でない」+「summary 駆動 fold == dense fold」+
 * 「NOCENTRAL min-id 代表」+「coordinator 死 → 決定論的 b2 へ再委譲」を数で守る。
 * 本番 dkva_infer と同一の純コア dkva_expect_core を通す。shell `dkva test` から
 * 呼び、CI が "[fed-2cluster] PASS (in-proc)" を grep する。戻り値 = 失敗数。
 */
INT dkva_fed2_self_test(void);

/*
 * シェルコマンド "dkva [infer [a b c d]]"。
 * 引数から決定論的に Q を合成し、このノードを起点に dkva_infer を回す。
 * ノード ID に依存しない: 起点が死んでも、生き残りのどのノードからでも
 * 同じ問いを発行して完遂できる (survival, wave 8)。
 */
void dkva_cmd(const UB *args, UW len);
