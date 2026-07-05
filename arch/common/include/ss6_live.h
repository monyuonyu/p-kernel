/*
 *  ss6_live.h — SS-6 LIVE remote-expert transport (wave-ss6-live).
 *
 *  SS-6 (arch/common/llm/student.c) proved, IN-PROCESS and BYTE-IDENTICAL, that
 *  a MoE forward whose WIDE experts (chosen-slot j >= K_min) are computed by a
 *  STUB "peer" equals the single-node forward bit-for-bit, with a hard timeout
 *  -> LOCAL fallback. The transport for the peer call was a caller-installed
 *  st_set_remote_expert hook; the in-process cert drove it with a stub.
 *
 *  THIS module cashes the DEFERRED [live] row: it installs a REAL transport that
 *  ships the [D] f_in vector to the SS-5 placement owner over the mesh (a
 *  dedicated UDP request/reply on SS6L_PORT, mirroring drpc.c's pending-table +
 *  semaphore + hard timeout), the owner computes the EXACT st_expert_forward_ref
 *  and replies the [D] output, and student.c sums it in the SAME canonical order.
 *  A killed/absent owner times out -> student.c recomputes that expert LOCALLY
 *  (honest degraded), exactly the SS-6 contract carried over the wire.
 *
 *  WHY a private UDP port, not KDDS: a full L-tier expert vector is [D]=256
 *  floats = 1 KB > KDDS_DATA_MAX(192). DRPC's UDP REQ/REPLY already carries an
 *  arbitrary packet with a pending/semaphore/timeout machine; we mirror it
 *  (NOT reuse drpc_call, whose payload is 3 scalar args). rw[]/gl_merge/kv_step
 *  are UNTOUCHED.
 *
 *  SCOPE (honest): this drives the SS-6 forward path (st_forward) only — the KV
 *  incremental kv_step generation path is NOT wired here (a documented SS-6
 *  follow-up; run_kv.sh stays byte-identical). So "live cross-node forward" is
 *  NOT "live chat is distributed".
 */
#pragma once
#include "kernel.h"

/* Dedicated UDP port for the SS-6 live remote-expert REQ/REPLY (between
 * DRPC 7374 and KDDS 7376; distinct from both). */
#define SS6L_PORT       7378

/* SS6L v2 (DMOE-A, distributed_moe_design.md §6.3): the fire packet grows the
 * version pin (ver_lo/ver_hi), the core-epoch, and a flags byte; the reply gains
 * a refuse_reason. The magic BUMPS to a v2 value so a v1 node cleanly IGNORES a
 * v2 datagram (a mixed-fleet boot never mis-parses). v1 magics kept for the
 * record. */
#define SS6L_REQ_MAGIC_V1 0x4C365353UL  /* "SS6L" LE — v1 request (SS-6)         */
#define SS6L_REP_MAGIC_V1 0x52365353UL  /* "SS6R" LE — v1 reply                  */
#define SS6L_REQ_MAGIC   0x32365353UL   /* "SS62" LE — v2 request (DMOE)         */
#define SS6L_REP_MAGIC   0x72365353UL   /* "SS6r" LE — v2 reply                  */

/* fire-packet flags. BANK marks a request for a DMOE bank expert (ver-pinned,
 * §2.3); absent (0) is a plain SS-6 floor expert (ver fields ignored). */
#define SS6L_FLAG_BANK   0x01u

/* reply refuse_reason codes (0 == OK). */
#define SS6L_REFUSE_NONE     0
#define SS6L_REFUSE_ABSENT   1   /* not resident here                            */
#define SS6L_REFUSE_VERSKEW  2   /* the requester's pin != our blob's ver (§2.3) */

/* Install the LIVE remote-expert transport into student.c (st_set_remote_expert)
 * and bind SS6L_PORT. `m` is the resident model this node will run remote
 * experts against AS A RESPONDER (every node must hold a byte-identical model
 * for the remote [D] output to match the single-node forward). Idempotent: a
 * second call updates the model pointer. The gate is fail-closed: it fires a
 * remote expert ONLY when (env PKERNEL_REMOTE_EXPERTS=1) AND the slot is wide
 * (j >= K_min) AND SS-5 placement says a PEER owns it AND degrade==FULL AND
 * region_size()>=2 — so a default single node is byte-unchanged (hook never
 * fires) and training stays local (SS-6's st_backward guard fail-closes). */
void ss6_live_install(void *m);  /* m: st_model* (opaque here to keep this header kernel-tier-clean) */

/* Clear the hook + unbind (back to the single-node path). */
void ss6_live_uninstall(void);

/* Opt-in toggle (env PKERNEL_REMOTE_EXPERTS=1). Read on the host tier where
 * getenv is safe; OFF by default -> single-node byte-unchanged. */
void ss6_live_set_enabled(int on);

/* UDP receive callback (registered on SS6L_PORT by ss6_live_install). Public so
 * the bind site / a self-test can reference it; NOT for direct calls. */
void ss6_live_rx(UW src_ip, UH src_port, const UB *data, UH len);

/* Observability (read-only): remote requests this node SENT as a requester and
 * SERVED as a responder this boot. Deterministic given (weights, bytes, fleet). */
unsigned ss6_live_req_sent(void);
unsigned ss6_live_req_served(void);
