/*
 *  supernode.h — N-2c supernode packet forwarding (p2p-overlay.md "Supernodes").
 *
 *  N-2 shipped the deterministic SELECTOR (region_supernode() — the lowest-id
 *  region member that is supernode-capable; 0xFF = degrade to the central relay);
 *  N-2b shipped the SWIM capability-bit gossip that makes that selector
 *  fleet-real. NEITHER moved a single datagram — a supernode was elected but
 *  forwarded nothing.
 *
 *  THIS module cashes the next slice: the elected supernode actually FORWARDS
 *  region traffic. When a node wants to send a payload to a region PEER B and
 *  region_supernode() elects a capable node S (S != 0xFF, S != self, S != B,
 *  S reachable), the sender wraps the payload as a SNF_FWD packet addressed to S
 *  (over a dedicated UDP port, riding the SAME net_relay transport — NO new wire
 *  protocol; the supernode is just another node), S re-forwards it to B as a
 *  SNF_DELIVER, and B receives the payload BYTE-IDENTICAL. This is the first real
 *  step of routing THROUGH a supernode instead of unconditionally via ./relay.
 *
 *  FAIL-CLOSED / honest degrade (the Skype-style graceful path):
 *    - no supernode (region_supernode()==0xFF, i.e. nobody capable)  -> DIRECT
 *      (the sender SNF_DELIVERs straight to B == today's central-relay behavior);
 *    - the elected S is the SELF, or IS the destination B            -> DIRECT;
 *    - S unreachable / times out (no ACK within the bounded budget)  -> DIRECT
 *      back to the central relay, NO packet lost.
 *  So a default single node (no capable supernode) is byte-unchanged.
 *
 *  INVARIANTS: NOCENTRAL/deterministic (S = the existing min-id selector, no
 *  vote); the forwarded payload is BYTE-IDENTICAL end-to-end (one-mind: a
 *  forward must not corrupt the bytes); no VLA (fixed SNF_PAYLOAD_MAX scratch,
 *  all file-static — never on a task stack, per feedback_hosted_relay_stack_
 *  overflow); region.c / swim.c / the mind math are UNTOUCHED.
 *
 *  SCOPE (honest): this is the FORWARDING PLANE + its cert. NAT hole-punch (N-3)
 *  and seed bootstrap (N-4) are DEFERRED. The in-process cert drives the REAL
 *  route-decision + wrap/unwrap + forwarded counter with a stub transport; the
 *  true multi-process supernode-forward is a [live] row (run_supernode_fwd.sh).
 */
#pragma once
#include "kernel.h"

/* Dedicated UDP port for supernode forward REQ/DELIVER. MUST be distinct from
 * EVERY other udp_bind() port — netstack's udp_input dispatches a datagram to
 * the FIRST socket whose port matches and returns, so a port shared with an
 * earlier-bound handler silently steals all our traffic (that handler never
 * sees a second binder on the same port). The N-2c live bug was exactly this:
 * SNF_PORT was 7380 == PMESH_PORT (bound at boot by pmesh_init, long before
 * snf_install), so pmesh_rx ate every SNF_FWD/SNF_DELIVER and snf_rx never
 * fired on the supernode/destination. 7377 is the free slot between
 * KDDS (7376) and SS6L (7378); also distinct from DRPC (7374), SWIM (7375),
 * REPLICA (7379), PMESH (7380), SFS (7381), PFSR/RAFT (7382), SPAWN (7383),
 * KLOAD (7386/7387). */
#define SNF_PORT        7377

#define SNF_FWD_MAGIC    0x57464E53UL   /* "SNFW" LE — forward-to-supernode    */
#define SNF_DLV_MAGIC    0x4C444E53UL   /* "SNDL" LE — deliver-to-destination  */
#define SNF_ACK_MAGIC    0x4B414E53UL   /* "SNAK" LE — supernode accepted/fwd  */

/* Max application payload we forward in one datagram. Comfortably inside a UDP
 * datagram on the mesh (the relay caps payload at 1380; header + this stays
 * under). Fixed -> no VLA. */
#define SNF_PAYLOAD_MAX  512

/* Install: bind SNF_PORT (once drpc_my_node is set) so this node can act as a
 * supernode (receive SNF_FWD, re-forward as SNF_DELIVER) AND as a destination
 * (receive SNF_DELIVER). Idempotent. On a standalone node (drpc_my_node==0xFF)
 * the bind is skipped and snf_send() degrades to the caller's direct path. */
void snf_install(void);

/* Send `len` bytes of `payload` to region peer `dst` (1..DNODE_MAX-1), routing
 * THROUGH the elected supernode when one exists, else DIRECT (degrade). Returns
 * 1 if a delivery datagram was emitted (forwarded OR direct), 0 on a hard error
 * (bad args / port not bound). The byte-identical payload reaches `dst`'s
 * SNF_PORT handler -> snf_set_sink() callback. */
INT snf_send(UB dst, const UB *payload, UH len);

/* The destination-side delivery sink: invoked on the node that owns `dst` when a
 * SNF_DELIVER for it arrives (whether forwarded by a supernode or sent direct).
 * `via_super` = 1 if a supernode forwarded it, 0 if it came direct. Lets a cert /
 * an application observe the byte-identical received payload. */
typedef void (*snf_sink_fn)(UB src, const UB *payload, UH len, INT via_super);
void snf_set_sink(snf_sink_fn fn);

/* UDP receive callback (registered on SNF_PORT by snf_install). Public so the
 * bind site / a self-test can reference it; NOT for direct calls. */
void snf_rx(UW src_ip, UH src_port, const UB *data, UH len);

/* Observability (read-only), per boot:
 *   snf_forwarded() : datagrams this node RE-FORWARDED as a supernode (S's count)
 *   snf_via_super() : datagrams this node SENT that went THROUGH a supernode
 *   snf_direct()    : datagrams this node SENT DIRECT (degrade / no supernode)
 *   snf_delivered() : SNF_DELIVERs this node RECEIVED as a destination          */
unsigned snf_forwarded(void);
unsigned snf_via_super(void);
unsigned snf_direct(void);
unsigned snf_delivered(void);

/* Host cert (shell `super test` / `region fwd`): drives the REAL route-decision
 * (snf_route_target), wrap/unwrap, and forwarded counter against synthetic
 * converged views — deterministic, arch-uniform, NO live UDP. Proves:
 *   [supernode-forward]  A->B via elected S: S.forwarded>0, B's bytes identical;
 *   falsifier (a) no supernode -> DIRECT (S forwards 0, B still gets the bytes);
 *   falsifier (b) S unreachable -> fail-closed DIRECT, no packet lost.
 * Prints PASS/FAIL lines; the caller's `print` is used for output. */
void supernode_forward_self_test(void (*print)(const char *));

/* LIVE driver (shell `snf sink|send <dst>|recv|stat`): drives the REAL UDP
 * wire over the mesh so a multi-process run can prove A->B forwarded BY the
 * elected supernode S (S.forwarded>0, B byte-identical) — the [live] arm.
 * See samples/11_distributed/run_supernode_fwd.sh. */
void snf_cmd(const UB *args, UW len, void (*print)(const char *));
