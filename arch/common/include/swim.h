/*
 *  swim.h (x86)
 *  SWIM — Scalable Weakly-consistent Infection-style Membership
 *
 *  Phase 1: Biotic Mesh Network
 *
 *  既存の drpc ハートビートに以下を追加する:
 *    - 直接プローブ   (SWIM_PING / SWIM_ACK) — 1 ラウンドに 1 ノードを選択
 *    - 間接プローブ   (SWIM_PING_REQ) — K=2 台のヘルパーに代理プローブを依頼
 *    - ゴシップ伝播   — 全 SWIM パケットにメンバーシップ変化をピギーバック
 *
 *  ノードテーブル: drpc と共有 (dnode_table[] / drpc_my_node)
 *  UDP ポート    : SWIM_PORT 7375
 *
 *  プローブラウンド (SWIM_PROBE_INTERVAL_MS ごと):
 *    1. ラウンドロビンで次のピアを選ぶ
 *    2. SWIM_PING を送り、SWIM_PROBE_TMO_MS 以内の SWIM_ACK を待つ
 *    3. ACK なし → K 台のヘルパーに SWIM_PING_REQ を送り、SWIM_IND_TMO_MS 待つ
 *    4. それでも無応答 → 状態を ALIVE→SUSPECT→DEAD に昇格 + ゴシップ
 *    5. 正常応答があれば ALIVE を確認 + 状態変化があればゴシップ
 */

#pragma once
#include "drpc.h"   /* dnode_table[], DNODE_MAX, DNODE_*, drpc_my_node */

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define SWIM_PORT              7375
#define SWIM_GOSSIP_MAX        3        /* 1 パケットあたりのゴシップ数      */
#define SWIM_K_HELPERS         2        /* 間接プローブのヘルパー数          */
#define SWIM_PROBE_INTERVAL_MS 1000     /* プローブラウンド間隔 (ms)         */
#define SWIM_PROBE_TMO_MS      400      /* 直接プローブタイムアウト          */
#define SWIM_IND_TMO_MS        500      /* 間接プローブタイムアウト          */
#define SWIM_SUSPECT_ROUNDS    2        /* ALIVE 連続無応答 → SUSPECT       */
#define SWIM_DEAD_ROUNDS       3        /* SUSPECT ラウンド数 → DEAD        */

/* ------------------------------------------------------------------ */
/* パケットフォーマット                                                */
/* ------------------------------------------------------------------ */

#define SWIM_MAGIC    0x4D494D53UL   /* "SWIM" little-endian */
/* N-2b: NOT bumped. The capability bit reuses the already-reserved zero
 * `_pad` byte of SWIM_GOSSIP_EVT, so the wire layout/size are byte-identical
 * to v1. Bumping the version would make v1 nodes (which gate on
 * `version != SWIM_VERSION` in swim_rx) DROP the whole packet — losing
 * membership/gossip interop — for a strictly-additive, zero-default field.
 * Mixed fleet: an old node emits capability=0 → read as non-capable →
 * relay fallback (safe); a new node ignores the field on old peers and
 * never crashes. So compatibility is best served by keeping v1. */
#define SWIM_VERSION  1

#define SWIM_PING      0x20   /* 直接プローブ; SWIM_ACK を期待         */
#define SWIM_ACK       0x21   /* 生存確認の返答                         */
#define SWIM_PING_REQ  0x22   /* probe_target への代理プローブを依頼    */

/* ゴシップイベント 1 件 */
typedef struct {
    UB  node_id;
    UB  state;          /* DNODE_ALIVE / DNODE_SUSPECT / DNODE_DEAD */
    UB  incarnation;    /* より新しい incarnation で古い疑惑を上書き */
    /* N-2b supernode capability bit (p2p-overlay.md "Supernodes (N-2)").
     * Was the reserved zero `_pad`; reusing it is WIRE-COMPATIBLE — the
     * layout, size (24B packet) and SWIM_VERSION are unchanged, and old
     * emitters already zero this byte. An old node therefore sends 0 and
     * a new node reads it as "non-capable" → safe degrade to the relay.
     * SELF-AUTHORITATIVE: only node X's own gossip about X sets a real
     * value here; every other node relays the byte VERBATIM (epidemic),
     * it never originates a peer's capability. Converges in lock-step with
     * membership state under the SAME (incarnation,state) last-writer-wins
     * gate, so a stale rumor cannot flip it. */
    UB  capability;     /* 1 = node self-declares supernode-capable */
} __attribute__((packed)) SWIM_GOSSIP_EVT;

/* SWIM パケット本体 (24 bytes) */
typedef struct {
    UW  magic;                               /* SWIM_MAGIC              */
    UB  version;                             /* SWIM_VERSION            */
    UB  type;                                /* SWIM_PING/ACK/PING_REQ  */
    UH  seq;                                 /* PING↔ACK 照合用         */
    UB  src_node;
    UB  probe_target;   /* PING_REQ: プローブ対象; ACK: 確認対象       */
    UB  gossip_cnt;     /* gossip[] の有効エントリ数 (0..SWIM_GOSSIP_MAX) */
    UB  _pad;
    SWIM_GOSSIP_EVT gossip[SWIM_GOSSIP_MAX]; /* ピギーバックイベント    */
} __attribute__((packed)) SWIM_PKT;

/* N-2b wire-identity tripwire: the capability bit reused the reserved `_pad`,
 * so the on-wire layout MUST stay byte-identical (entry=4B, packet=24B) or
 * old↔new membership interop silently breaks. Make that claim self-enforcing
 * at compile time (an audit nit: the docs said "static-checked" — now it is). */
_Static_assert(sizeof(SWIM_GOSSIP_EVT) == 4,
               "SWIM_GOSSIP_EVT must stay 4 bytes (wire-compatible with pre-N-2b nodes)");
_Static_assert(sizeof(SWIM_PKT) == 24,
               "SWIM_PKT must stay 24 bytes (wire-compatible with pre-N-2b nodes)");

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* drpc_init() の後に呼ぶ。SWIM_PORT へ UDP バインドする。 */
void swim_init(void);

/* T-Kernel タスク本体 (優先度 6, スタック 4096)。
 * プローブループを実行し dnode_table[] を in-place 更新する。 */
void swim_task(INT stacd, void *exinf);

/* UDP 受信コールバック (swim_init() が SWIM_PORT に登録する)。 */
void swim_rx(UW src_ip, UH src_port, const UB *data, UH len);

/* SWIM 強化版クラスタ表示 (shell `nodes` から呼ぶ)。 */
void swim_nodes_print(void);

/* ノードへの平滑化 RTT (ms)。直接プローブの往復から EWMA で推定。
 * 未実測のノードは 0xFFFFFFFF を返す。
 * (R0, regions design — docs/architecture/regions.md。region 形成と
 *  locality-aware MoE ゲーティングが消費する。) */
UW swim_rtt_ms(UB node);

/* RTT シミュレーション注入 (テスト専用)。zone = node_id / zone_size とし、
 * 異 zone のピアへの観測 RTT に penalty_ms を上乗せする。zone_size==0 で
 * 無効。localhost で人工的に複数 region を作って region 形成を検証するため。
 * arch 非依存を保つため env は読まず、linux usermain が呼ぶ。 */
void swim_set_sim_zone(UB zone_size, UW penalty_ms);

/* SWIM-INCARN cert (external audit 2026-06-13): drives the REAL gossip_apply
 * with crafted SWIM_PKTs and asserts canonical anti-stale-rumor behaviour —
 * a fresh-incarnation ALIVE refutes a stale DEAD rumor about SELF (the node
 * is NOT marked dead) and a peer's stale lower-incarnation DEAD does not
 * override a higher-incarnation ALIVE. Emits "[swim-incarn] PASS"/"FAIL ...".
 * Returns 0 on PASS, else the fail count. */
INT swim_incarn_self_test(void (*emit)(const char *));

/* N-2b cap-gossip cert (p2p-overlay.md "Supernodes (N-2)"): drives the REAL
 * gossip_apply() with crafted SWIM_PKTs carrying the capability bit and
 * asserts (a) a fresh self-rumor with capability=1 makes the receiver report
 * region_is_super_capable(X)==TRUE and region_supernode() select X (multi-node
 * convergence, NO vote / NOCENTRAL); (b) a STALE lower-incarnation rumor does
 * NOT flip capability (reuses the incarnation gate), a fresh higher one does;
 * (c) FALSIFIABLE — the same craft FAILS if the apply ignored the byte or let
 * a third party originate it. Emits "[cap-gossip] ..." lines; returns 0 on
 * PASS else the fail count. */
INT swim_cap_gossip_self_test(void (*emit)(const char *));
