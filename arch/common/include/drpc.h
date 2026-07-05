/*
 *  drpc.h (x86)
 *  Distributed T-Kernel RPC over UDP
 *
 *  Two QEMU instances share a virtual Ethernet cable (socket networking).
 *  The same bootloader.bin binary detects its node ID from the MAC address
 *  at boot time — no recompilation needed per node.
 *
 *    Terminal 0:  make run-node0   →  Node 0  IP=10.1.0.1  MAC=52:54:00:00:00:01
 *    Terminal 1:  make run-node1   →  Node 1  IP=10.1.0.2  MAC=52:54:00:00:00:02
 *
 *  Protocol (UDP port 7374):
 *    HEARTBEAT  500 ms broadcast → peer discovery + keep-alive
 *    REQ/REPLY  synchronous RPC (3 s timeout)
 *
 *  Distributed T-Kernel API:
 *    dtk_cre_tsk(node, func_id, pri)   create task on any node
 *    dtk_cre_sem(isemcnt)              create semaphore, returns global ID
 *    dtk_sig_sem(global_semid, cnt)    signal (routes over network if remote)
 *    dtk_wai_sem(global_semid, cnt, t) wait  (semaphore must live on local node)
 *
 *  Global Object ID encoding:
 *    bits 31..24 = node_id (0-254; 0xFF=sentinel; capped at DNODE_MAX)
 *    bits 23.. 0 = local T-Kernel object ID
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define DRPC_PORT       7374        /* UDP port for all DRPC traffic     */
/* DNODE_MAX is overridable at compile time (-DDNODE_MAX=…) so the
 * [unbounded-*] coupling probe (tests/unbounded) can WIDEN the wire ceiling
 * to 256/1024 and prove the region-local (R-sized) cost stays constant while
 * this fleet-sized table grows. The shipping default is unchanged: 64. */
#ifndef DNODE_MAX
#define DNODE_MAX       64          /* max nodes in cluster (node 0-63)
                                     * Bounded by the 8-bit node_id field
                                     * (UB src_node/dst_node on the wire and
                                     * obj_id bits 31..24). Valid ids are
                                     * 0..254 (0xFF is the "not-a-node"
                                     * sentinel, e.g. drpc_my_node==0xFF),
                                     * so the 8-bit HARD ceiling is 255.
                                     * Raised 8 -> 32 (regions, regions.md
                                     * §1.2) -> 64 (G23: UMP "every install
                                     * = a node"; the flat 32-cap contradicted
                                     * an open fleet). 64 doubles capacity
                                     * while keeping the per-node static
                                     * tables (DNODE_MAX-sized arrays in
                                     * drpc/swim/moe/dkva/world/kdds/...) and
                                     * the pmesh BEACON (8+4*DNODE_MAX bytes)
                                     * comfortably bounded.
                                     * FOLLOW-UP: going past ~254 needs a
                                     * 16-bit node_id, which changes the wire
                                     * protocol (src/dst_node fields + obj_id
                                     * layout) — a separate wave; do NOT do it
                                     * by bumping this constant alone.
                                     * Mixed-DNODE_MAX binaries on one wire
                                     * stay compatible only where a count
                                     * field bounds reads (e.g. pmesh
                                     * entry_cnt); a full fleet should run a
                                     * single DNODE_MAX.                       */
#endif  /* DNODE_MAX */

/* ------------------------------------------------------------------ *
 * DREGION_MAX (R) — region capacity, an INDEPENDENT sizing literal.   *
 *                                                                     *
 * unbounded_n_design.md §2: "N is unbounded iff per-node cost is      *
 * independent of N." DNODE_MAX historically served DOUBLE duty — the  *
 * 8-bit wire id ceiling AND the sizing of the per-node tables (the    *
 * global-view assumption). This wave (U-0, first slice) SPLITS them:  *
 *                                                                     *
 *   DNODE_MAX   = the wire/id ceiling. The 255-node 8-bit cap is      *
 *                 STILL HERE — widening it is wire-v2 (U-2), DEFERRED. *
 *   DREGION_MAX = R, the region-local per-node SIZING constant, its   *
 *                 OWN literal — deliberately NOT                      *
 *                 `#define DREGION_MAX DNODE_MAX` (that alias is the   *
 *                 exact fake the audit rejected: it re-couples the two *
 *                 roles, so bumping DNODE_MAX regrows every R table).  *
 *                                                                     *
 * WHAT THIS SLICE ACTUALLY DECOUPLES FROM FLEET N — each proven by    *
 * the [unbounded-coupling] probe (tests/unbounded), which compiles    *
 * the REAL headers at DNODE_MAX ∈ {64,256,1024} and diffs the sizes:  *
 *   - the K-DDS topic/handle budget         (kdds.h: 6*R / 10*R),     *
 *   - the dkva boot topic pre-open          (dkva.c: ≤ 3*R),          *
 *   - the dkva coordinator-aggregation ORIGIN axis (dkva.c: a nodemap *
 *     of capacity R replaces cagg[DNODE_MAX] — O(N) → O(R)).          *
 * Widen DNODE_MAX 64→256→1024 and every one of those is byte-for-byte *
 * CONSTANT, while dnode_table[DNODE_MAX] (and the cagg MEMBER axis)   *
 * grow — the probe prints both columns side by side and RED-flags any *
 * R-column that moves.                                               *
 *                                                                     *
 * STILL FLEET-SIZED — stated honestly, NOT claimed bounded here:      *
 * dnode_table (drpc.c), the SWIM per-node arrays, the cagg MEMBER     *
 * (second) axis (node-id indexed, quorum_core reuse), and the         *
 * world/moe/pmesh tables. Converting those to the nodemap primitive   *
 * is the remainder of U-0 and the O(R·N)→O(R²) member-axis shrink is  *
 * U-3. This commit ships the ORIGIN-axis + topic-budget decouple, the *
 * primitive, and the falsifier — not the whole fleet-table migration. *
 *                                                                     *
 * R is set to 64 (== today's DNODE_MAX) so a ≤64-node fleet is exactly *
 * ONE region and is BEHAVIOR-IDENTICAL to the pre-wave build (every    *
 * peer admits; the nodemap never evicts). The design's smaller default *
 * (R=32, "one or two regions") is a wire-v2 tuning — a pure constant   *
 * change once N>R ships.                                               *
 *                                                                     *
 * OVERRIDABLE at compile time (-DDREGION_MAX=…) EXACTLY like DNODE_MAX *
 * above, so the [unbounded-*] probes can build the REAL headers with   *
 * R < N (e.g. R=32, wire=256) and prove the DKVA_CAGG origin axis is    *
 * sized by R not fleet N — NON-VACUOUSLY (at the shipping R==N==64 the  *
 * equality slot[R]==slot[N] cannot tell them apart; cross-audit #3).    *
 * The shipping default is UNCHANGED (64), so bare-metal .text does not   *
 * move (the #ifndef is a no-op in every normal build; crown-neutral).   */
#ifndef DREGION_MAX
#define DREGION_MAX     64
#endif

/* ------------------------------------------------------------------ */
/* Packet header                                                        */
/* ------------------------------------------------------------------ */

#define DRPC_MAGIC      0x4B525064UL    /* "dRPK" LE                     */
#define DRPC_VERSION    1

/* type field values */
#define DRPC_HEARTBEAT  0x01
#define DRPC_REQ        0x10
#define DRPC_REPLY      0x11

/* call_id values */
#define DRPC_CALL_PING      0x0001
#define DRPC_CALL_CRE_TSK   0x0101    /* create task on remote node      */
#define DRPC_CALL_SIG_SEM   0x0201    /* signal remote semaphore         */
#define DRPC_CALL_INFER     0x0301    /* MLP inference on remote node    */
#define DRPC_CALL_FL_AGG    0x0401    /* FedAvg weight aggregation       */

typedef struct {
    UW  magic;          /* DRPC_MAGIC                        */
    UB  version;        /* DRPC_VERSION                      */
    UB  type;           /* DRPC_HEARTBEAT / REQ / REPLY      */
    UH  seq;            /* sequence number                   */
    UB  src_node;
    UB  dst_node;
    UH  call_id;        /* DRPC_CALL_*                       */
    UW  obj_id;         /* global object ID or func_id       */
    W   arg[3];         /* call arguments                    */
    W   result;         /* return value (REPLY only)         */
} __attribute__((packed)) DRPC_PKT;   /* 32 bytes */

/* ------------------------------------------------------------------ */
/* Global Object ID                                                    */
/* ------------------------------------------------------------------ */

#define GOBJ_MAKE(node, local)  \
    (((UW)(node) << 24) | ((UW)(local) & 0x00FFFFFFUL))
#define GOBJ_NODE(g)    ((UB)((UW)(g) >> 24))
#define GOBJ_LOCAL(g)   ((UW)(g) & 0x00FFFFFFUL)

/* ------------------------------------------------------------------ */
/* Node state machine                                                  */
/* ------------------------------------------------------------------ */

#define DNODE_UNKNOWN   0   /* never heard from                          */
#define DNODE_ALIVE     1   /* heartbeat received recently               */
#define DNODE_SUSPECT   2   /* missed SUSPECT_THRESH heartbeats          */
#define DNODE_DEAD      3   /* missed DEAD_THRESH more; pending cancelled */

/* ------------------------------------------------------------------ */
/* Node table                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    UB  node_id;
    UW  ip;
    UB  state;  /* DNODE_UNKNOWN / ALIVE / SUSPECT / DEAD               */
    UB  missed; /* consecutive missed heartbeat periods                  */
} DNODE;

extern DNODE dnode_table[DNODE_MAX];
extern UB    drpc_my_node;          /* 0xFF = not initialized            */

/* ------------------------------------------------------------------ */
/* Init & tasks                                                        */
/* ------------------------------------------------------------------ */

/* Call from usermain after rtl8139_init(), before netstack_start().
 * Sets net_my_ip and registers UDP port. */
void drpc_init(UB my_node_id, UW my_ip);

/* T-Kernel heartbeat task (pri 5) */
void drpc_task(INT stacd, void *exinf);

/* UDP receive callback — registered on DRPC_PORT */
void drpc_rx(UW src_ip, UH src_port, const UB *data, UH len);

/* Print node table to serial */
void drpc_nodes_list(void);

/* ------------------------------------------------------------------ */
/* Distributed T-Kernel API                                            */
/* ------------------------------------------------------------------ */

/* Create a task (local or remote).
 * func_id: 0x0001=hello  0x0002=counter  (see drpc.c rfunc_table)
 * Returns local task ID (local) or remote task ID (remote), or negative error. */
W  dtk_cre_tsk(UB node_id, UH func_id, INT pri);

/* Create semaphore on this node; returns global semaphore ID. */
UW dtk_cre_sem(INT isemcnt);

/* Wait on semaphore (semaphore must be on local node). */
ER dtk_wai_sem(UW gsemid, INT cnt, TMO tmout);

/* Signal semaphore — routes over the network if on remote node. */
ER dtk_sig_sem(UW gsemid, INT cnt);

/* Run MLP inference on a remote node (or local if node_id==drpc_my_node).
 * sensor_packed = SENSOR_PACK(temp_q8, hum_q8, press_q8, light_q8)
 * Returns E_OK and sets *class_out to 0/1/2. */
ER dtk_infer(UB node_id, W sensor_packed, UB *class_out, TMO tmout);

/*
 * heal.c から呼ぶ公開ラッパー。
 * DEAD ノードの代わりにローカルで rfunc タスクを起動する。
 */
W drpc_local_restart(UH func_id, INT pri, UB caller_node);

/* SEC-OOB-DRPC cert (external audit 2026-06-13): drives the real drpc_call /
 * dtk_cre_tsk / dtk_sig_sem / dtk_infer entry points with a node id >=
 * DNODE_MAX and confirms each is rejected (no out-of-bounds dnode_table
 * access), and that a legitimate in-range id still flows past the guard.
 * Emits "[drpc-oob] PASS"/"FAIL ...". Returns 0 on PASS, else the fail count. */
INT drpc_oob_self_test(void (*emit)(const char *));
