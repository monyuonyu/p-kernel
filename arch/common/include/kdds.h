/*
 *  kdds.h (x86)
 *  K-DDS — カーネルネイティブ Data Distribution Service
 *
 *  Phase 2: 「すべてはトピック」
 *
 *  Unix の「すべてはファイル」に対して、p-kernel はトピックを一級市民とする。
 *  アプリケーションは sys_topic_pub/sub を呼ぶだけで、同一ノード内・
 *  複数ノード間を問わず同じ API でデータを共有できる。
 *
 *  syscall API:
 *    SYS_TOPIC_OPEN  (0x220) — トピックを開く / 作成する
 *    SYS_TOPIC_PUB   (0x221) — データを発行する
 *    SYS_TOPIC_SUB   (0x222) — データを受信する (ブロッキング)
 *    SYS_TOPIC_CLOSE (0x223) — ハンドルを閉じる
 *
 *  QoS ポリシー:
 *    KDDS_QOS_BEST_EFFORT  — 順序・信頼性保証なし
 *    KDDS_QOS_RELIABLE     — 遅延参加者にも最新値を配信 (store-and-forward)
 *    KDDS_QOS_LATEST_ONLY  — 最新値のみ保持 (センサーデータ向け)
 *
 *  リモート配送 (分散モード):
 *    kdds_pub() 呼び出し時に drpc_my_node != 0xFF なら、
 *    ALIVE 状態の全ノードへ KDDS_PKT を UDP ブロードキャストする。
 *    各受信ノードはローカルの subscriber に配信する。
 */

#pragma once
#include "kernel.h"
#include "drpc.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define KDDS_PORT        7376
#define KDDS_SINGLETON_TOPICS 16 /* cluster-wide SINGLE topics (NOT per-node):
                                 * pfs ann/want/sync/ref + "mind/teach" (LM-7,
                                 * the shared mind) + "mind/w" (LM-10 Path W merge
                                 * announce) + "mind/want" (LM-15 pull-teach, the
                                 * QUESTION channel) + "cradle/teach" + headroom.
                                 * dkva pre-opens fill the per-node budget
                                 * (3×DNODE_MAX) exactly, leaving no slot for a
                                 * cluster-singleton, so these get their OWN fixed
                                 * headroom — they do NOT scale with DNODE_MAX.
                                 * LM-15 ledger (living-mind-lm15-pullteach.md §3):
                                 * eager-at-boot singletons 7 -> 8 of 16; named
                                 * worst-case co-active set 15 of 16 (one spare).
                                 * The NEXT cluster-singleton must bump 16 -> 20. */
/* unbounded_n_design.md §3 — the wave-48 fix, made permanent for the dkva
 * pre-open. The topic budget is sized by the REGION capacity DREGION_MAX (R),
 * an INDEPENDENT literal (drpc.h), NOT by fleet size N. The dkva coordinator
 * pre-open (dkva.c) is now bounded by R (≤3×R) — so a growing fleet (wire-v2,
 * U-2) can NEVER re-trigger the wave-48 overflow from THAT source, because R
 * does not move with N (proven by the [unbounded-coupling] probe: bump
 * DNODE_MAX 64→1024, KDDS_TOPIC_MAX stays 400).
 *   HONEST SCOPE: the moe(score)/world(beacon)/reflex/edf families still open
 * LAZILY per SOURCE node id (not yet per-region-slot) — their conversion to
 * the nodemap primitive is deferred (U-0 remainder). This budget bounds the
 * dkva pre-open by R; it does not yet make every lazy family R-bounded.
 * At R==DNODE_MAX(=64) the table is byte-identical to the historical
 * 6*DNODE_MAX (one region == the whole ≤64-node fleet). */
#define KDDS_TOPIC_MAX   (6 * DREGION_MAX + KDDS_SINGLETON_TOPICS)
                                /* 6×, not 5× (wave-48 fix, found by mk_pino's
                                 * phone boot log): with the FULL net stack up
                                 * (autonet, as the Android app always runs),
                                 * the EAGER per-node opens alone are 5 families
                                 * (dkva q/resp/rsum + moe/score + world/beacon)
                                 * = 5*DNODE_MAX, and singletons (mind/teach,
                                 * mind/w, mind/want, dtr l0/result/input/head1,
                                 * pfs ann/want/sync/ref, ...) plus the LAZY
                                 * per-source families (reflex/edf) landed on a
                                 * table that was already full -> 65x "topic
                                 * table full" at boot on DNODE_MAX=64; the
                                 * star was blind to world/beacon of nodes
                                 * 46..63. G23 kept the 32-node 5x ratio but
                                 * never booted a 64-table AUTONET node. */
                                /* カーネルが同時に管理できるトピック数。
                                 * dkva が q/resp/rsum を per-node (=3×DNODE_MAX) で
                                 * 全枠 pre-open し、moe(score)/world(beacon)/reflex/
                                 * edf が per-source トピックを遅延 open するため、
                                 * DNODE_MAX に比例させる (ONE source of truth)。
                                 * G23: DNODE_MAX を 32→64 に倍化したのに伴い、
                                 * 32 で検証済みの比率 (160=5×32) をそのまま保つ。
                                 * LM-7: + KDDS_SINGLETON_TOPICS for the cluster-
                                 * wide "mind/teach" topic (per-node 枠は満杯)。 */
#define KDDS_HANDLE_MAX  (10 * DREGION_MAX)  /* 同時オープンハンドル数 (pub/sub 各ハンドル)。
                                 * dkva は q/resp/rsum の pub+sub=6 ハンドルを
                                 * REGION-LOCAL スロットごとに開く (=6×R, dkva.c の
                                 * pre-open)。DREGION_MAX(=R) に比例 — 艦隊 N には比例
                                 * しない (§3, [unbounded-coupling] が証明)。moe/world 等の
                                 * lazy per-source open は未変換 (U-0 残り, honest)。
                                 * R==DNODE_MAX なので現行は 640 で byte-identical。 */

/* ANTI-THEATER compile-time gate (unbounded_n_design.md §3/§8) — the PERMANENT
 * wave-48 gate. dkva pre-opens q/resp/rsum for each REGION-LOCAL slot at boot:
 * 3 topics × KDDS_DKVA_PREOPEN_SCALE, where the scale is the region capacity R
 * (DREGION_MAX) — NOT fleet N. The gate asserts that region-local pre-open FITS
 * the topic table with room for the eager cluster-singletons.
 *
 * This is NOT the tautology `KDDS_TOPIC_MAX == 6*R+16` (which merely restates
 * the #define and proves nothing — the fake the audit rejected). It relates two
 * INDEPENDENTLY-defined quantities: the dkva pre-open COUNT and the table SIZE.
 * The [unbounded-disease] binary (tests/unbounded/disease.c) re-couples the
 * pre-open to fleet N — it builds with
 *     -DKDDS_DKVA_PREOPEN_SCALE=DNODE_MAX -DDNODE_MAX=1024
 * and THIS gate then fails to compile: 3*1024+16 > 6*64+16. That is the exact
 * wave-48 overflow (pre-open ∝ fleet against a table sized ∝ R), now caught at
 * BUILD time on every kernel target — not a printf in a cert. */
#ifndef KDDS_DKVA_PREOPEN_SCALE
#define KDDS_DKVA_PREOPEN_SCALE  DREGION_MAX   /* region-local (the cure)      */
#endif
#define KDDS_DKVA_PREOPEN        (3 * (KDDS_DKVA_PREOPEN_SCALE))
_Static_assert(KDDS_DKVA_PREOPEN + KDDS_SINGLETON_TOPICS <= KDDS_TOPIC_MAX,
               "dkva region-local pre-open (3*R) + singletons must FIT the "
               "topic table; a fleet-N pre-open (wave-48) overflows it (§3)");
/* The handle table holds pub+sub for each pre-opened topic (2×) — sized by R
 * for the same reason; the pre-open must also fit the handle budget. */
_Static_assert(2 * KDDS_DKVA_PREOPEN <= KDDS_HANDLE_MAX,
               "dkva pre-open pub+sub handles (6*R) must FIT the handle table (§3)");
#define KDDS_NAME_MAX    32     /* トピック名の最大長 (null 含む)          */
#define KDDS_DATA_MAX    192    /* トピックデータの最大バイト数 (DKVA_RESP_PKT=172B が必要) */
#define KDDS_SUB_MAX     4      /* トピックあたりの最大サブスクライバ数    */

/* QoS ポリシー */
#define KDDS_QOS_BEST_EFFORT  0
#define KDDS_QOS_RELIABLE     1
#define KDDS_QOS_LATEST_ONLY  2

/* 配信スコープ (regions, R0 — docs/architecture/20-architecture/regions.md)
 *   GLOBAL: 従来どおり全ノードへ送る (フラット broadcast)
 *   REGION: 自 region のメンバ (RTT≤τ) にだけ送る — region 内に閉じた密通信。
 * スコープは送信時にローカルで強制され、ワイヤ (KDDS_PKT) には載らない。 */
#define KDDS_SCOPE_GLOBAL     0
#define KDDS_SCOPE_REGION     1

/* ------------------------------------------------------------------ */
/* 内部トピックスロット (カーネル側)                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char  name[KDDS_NAME_MAX];  /* トピック名 (例: "sensor/temperature") */
    UB    data[KDDS_DATA_MAX];  /* 最新の発行データ                       */
    UH    data_len;             /* データバイト数 (0 = 未発行)            */
    UH    data_seq;             /* 複製判定用: publish ごとにインクリメント */
    UB    qos;                  /* KDDS_QOS_*                             */
    UB    scope;                /* KDDS_SCOPE_* (配信範囲)                */
    UB    open;                 /* 1 = 使用中                             */
} KDDS_TOPIC;

extern KDDS_TOPIC kdds_topics[KDDS_TOPIC_MAX];

/* ------------------------------------------------------------------ */
/* ハンドルテーブル (カーネル側)                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    W   topic_idx;  /* kdds_topics[] へのインデックス (-1 = 未使用)  */
    ID  sub_sem;    /* subscriber がブロックするセマフォ (-1 = なし) */
    ID  owner;      /* このハンドルを開いたタスク id (0 = カーネル所有 / 所有者なし)。
                     * MIN_TSKID=1 なので 0 は決して有効な task id ではなく、
                     * 「所有者なし」の番兵として安全に使える。SYS_TOPIC_OPEN
                     * から開かれた ring3 デーモンのハンドルだけが非ゼロ owner を
                     * 持ち、kdds_close_by_owner() の teardown で回収される。 */
    UB  open;       /* 1 = 使用中                                    */
} KDDS_HANDLE_SLOT;

extern KDDS_HANDLE_SLOT kdds_handles[KDDS_HANDLE_MAX];

/* ------------------------------------------------------------------ */
/* ネットワークパケット                                                */
/* ------------------------------------------------------------------ */

#define KDDS_MAGIC      0x5344444BUL   /* "KDDS" little-endian */
#define KDDS_VERSION    1
#define KDDS_DATA_PKT   0x01           /* データ配信パケット             */

typedef struct {
    UW   magic;                    /* KDDS_MAGIC                         */
    UB   version;                  /* KDDS_VERSION                       */
    UB   type;                     /* KDDS_DATA_PKT                      */
    UB   src_node;
    UB   _pad;
    UH   data_len;                 /* data[] の有効バイト数              */
    UH   name_len;                 /* name[] の有効バイト数 (null 含む)  */
    char name[KDDS_NAME_MAX];
    UB   data[KDDS_DATA_MAX];
} __attribute__((packed)) KDDS_PKT;  /* 4+1+1+1+1+2+2+32+128 = 172 bytes */

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* 初期化 — usermain() から drpc_init() の後に呼ぶ */
void kdds_init(void);

/* トピックを開く / 作成する。ハンドル (0..KDDS_HANDLE_MAX-1) を返す。
 * 失敗時は負のエラーコード。スコープは KDDS_SCOPE_GLOBAL。 */
W kdds_open(const char *name, W qos);

/* スコープ指定版。新規トピックなら scope で作成、既存なら scope を更新する。
 * (regions R0 — region-scoped topic は自 region メンバにだけ配信される) */
W kdds_open_scoped(const char *name, W qos, W scope);

/* poll-only オープン: subscriber 用セマフォを作らずハンドルだけ確保する。
 * LATEST_ONLY + kdds_sub(timeout=0) のポーリング購読/発行専用。ブロッキング
 * 待ちは不可 (timeout<0/>0 は即エラーを返す)。per-source topic を大量に開く
 * 購読者 (world/moe の世界マップ等) が CFN_MAX_SEMID を枯渇させないための
 * 軽量オープン。スコープは GLOBAL。 */
W kdds_open_poll(const char *name, W qos);

/* poll-only オープンの scope 指定版 (wave 10, G1)。kdds_open_poll と同じく
 * セマフォを 1 つも消費しないが、REGION スコープ等を指定できる。dkva の
 * per-origin/per-source トピック (q=GLOBAL, resp=REGION, rsum=GLOBAL) は
 * すべて timeout=0 ポーリングで読むのでブロッキング sem が要らない →
 * これで開けば dkva は CFN_MAX_SEMID を 1 つも使わない。 */
W kdds_open_poll_scoped(const char *name, W qos, W scope);

/* 直近の kdds_pub() が UDP 送信したピア数 (fanout)。スコープの効果を観測する
 * デモ/検証用。GLOBAL なら全 peer 数、REGION なら region メンバ数になる。 */
UW kdds_pub_fanout(void);

/* 局所性 (locality) 計測カウンタ — wave-12 / G25 (§4 の数値検証)。
 * kdds_pub() の累積配送数/バイトを「近傍 (同 region)」と「遠方 (異 region)」
 * に分けて読む。NULL を渡した項目はスキップ。`kdds` シェルコマンド
 * (kdds_list) の [locality] 行でも同じ値が見える。 */
void kdds_locality_stats(UW *msgs, UW *cross, UW *bytes, UW *bytes_cross);

/* データをトピックへ発行する。
 * ローカルの subscriber を起こし、分散モードなら全 ALIVE ノードへ送信。
 * 成功時は 0、失敗時は負のエラーコード。 */
W kdds_pub(W handle, const void *data, W len);

/* トピックの次の値を受信する (ブロッキング)。
 * buf へデータをコピーし、受信バイト数を返す。
 * timeout_ms: -1=無限待ち, 0=ポーリング。
 * 失敗時は負のエラーコード。 */
W kdds_sub(W handle, void *buf, W buflen, W timeout_ms);

/* ハンドルを閉じる */
void kdds_close(W handle);

/* このハンドルの所有タスク id を記録する (SYS_TOPIC_OPEN から ring3 デーモンの
 * 開いたハンドルに刻む)。tid<=0 は無視 (= 所有者なしのまま)。所有者付きハンドル
 * だけが kdds_close_by_owner() の teardown 掃き出し対象になる。
 * KDDS-HANDLE-LEAK fix (wave-56): 殺されたデーモンの ai/req・ai/rsp ハンドルが
 * teardown で閉じられず、churn でハンドルテーブルを枯渇させていた。 */
void kdds_set_owner(W handle, ID tid);

/* tid が所有する全ハンドルを閉じる (kdds_close と同じ後始末)。タスク teardown
 * から呼ぶ。tid<=0 や owner==0 (カーネル所有) のハンドルには決して触れない —
 * dproc/dkva/world 等のカーネル内部ハンドルは tid<=0 ではマッチしない。
 * 閉じた数を返す。 */
INT kdds_close_by_owner(ID tid);

/* 現在オープン中のハンドル数 (リーク計測 / churn harness 用)。 */
INT kdds_handle_count(void);

/* UDP 受信コールバック (KDDS_PORT に登録) */
void kdds_rx(UB src_node, UH dst_port, const UB *data, UH len);

/* トピックテーブルを表示 (shell `topic list`) */
void kdds_list(void);

/* トピックをクラスタ全体から削除する (tombstone gossip で全ノードへ伝播)。
 * ローカルスロットを解放し、分散モードでは replica_tombstone() を呼ぶ。 */
void kdds_delete_cluster(const char *name);

/* KDDS-DELCLUSTER cert (external audit 2026-06-13): opens handles on a cluster
 * topic, deletes the cluster via the REAL kdds_delete_cluster, and confirms NO
 * handle to the deleted topic remains open (the old empty close-loop leaked
 * them) while an unrelated topic's handle is untouched. Emits
 * "[kdds-delcluster] PASS"/"FAIL ...". Returns 0 on PASS, else the fail count. */
INT kdds_delcluster_self_test(void (*emit)(const char *));
