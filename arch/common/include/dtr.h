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
#include "pfs_block.h"   /* PFS_ID_LEN — LM-7 MT_TEACH_PKT.prov_head width */

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

/* 全重みを与えた seed から再ロール (He 初期化)。dtr_init が使う固定 seed
 * とは別 seed で呼べる — R3b の specialization (spec.c) が各エキスパートを
 * 別 seed で初期化して自然に分化させるために使う (survival-network.md 道B)。
 * セマフォ等の副作用は無く、純粋に重みだけを書き換える。 */
void dtr_reinit_weights(UW seed);

/* 現在ロードされている重みでの 1 入力のクラス確率 (retrieval OFF、副作用
 * なし)。R3b spec.c が専門家の生出力をルーティング/集約するために使う。 */
void dtr_forward_probs(const B input[DTR_SEQ_LEN], float out[DTR_OUT_DIM]);

/* ONE BRAIN (wave 18): 現在ロードされている学習脳の argmax クラス。live な
 * 推論経路 (moe_infer ローカル / drpc DRPC_CALL_INFER リモート) はこの forward
 * で返答・ルーティング・守りを駆動する (手書き mlp_forward を live path から
 * 排除)。docs/review-2026-06-three-brains.md 参照。 */
UB   dtr_classify(const B input[DTR_SEQ_LEN]);

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

/* DKVA 用 KV キャッシュ warmup (Phase 10, follow-up #2)。
 * node 固有の合成入力でローカル MHSA を DTR_KV_SEED_N 回回し、kv_cache を
 * seed する。FULL クラスタで分散 Attention が非自明になるよう dkva_task の
 * 起動時に一度だけ呼ぶ。 */
#define DTR_KV_SEED_N  3
void dtr_seed_kv_cache(UB node);

/* ------------------------------------------------------------------ */
/* Phase 14 — GA サポート API                                         */
/* ------------------------------------------------------------------ */

/* 全重みパラメータ数 (bias・LN 含む)
 *   W_emb(32)+b_emb(8)+W_q(64)+W_k(64)+W_v(64)+W_o(64)
 *   +ln1_g(8)+ln1_b(8)+ln2_g(8)+ln2_b(8)
 *   +W_ffn1(128)+b_ffn1(16)+W_ffn2(128)+b_ffn2(8)
 *   +W_cls(24)+b_cls(3) = 635                                        */
#define DTR_WEIGHT_FLOATS  635

/* ------------------------------------------------------------------ */
/* ring3-core Wave C (III.3a) — the kernel-compute counter             */
/*                                                                     */
/* Incremented at the entry of EVERY kernel-resident compute path that */
/* can produce a class (train_forward / run_embed_seq in dtr.c,        */
/* mlp_forward in ai_job.c).  The `ring3 mind` gate snapshots it after */
/* its ring-0 oracle call and requires delta == 0 across every ring-3  */
/* run: a ring-3 "mind" that secretly answers via SYS_INFER /          */
/* SYS_DTR_SUBMIT / SYS_AI_* bumps this counter and FAILS.             */
/* The user ELF dual-compiles the same dtr.c and gets its own private  */
/* copy in its own .data — no interference.  One increment per         */
/* forward: a no-op cost on hosted builds (linux_x86_64 / aarch64).    */
/* ------------------------------------------------------------------ */
extern volatile UW kernel_infer_count;

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

/* ------------------------------------------------------------------ */
/* R3a — supervised training (real loss, real gradients)               */
/* ------------------------------------------------------------------ */

/* accurate libc-free exp/ln (range-reduced; see dtr.c HONESTY NOTE).
 * Shared with fedlearn.c / ai_job.c so the cross-entropy there is the
 * same real math, not a Taylor approximation gone feral. */
float dtr_expf(float x);
float dtr_logf(float x);

/* ---- shared Transformer kernels (R3: same math, sensor + in-context) ---
 * Exposed so r3_incontext.c composes the SAME numerically-meaningful
 * kernels the live sensor brain uses — the anti-fork rule in
 * docs/architecture/r3-nontrivial-thought.md. Width is a parameter:
 * the sensor path passes DTR_EMBED_DIM, the recall harness its own
 * d_model. (dt_linear/dt_softmax were already dim-parameterized.) */
#define DTR_LN_MAXW 64   /* LM-9 (living-mind Part X.3): a CAPACITY CAP for the
                          * one scratch array dxh[DTR_LN_MAXW] in dtr_ln_bwd —
                          * NOT a behavioral constant. The dtr sensor brain
                          * still calls dtr_ln_bwd with n=DM=8 and touches only
                          * dxh[0..7]; its arithmetic is byte-identical after
                          * the bump ([lang-sensor-intact] proves it). R3 calls
                          * with n=R_DM up to 64 — sized so a future R_DM=64
                          * needs no second bump. */
float dt_relu(float x);
float dt_sqrt(float x);
void  dt_linear(const float *W, const float *b,
                const float *x, float *y, INT M, INT N);
void  dt_softmax(float *x, INT n);
void  dtr_ln_fwd_cache(const float *x, const float *g, const float *b,
                       float *xh, float *istd_out, float *y, INT n);
void  dtr_ln_bwd(const float *dy, const float *xh, float istd,
                 const float *g, float *dgam, float *dbet, float *dx, INT n);

/* One full-batch SGD step (analytic backprop, cross-entropy).
 * Returns mean CE loss at the current weights. Call from the shell
 * task only; set dtr_ga_busy around training to block dtr_infer. */
float dtr_train_batch(const B (*X)[DTR_SEQ_LEN], const UB *y, UW n,
                      float lr);

/* Mean CE loss + accuracy (correct_out) without weight updates. */
float dtr_eval_batch(const B (*X)[DTR_SEQ_LEN], const UB *y, UW n,
                     UW *correct_out);

/* Analytic-vs-finite-difference gradient check on one sample.
 * Returns the max relative error (should be < ~0.05 in float32). */
float dtr_grad_check(const B input[DTR_SEQ_LEN], UB label);

/* ---- trained-weight persistence blob (p-fs object "dtr/weights") -- */
/* Fixed-width header + DTR_WEIGHT_FLOATS little-endian IEEE754
 * float32 params. PORTABILITY (honest statement): all 4 supported
 * targets (i686 / aarch64 / aarch64-linux / x86_64-linux) are
 * little-endian IEEE754 binary32, the same assumption every p-fs /
 * K-DDS wire struct already makes; sizeof(float)==4 is _Static_assert
 * pinned in dtr.c and dtr_train.c. A big-endian port would need a
 * byte-swapping loader. */

#define DTR_WBLOB_MAGIC  0x57525444UL   /* "DTRW" LE */
#define DTR_WBLOB_VER    1

typedef struct {
    UW magic;                       /* DTR_WBLOB_MAGIC                 */
    UW version;                     /* DTR_WBLOB_VER                   */
    UW n_params;                    /* DTR_WEIGHT_FLOATS               */
    UB d_model;                     /* DTR_EMBED_DIM                   */
    UB n_heads;                     /* DTR_NUM_HEADS                   */
    UB seq_len;                     /* DTR_SEQ_LEN                     */
    UB ffn_dim;                     /* DTR_FFN_DIM                     */
    UB out_dim;                     /* DTR_OUT_DIM                     */
    UB _pad[3];
} __attribute__((packed)) DTR_WBLOB_HDR;   /* 20 bytes */

/* r3_incontext.c — R3 non-trivial-thought capacity certificate.
 * `r3 test` runs the in-context recall acceptance suite. */
void r3_cmd(const UB *args, UW len);
void r3_test(void);
void r3_handoff_test(void);

/* LM-5 (living-mind.md Part VI) -- the 随時 stream: facts taught
 * in-context at different times are consolidated into R3's own rw[]
 * across multiple bounded sleep rounds. r3_fact_learn / r3_facts_pending
 * / r3_consolidate_idle_round are the LIVE API the DMN idle hook drives
 * (dmn.c dmn_idle_work); r3_stream_test is the CI certificate
 * (`handoff stream`). The fact queue itself stays file-static. */
INT  r3_fact_learn(const UB *keys, const UB *vals, INT n);
INT  r3_facts_pending(void);
INT  r3_consolidate_idle_round(void);
void r3_stream_test(void);

/* LM-8 (living-mind.md Part IX) -- the language slice cert: `mind lang`.
 * The capacity curve (real word-bindings vs masked recall), the sky->blue
 * round-trip in WORDS, the OOV refusal, and the named live-wire tags.
 * Re-baselines LM-4..7 against the widened R_VALV (stricter-only). */
void r3_lang_test(void);

/* LM-6 (living-mind.md Part VII) -- the mouth: the `mind` shell verb.
 * `mind teach <k> <v>` enqueues one owner-taught binding through
 * r3_fact_learn; the DMN's OWN idle pulses consolidate it (mind_cmd
 * never calls the round); `mind ask <k>` answers from the weights on a
 * MASKED prompt; `mind wait [s]` polls drain; bare `mind` = status.
 * The ONLY public LM-6 entry — all sub-verb logic, the substrate
 * bootstrap and the quiesce flag stay file-static (VII.9). */
void mind_cmd(const UB *args, UW len);

/* LM-7 (living-mind.md Part VIII) -- the shared mind: a fact taught on node
 * A becomes answerable from node B. After a successful LOCAL teach, m_teach
 * publishes ONE MT_TEACH_PKT on the region-scoped K-DDS topic "mind/teach"
 * (Path E: the tiny ENGRAM travels, never the weights). mind_net_task on
 * each peer polls that topic and feeds each unseen (origin,seq) fact into
 * its OWN r3_fact_learn (G33, the production mouth) — its OWN DMN then
 * consolidates it into its OWN rw[]. Region-scoped = the REGION's shared
 * mind (VIII.7); own-origin packets are dropped (the gossip-loop guard);
 * a remote teach of an already-bound key is REFUSED and printed (VIII.5). */
#define MIND_TEACH_TOPIC  "mind/teach"
#define MT_MAGIC          0x444E494DUL   /* "MIND" LE — free magic (VIII.9)  */
/* LM-8 (living-mind.md Part IX.8): the wire now carries TOKEN IDS (real
 * words, IX.3), not synthetic k<8/v<4. The key/val fields stay U1 (v1
 * keeps R_KEYV<=256, R_VALV<=256 so the width is UNCHANGED — only the
 * MEANING changes: id-into-vocab, not a small int). A `wire_ver` byte is
 * added: a receiver whose version does not match its own DROPS the packet
 * and PRINTS it — the ONE place the region partitions by version, made
 * OBSERVABLE (IX.7 mixed-version honesty). Old (LM-7) nodes set/expect
 * MT_WIRE_VER_LEGACY; LM-8 nodes set MT_WIRE_VER_LANG. */
#define MT_WIRE_VER_LEGACY  0    /* LM-7: synthetic k<8/v<4 (no version field)*/
#define MT_WIRE_VER_LANG    1    /* LM-8: token ids into the embedded vocab   */
/* wave-47: the LM-8->LM-9 grow (R_KEYV 8->16, R_VALV 32->64) widened the
 * VOCABULARY but kept wire_ver==MT_WIRE_VER_LANG and carried NO vocab id on
 * the wire. A peer still on the LM-8 word list could send ver-1 token ids
 * that pass the receiver's range check (v<R_VALV) yet MEAN a different word,
 * so the binding lands on the wrong answer — the scrambled / slot-shifted
 * answers seen on the first phone (sky->light, fire->stale, night->blue).
 * MT_WIRE_VER_VOCAB adds vocab_fp (the /vocab content-id fingerprint); a
 * receiver REFUSES an engram whose fingerprint != its own vocab. ver-1 is
 * now treated as an OLD/foreign word list and dropped by the version gate. */
#define MT_WIRE_VER_VOCAB   2    /* LM-9+: token ids + vocab_fp (refuse-on-    */
                                 /* vocab-mismatch; the live wire version)     */
#define MT_VOCAB_FP_LEN     8    /* key-id[0..3] ++ val-id[0..3] of /vocab     */
                                 /* content-ids: detects a different word list */
typedef struct {
    UW magic;                    /* MT_MAGIC                                */
    UW fact_seq;                 /* A's R3_FACT.seq — the autobiographical  */
                                 /* "when" (per-origin dedup high-water)    */
    U1 origin_node;              /* drpc_my_node of A (the teacher's node)  */
    U1 key, val;                 /* token ids (LM-8) — was k<8/v<4 (LM-7)   */
    U1 src;                      /* ARK_PROV_SRC_SHELL/WEB (carried for prov)*/
    U1 wire_ver;                 /* MT_WIRE_VER_VOCAB (mismatch = drop+print)*/
    U1 prov_head[PFS_ID_LEN];    /* content-id of A's ARK_PROV — resolves   */
                                 /* the teacher via the P1-replicated       */
                                 /* self/prov + self/prof (VIII.4)          */
    U1 vocab_fp[MT_VOCAB_FP_LEN];/* wave-47: vocab content-id fingerprint.   */
                                 /* Mismatch with the receiver's own vocab   */
                                 /* means the ids index a DIFFERENT word list*/
                                 /* -> REFUSE (engram analog of the weight-  */
                                 /* merge n_floats==R_NP guard, line ~2823). */
} __attribute__((packed)) MT_TEACH_PKT;   /* 45 + 8 = 53 B (<= KDDS_DATA_MAX)*/

/* Reserve the "mind/teach" topic slot at boot — call from the hosted
 * usermain right AFTER kdds_init() and BEFORE dkva_init(), so the cluster-
 * wide singleton gets a slot before dkva's per-node pre-opens saturate the
 * bounded topic table. Idempotent; no-op-safe on solo nodes. */
void mind_net_open(void);

/* The region subscriber task (VIII.9 FLAGGED). Created in BOTH hosted
 * usermains beside pfs_repl_task. The ONLY new public callable for arrival;
 * every arrival helper stays file-static in r3_incontext.c. */
void mind_net_task(INT stacd, void *exinf);

/* LM-10 (living-mind.md Part XI) — Path W: the one mind. The weight-states
 * themselves converge: after consolidation a node publishes its rw[] (84 KB,
 * chunked over content-addressed p-fs blocks under ONE per-origin manifest
 * ref) and gl_merge()s the region's set into ONE shared weight-state (the
 * Collective made literal at the weight level). The merge is no-central,
 * order-independent, region-scoped, death-surviving. `mind merge` drives it
 * on demand (M-b); mind_merge_task is the fleet-DMN slow-band pulse (M-a, the
 * "collective sleep" cadence). The disease/cure headline is MEASURED, not
 * asserted: `mind onemind` prints the 2x2 (node x fact) accuracy matrix of
 * the merged weights + classifies (a)/(b)/(c); `mind nocentral` proves
 * order-independence at n=R_NP. r3_weights_get/set are the ONLY new R3
 * surface (the dtr-accessor mirror; rw[] stays file-static). */
void r3_weights_get(float *out);            /* R_NP floats out of rw[]       */
void r3_weights_set(const float *in);       /* R_NP floats into rw[]         */
UW   r3_merge_epoch(void);                  /* the version high-water        */
void r3_onemind_test(void);                 /* the disease/cure cert (XI.4)  */
void r3_onemind_nocentral_test(void);       /* order-independence at n=R_NP  */
/* persistence SLICE 2 (docs/architecture/persistence.md): the learned mind
 * survives a reboot. r3_weights_persist() durably saves rw[] (header-guarded
 * by version+R_NP+vocab content-id, content-id no-op compare) — called by
 * the DMN after a consolidation tick. r3_weights_restore_or_pretrain() loads
 * it at boot IF the header matches the current build, else refuses + lets the
 * lazy pretrain rebuild (the wave-47 stale-weights trap, sealed). Both are
 * no-ops on bare metal / without PKERNEL_PFS_DIR (memory-only). */
INT  r3_weights_persist(void);              /* 1=wrote 0=no-op -1=durable err */
INT  r3_weights_restore_or_pretrain(void);  /* 1=restored 0=lazy-pretrain     */
/* the fleet-DMN slow-band merge pulse; created in both hosted usermains
 * beside mind_net_task (Path W's production cadence). */
void mind_merge_task(INT stacd, void *exinf);

/* galaxy v1 (docs/architecture/galaxy.md §6): a snapshot of the LAST
 * `mind ask <k>` result, written at the single site in m_ask where the
 * masked majority vote computes pred/share. The galaxy POST /ask bridge
 * calls mind_cmd("ask k") (the production mouth) then reads this — the
 * console output stays the verb's primary record; the JSON is a reading
 * of the same state, not a second path. *share is the modal class's
 * percent share *10 (e.g. 750 = 75.0%). */
void mind_last_answer(UB *k, UB *v, UW *share);

/* dtr_train.c — dataset + train/eval/save/load shell verbs.
 * args points just past "dtr"; handles
 *   eval | train [epochs] | save | load | grad | crash | stat */
void dtr_train_cmd(const UB *args, UW len);

/* ------------------------------------------------------------------ */
/* Wave 7 — task fault isolation (guard.c) integration                 */
/* ------------------------------------------------------------------ */

/* The guarded ring-0 inference worker (register via guard_register).
 * Idles polling the crash-injection flag; `dtr crash` makes it
 * corrupt the in-memory weights and write to NULL — the demo fault
 * that guard isolates and recovers from. */
void dtr_worker_task(INT stacd, void *exinf);

/* guard recover_fn: reload trained weights from the p-fs versioned
 * object "dtr/weights" (what `dtr save` wrote). Safe to call even if
 * the object does not exist (prints a warning, weights unchanged). */
void dtr_recover_weights(void);

/* `dtr crash` arms this; dtr_worker_task trips on it. */
extern volatile UB dtr_crash_req;
