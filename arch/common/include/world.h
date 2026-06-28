/*
 *  world.h
 *  Decentralized whole-network situational-awareness map (regions / Collective)
 *
 *  設計: docs/architecture/survival-network.md (第II部 §6 応援・受援 / §7 分散ゲーティング)
 *        docs/architecture/regions.md
 *
 *  「観測の分散版ゲーティング」。各ノードが自分の状態を周期的に self-beacon として
 *  publish し、受信したビーコンから *自分自身の* 全網ビュー (world-table) を組み立てる。
 *
 *  ── 不変条件 (NO-CENTRAL INVARIANT) ─────────────────────────────────
 *    どのノードも中央集約器ではない。各ノードは受信したゴシップ (K-DDS 経由の
 *    self-beacon) だけから自分の世界像を構築する。したがってどのノードを破壊しても
 *    地図そのものは消えない (§2 守る単位と守る力の分離 / §3 一点突破で殺せない構造)。
 *
 *    実装でこれを保証する仕組み:
 *      - 集約専用ノードや coordinator は一切存在しない。world_task は全ノードで同一に走る。
 *      - ビーコンは moe.c と同じ per-source topic "world/beacon/<node>" に publish される。
 *        単一 LATEST_ONLY スロットへ全員が上書きし合う集約点を作らない。
 *      - world-table は各ノードのローカル配列で、観測した範囲・古さ・欠落をそのまま
 *        持つ (設計の「古さ・不完全さ」の尊重)。完全な真実を持つ単一の場所は存在しない。
 *  ────────────────────────────────────────────────────────────────────
 */

#pragma once
#include "kernel.h"
#include "drpc.h"   /* DNODE_MAX */

/* ------------------------------------------------------------------ */
/* device_type — arch + role を 1 バイトに畳む                         */
/* ------------------------------------------------------------------ */

#define WORLD_DEV_UNKNOWN      0
#define WORLD_DEV_X86_BARE     1   /* boot/x86 ベアメタル                 */
#define WORLD_DEV_AARCH64_BARE 2   /* boot/aarch64 ベアメタル             */
#define WORLD_DEV_LINUX_X86_64 3   /* boot/linux_x86_64 ユーザモード      */
#define WORLD_DEV_LINUX_AARCH64 4  /* boot/linux ユーザモード (aarch64)   */
#define WORLD_DEV_ANDROID_UMP  5   /* UMP APK (Android NDK)               */

/* ------------------------------------------------------------------ */
/* self-beacon パケット (K-DDS 経由で per-source topic へ publish)      */
/*                                                                     */
/* LP64 トラップ対策: T-Kernel の UW/W は環境により long-based となり    */
/* LP64 で 8 バイトへ膨らむ。ワイヤ構造体には固定幅型のみを使い、       */
/* sizeof を _Static_assert で固定する (絶対に UW/W を載せない)。       */
/* ------------------------------------------------------------------ */

/* U1/U2/U4 = 真の固定幅 (typedef.h で unsigned char/short/int に無条件 typedef)。
 * T-Kernel の UB/UH は同じ実体だが、ワイヤ構造体では絶対に UW/W を使わない:
 * UW/W は環境によって long-based となり LP64 で 8 バイトへ膨らむため。 */
typedef struct {
    U1   node_id;       /* 発信ノード ID (0..DNODE_MAX-1)            */
    U1   device_type;   /* WORLD_DEV_*                              */
    U1   region_id;     /* 発信時の自 region ID (0xFF=未確立)       */
    U1   pressure;      /* 負荷軸 0..100 (LOAD; capacity 余力の逆 §6; 避ける) */
    U1   firing;        /* 発火ビットマスク (gate class ごと, §4)    */
    U1   region_size;   /* 自 region のメンバ数 (観測の補助)        */
    U1   threat;        /* 脅威軸 0..100 (THREAT; reflex CONSERVE §2; 寄る) */
    U1   atrisk;        /* G35/§5: 同時に at-risk な protected 点の数 (0..) */
    U4   seq;           /* 発信ごとに増える単調シーケンス            */
} __attribute__((packed)) WORLD_BEACON;   /* 12 bytes */

_Static_assert(sizeof(WORLD_BEACON) == 12, "WORLD_BEACON must be 12 bytes (LP64-safe wire)");
_Static_assert(sizeof(U1) == 1 && sizeof(U2) == 2 && sizeof(U4) == 4,
               "world wire fields must be true fixed-width");

/* firing ビット: gate class c が直近に発火したら bit c を立てる         */
#define WORLD_FIRE_BIT(c)   ((U1)(1u << (c)))
#define WORLD_FIRE_MASK     0x07   /* 下位 3 bit = MOE_NUM_CLASSES 個     */

/* selfc-ring3 galaxy hook (docs/architecture/selfc-ring3.md §8): a node
 * that germinated / rolled back a self-built unit sets this bit in the
 * beacon's `firing` byte for ONE beacon period — every peer's world-table
 * (and therefore the galaxy observation window) sees a star visibly
 * rebuilding itself, with zero new packet types. Bits 3..7 are free above
 * WORLD_FIRE_MASK; this uses bit 7 (0x80). */
#define WORLD_REBUILD_BIT   0x80
#define WORLD_BEACON_FIRE_MASK  (WORLD_FIRE_MASK | WORLD_REBUILD_BIT)

/* survival-loop L0 (docs/architecture/survival-loop.md §9, 司令官判断 2026-06-28):
 * the per-node 2-bit STATE rides in the firing byte's SPARE bits 3-4. The fire
 * classes use bits 0-2 (WORLD_FIRE_MASK) and selfc-ring3 uses bit 7
 * (WORLD_REBUILD_BIT), so bits 3-6 (0x78) are free. Packing the state here keeps
 * WORLD_BEACON at 12 bytes — NO wire change, NO version byte, NO dual-size
 * scheme — and an old node reads bits 3-4 = 0 = WSTATE_ACTIVE by default
 * (automatic back-compat). Same established method as WORLD_REBUILD_BIT(bit7)
 * and the N-2b SWIM capability gossip (_pad -> spare bit). 0x18 is disjoint from
 * WORLD_BEACON_FIRE_MASK (0x87), so every existing firing reader is unaffected. */
#define WORLD_STATE_SHIFT   3
#define WORLD_STATE_MASK    0x18   /* bits 3-4 = 2-bit state                    */

/* The node STATE enum carried in WORLD_STATE_MASK. Only ACTIVE/STRESSED are
 * emitted in L0; HIBERNATING/DYING are reserved for L2/L3 (NOT emitted yet). */
#define WSTATE_ACTIVE       0      /* default — LLM running, can be supported   */
#define WSTATE_STRESSED     1      /* shedding its own activity (non-threat S_n) */
#define WSTATE_HIBERNATING  2      /* reserved (L2): resource conservation       */
#define WSTATE_DYING        3      /* reserved (L3): apoptosis                    */

/* topic prefix: "world/beacon/" + 1 桁ノード ID                        */
#define WORLD_BEACON_TOPIC_PFX "world/beacon/"

#define WORLD_BEACON_MS   3000   /* self-beacon 発信間隔 (ms)            */
#define WORLD_POLL_MS     250    /* 近隣ビーコン取り込みのポーリング間隔 */

/* この時間 (ms) 受信が途絶えたエントリは stale 表示にする (古さの尊重)  */
#define WORLD_STALE_MS    9000   /* = 3 ビーコン周期                      */

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* usermain() の初期化ブロックで moe_init() の近くで呼ぶ。 */
void world_init(void);

/* self-beacon の発信 + 近隣ビーコンの取り込みを行う常駐タスク (pri 7)。
 * cmd_net から create_task で起動する。全ノードで同一に走る (中央なし)。 */
void world_task(INT stacd, void *exinf);

/* 受信した他ノードのビーコンを world-table に取り込む。
 * world_task のポーリングループから呼ぶ。 */
void world_observe(const WORLD_BEACON *b);

/* この瞬間の自ノードの逼迫度 (0..100) を計算する (degrade/capacity 由来)。
 * firing ビットを MoE 発火が立てるための公開フック。 */
void world_note_firing(UB gate_class);

/* selfc-ring3 §8 galaxy hook — emitted from EXACTLY ONE place (the germ
 * supervisor's germinate/rollback transition, arch/linux/selfc_proc.c).
 * Sets WORLD_REBUILD_BIT in the next beacon's firing byte for one period;
 * decays with the firing bits. No central collector. */
void world_note_rebuild(void);

/* shell `world` / `map` コマンド: 全網の状況をテキストで描画する。
 * 各既知ノードの id / device_type / region / alive・age / pressure bar /
 * firing インジケータを表示する。中央コレクタではなく、このノードが
 * 受信したゴシップから組み立てた *自分の* 世界像である。 */
void world_print(void);

/* ------------------------------------------------------------------ */
/* gating アクセサ (§7 分散ゲーティング — 局所勾配を読む窓口)          */
/*                                                                     */
/* これらは「このノードが受信したゴシップビーコンだけ」を読む。中央の  */
/* 真実 (broadcast score table のグローバル最大) ではなく、各ノードが   */
/* 局所に保持した近隣状態 (余力/逼迫度の勾配信号) を返す。select_expert */
/* はこの窓口越しにしか負荷を見ない (NO-CENTRAL 不変条件)。            */
/* ------------------------------------------------------------------ */

/* node のビーコンを一度でも受信したか (= 局所ビューに存在するか)。
 * 受信していなければ MoE は負荷項を諦め、安全側にフォールバックする。 */
BOOL world_peer_known(UB node);

/* node の逼迫度 (0..100) を局所ビューから読む。未知なら -1。
 * これが §6 の応援・受援の勾配信号 (負荷軸 LOAD): 高いほど逼迫 (避ける)、
 * 低いほど余力あり (応援を引き受けられる)。 */
INT  world_peer_pressure(UB node);

/* node の脅威度 (0..100) を局所ビューから読む。未知なら -1 (= 脅威なし扱い)。
 * これが §2 の一点集束の勾配信号 (脅威軸 THREAT; reflex CONSERVE が立てる):
 * 高いほど「守るべき/危険」→ moe ゲートで *加点* され群れがそこへ寄る (rally)。
 * load (world_peer_pressure) とは別軸・逆符号 (G20: 旧実装は両者を pressure
 * 1 本に畳んで「脅威=避ける」の符号倒錯を起こしていた)。 */
INT  world_peer_threat(UB node);

/* G35/§5: node が今ゴシップした「同時に at-risk な protected 点の数」を局所
 * world-table から読む。未知なら -1。threat (単一スカラ = aggregate rally 信号)
 * が畳んでしまう *多点性* をここで perceive する: 近傍は「あのノードは 1 点で
 * なく 3 点を同時に守っている」と見える。単一意識のボトルネックを乗り越える
 * 観測軸 (§5「同時に数百件…並行」)。中央集約なし — 各 node 自身が数えた値。 */
INT  world_peer_atrisk(UB node);

/* node が gossip で広告した「自 region の coordinator ID (region_id)」を
 * 局所 world-table から読む。未知 / 未広告 (0xFF) なら -1 (wave 10, G2)。
 * dkva の requester はこれで他 region をまとめ、欠けた region を degraded に
 * 正直計上する (中央の真実ではなく、受信したビーコンだけから組む)。 */
INT  world_peer_region(UB node);

/* world_peer_region と同じだが、ビーコンが新鮮 (age <= WORLD_STALE_MS) な
 * ときだけ region を返す (G12, wave 12)。未受信 / 古い / 未広告なら -1。
 * dkva は「新鮮に確認できた」region だけで degraded の分母を組み、確認できない
 * remote は uncertain として別計上する (degraded の数字を gossip 鮮度に依存
 * させない = I4/I16 honesty)。 */
INT  world_peer_region_fresh(UB node);

/* galaxy v1 (galaxy.md §9): the beacon-carried device_type of a peer
 * (WORLD_DEV_*), -1 if unknown. The galaxy is world.c's FACE. */
INT  world_peer_device(UB node);

/* galaxy v1: ms since `node`'s beacon was last freshly observed, -1 if
 * unknown — so the window fades stale peers honestly (古さの尊重). */
INT  world_peer_age_ms(UB node);

/* テスト専用 (G12 デモ): 起動後 ms ミリ秒だけ self-beacon を抑止し、gossip を
 * 意図的に未収束のまま保つ。0 = 無効 (既定; 本番挙動は不変)。 */
void world_set_beacon_hold(UW ms);

/* ------------------------------------------------------------------ */
/* survival-loop L0 — per-node STATE FSM + gossip accessor (hosted)    */
/*                                                                     */
/* docs/architecture/survival-loop.md §1.1 / §6-L0 / §9. The survival  */
/* loop runs on the FLEET (boot/linux + Android), never on the QEMU    */
/* bare-metal targets — so the FSM and its accessor are hosted-only    */
/* (`_TK_HOSTED_LIBC_`). Bare-metal keeps state bits = 0 = ACTIVE with */
/* ZERO new code (the crown stays byte-identical).                     */
/* ------------------------------------------------------------------ */
#ifdef _TK_HOSTED_LIBC_

/* Advance the self STATE FSM one tick from the live S_n bus (axis-dependent,
 * 2-time-constant hysteresis) and return the committed state (WSTATE_*). The
 * ONE production call site is world_task's periodic self-update. */
UB  world_self_state_step(void);

/* The current committed self STATE (WSTATE_*), without advancing the FSM. */
UB  world_self_state(void);

/* Read peer `node`'s gossiped STATE from this node's local world-table (mirrors
 * world_peer_pressure): the 2 bits WORLD_STATE_MASK of the beacon firing byte.
 * Returns WSTATE_* (0..3) for a known peer (a 12-byte old beacon with those bits
 * 0 reads as WSTATE_ACTIVE), or -1 if `node` is unknown. */
INT world_peer_state(UB node);

/* Hosted cert (tests/host/run_survival_l0.sh): [state-axis] (axis-dependence is
 * load-bearing — THREAT@hi -> ACTIVE, other axes@hi -> STRESSED) + [state-gossip]
 * (stamp -> observe -> world_peer_state). Returns 0 = PASS. Built with
 * -DSURVIVAL_L0_MONOTONE the FSM is forced monotone and [state-axis] goes RED. */
INT world_survival_l0_test(void);

#endif /* _TK_HOSTED_LIBC_ */
