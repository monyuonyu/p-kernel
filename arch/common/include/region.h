/*
 *  region.h
 *  Regions — 遅延クラスタによる脳の領域分割 (R0)
 *
 *  設計: docs/architecture/regions.md
 *
 *  Region とは「相互 RTT が閾値 REGION_TAU_MS 以内のノード集合」。
 *  物理的・ネットワーク的に近いノードが1つの region を成し、region 内は
 *  密に (DKVA / MoE / replica)、region 間は疎に通信する。
 *
 *  v1 (このファイル) は各ノードの自己中心的 (egocentric) な局所ビュー:
 *    自 region = { 自分 } ∪ { ALIVE かつ RTT≤τ のノード }
 *    coordinator = region 内の最小ノード ID (決定的)
 *  RTT の非対称性やメンバシップ差により、ノード間でビューが完全一致する
 *  保証はまだ無い (region 間の合意は regions.md の未解決問題)。それでも
 *  「近さで束ねる」プリミティブとして、K-DDS スコープや locality-MoE が
 *  この上に乗れる。
 */

#pragma once
#include "drpc.h"   /* dnode_table[], DNODE_MAX, DNODE_ALIVE, drpc_my_node */

/* RTT がこの値 (ms) 以内なら同一 region とみなす */
#define REGION_TAU_MS   50

/* 現在の RTT / membership から自 region を再計算する。
 * 安価 (O(DNODE_MAX)) なので問い合わせ時に都度呼んでよい。 */
void region_recompute(void);

/* 自 region の ID (= coordinator のノード ID)。
 * 分散モード未確立 (drpc_my_node==0xFF) なら 0xFF。 */
UB   region_id(void);

/* 自 region の coordinator ノード ID (region 内の最小 ID)。 */
UB   region_coordinator(void);

/* node が自分と同じ region に属するか (内部で region_recompute する)。 */
BOOL region_contains(UB node);

/* 最後の region_recompute() 結果を再計算せずに読む (ホットパス用)。
 * 呼ぶ前に region_recompute() を一度実行しておくこと。 */
BOOL region_is_member(UB node);

/* 自 region のメンバ数 (自分を含む)。 */
UB   region_size(void);

/* shell `region` 表示。 */
void region_print(void);

/* ------------------------------------------------------------------ */
/* Supernode selection (N-2 first slice — selection function only)     */
/*                                                                     */
/* p2p-overlay.md "Supernodes (N-2)": the per-region supernode is the   */
/* LOWEST-id node that is BOTH a current region member AND              */
/* supernode-capable, recomputed locally by all → convergence with no   */
/* vote, survives death by recomputation.                              */
/* DEFERRED to later slices: SWIM capability gossip, supernode packet   */
/* forwarding, NAT hole-punch. Capability is a LOCAL table for now.    */

/* The self's per-region supernode for the current local view; 0xFF if  */
/* no capable member exists (fall back to the central relay).          */
UB   region_supernode(void);

/* Mark/unmark a node as supernode-capable (reachable + volunteered).   */
/* Local only this slice — NOT yet gossiped over SWIM.                 */
void region_set_super_capable(UB node, BOOL yes);
BOOL region_is_super_capable(UB node);

/* Read the self capability opt-in once (env PKERNEL_SUPERNODE=1 on      */
/* hosted nodes; default NOT capable). Idempotent. */
void region_super_init(void);

/* Host cert: deterministic-selection / convergence / survives-death /   */
/* relay-fallback. Prints PASS/FAIL + "[region-super] N PASS, M FAIL".  */
void region_supernode_test(void);

/* ------------------------------------------------------------------ */
/* Teacher selection (T-fix-a — thread-t-impl-plan.md §2.3)            */
/*                                                                     */
/* EXACT mirror of the supernode selector for the Cradle teacher: the  */
/* per-region teacher is the LOWEST-id node that is BOTH a current      */
/* region member AND teacher-capable (holds a loaded teacher GGUF),     */
/* recomputed locally by every node → convergence with NO vote, and it  */
/* survives the teacher's death by recomputation (kill the teacher →    */
/* member[]=0 → next teacher-capable id wins). 0xFF = no teacher this   */
/* region (degrade: the child keeps its ring / falls back to fixture).  */
/* SWIM bit 1 (capability>>1) feeds this table verbatim under the same  */
/* LWW gate as the supernode bit; teacher SELECTION only — the lesson   */
/* transport (CRADLE_TEACH_PKT + p-fs body) is T-fix-b/T-1, deferred. */

/* The self's per-region teacher for the current local view; 0xFF if no */
/* teacher-capable member exists. */
UB   region_teacher(void);

/* Mark/unmark a node as teacher-capable. Fed VERBATIM by swim.c's      */
/* gossip_apply (capability bit 1) under the same anti-stale LWW gate.  */
void region_set_teacher_capable(UB node, BOOL yes);
BOOL region_is_teacher_capable(UB node);
