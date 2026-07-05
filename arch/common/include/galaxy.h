/*
 *  galaxy.h — 銀河の観測窓 v1 (docs/architecture/30-module/galaxy.md)
 *
 *  Per-node observation window. Each hosted node serves its OWN view of
 *  the galaxy (its gossip-bounded world) over a loopback HTTP/1.0 server.
 *  Three non-negotiables (galaxy.md §intro): honesty (every photon is a
 *  REAL event), decentralization (no galaxy server; each node serves its
 *  own world.c-bounded view), and "observation must never destabilize the
 *  organism" (O(1) hooks, non-blocking I/O, overflow drops + reports).
 *
 *  This is a FACE on existing organs (§9): peer stars come from
 *  dnode_table[] + world_peer_* accessors; teach/ask drive the LM-6
 *  production mouth (mind_cmd) — galaxy.c contains NO second node table,
 *  NO second mind path, NO second HTTP/JSON library (the auditor greps).
 *
 *  v1 is hosted-only (§1): the kernel-side code is written against the
 *  5-function galaxy_io_* transport ABI (galaxy_posix.c) so the future
 *  netstack-tcp-server slice slots bare-metal in without touching this.
 */
#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* Event vocabulary (§4) — the EV_* contract shared by the emit side    */
/* (dmn/swim/moe/drpc via galaxy_emit) and the read side (ui_api.c +     */
/* galaxy.c). The 12-byte event STRUCT and the ring itself now live in   */
/* the bounded UI layer (ui_api.h: UI_EVENT / UI_EVENT_RING); galaxy.h   */
/* keeps only the wire-stable type/sentinel constants both sides need.   */
/* ------------------------------------------------------------------ */

#define GALAXY_NODE_NONE    0xFF

/* Event types (§4.2). Precious = never sampled; chatty = token-bucket.  */
#define EV_SWIM         1    /* a (old state) b (new state); src = node    */
#define EV_DMN_WAKE     2    /* ACTIVE again                               */
#define EV_DMN_IDLE     3    /* dreaming (dmn_state == DMN_IDLE)           */
#define EV_CONSOLIDATE  4    /* a fact sank into rw[] (a = round kind)      */
#define EV_TEACH        5    /* a = key, b = value                          */
#define EV_ASK          6    /* a = key, b = pred                           */
#define EV_DRPC_IN      7    /* src asked ME (a = call_id, b = class)        */
#define EV_DRPC_OUT     8    /* I asked dst (a = call_id)                    */
#define EV_MOE          9    /* my star fires toward dst (a = gate, b = cls) */
#define EV_DKVA         10   /* distributed attention (a = phase, b = n)     */
#define EV_KDDS         11   /* gossip publish (a = topic16, b = len) SAMPLED */
#define EV_PMESH_TX     12   /* directed mesh send (SAMPLED)                 */
#define EV_PMESH_RX     13   /* directed mesh recv (SAMPLED)                 */
#define EV_SUMMARY      14   /* a = the suppressed EV_* type, b = count      */
#define EV_REMOTE_TEACH 15   /* LM-7: a fact taught on node A landed in MY    */
                             /* queue. src = A (origin), dst = me; a = key,   */
                             /* b = val. ONE site: mind_net_task arrival       */
                             /* (living-mind.md VIII.10).                      */
#define EV_MERGE        16   /* LM-10 Path W: I folded my region into rw[].    */
                             /* src = me, dst = NONE; a = merge_epoch16,       */
                             /* b = peers folded (low 15 bits) | the LM-11     */
                             /* weighted flag (bit 15). ONE site: mw_fold_     */
                             /* region (living-mind.md XI.7 / XII.7) — the     */
                             /* region's stars pulse in unison (collective     */
                             /* sleep), now with a quality bit: a Fisher-      */
                             /* weighted fold (Path W²) vs a plain fold.       */
/* LM-11 Path W² (living-mind XII.7): one-bit annotation in EV_MERGE's b
 * payload — no new event type. A weighted (Fisher) fold sets it; a plain
 * fold clears it, so the galaxy view can later render "the region slept
 * with Fisher-weighting" vs a plain pulse. Peer count uses the low 15
 * bits (<=DNODE_MAX, fits). */
#define EV_MERGE_WEIGHTED  0x8000u   /* bit 15 of b = a Fisher-weighted fold */
#define EV_STATE        17   /* survival-loop L0 (survival-loop.md §6-L0): my    */
                             /* node STATE changed. src = me, dst = NONE;        */
                             /* a = old WSTATE_*, b = new WSTATE_*. ONE site:     */
                             /* world_self_state_step on a committed transition.  */
                             /* Hosted-only (galaxy.c is hosted-only).            */
#define EV_REVISE       18   /* LM-12 (living-mind-lm12-belief-revision.md): a    */
                             /* weight-resident belief was REVISED in place — the */
                             /* old value DISPLACED, the new one now owed a DMN    */
                             /* sleep. a = key, b = (v_old<<8)|v_new. LOCAL revise */
                             /* (Site 1): src = me, dst = NONE. REMOTE revise      */
                             /* (Site 2): src = origin (the reviser), dst = me.    */
#define EV_FORGET       19   /* LM-13 (living-mind-lm13-forgetting.md): a fact    */
                             /* was EVICTED from the bounded R3 queue by the       */
                             /* min-EARNED-salience selector (the LM-5 FIFO's      */
                             /* successor) — chosen forgetting, its weight trace   */
                             /* may now decay. src = me, dst = NONE; a = evicted    */
                             /* fact seq, b = its salience (1 = default/unearned,   */
                             /* >1 = earned but still the least-salient). ONE site: */
                             /* the r3_fact_learn eviction branch.                  */
#define EV_WONDER       20   /* LM-14 (living-mind-lm14-curiosity.md): the mind    */
                             /* was ASKED about a key it does NOT hold and does     */
                             /* NOT know (unbound AND masked share < M_KNOWN_SHARE) */
                             /* — an accrued WANT, the DUAL of EV_FORGET's chosen   */
                             /* forgetting. src = me, dst = NONE; a = wanted key,   */
                             /* b = accrued want (1..R3_WANT_CAP). ONE site: the    */
                             /* r3_want_note accrual (m_ask MISS branch). LOCAL —   */
                             /* want never crosses the wire.                        */
#define EV_REFUSE       21   /* 良心 (conscience): a harmful request was REFUSED at  */
                             /* a mouth. src = me, dst = NONE; a = CONS_SITE_* (which*/
                             /* mouth), b = harm class (0xFF = FAILSAFE/floor        */
                             /* unverifiable). NEVER carries the withheld content —  */
                             /* the refusal replaces EV_ASK(k,pred), it does not add  */
                             /* to it (conscience.md §1.1/§1.3). ONE site:            */
                             /* conscience_on_refuse.                                */

/* ------------------------------------------------------------------ */
/* Publics (§9 — the complete flagged list for galaxy.c)               */
/* ------------------------------------------------------------------ */

/* first instruction of every hook; ==0 means the galaxy is off and the
 * hook is a single predictable branch (PKERNEL_GALAXY=0). */
extern volatile UB galaxy_on;

/* called once from the hosted usermain before galaxy_task is created. */
void galaxy_init(void);

/* the resident server task (priority 8). Accept -> route -> SSE fan-out
 * -> tk_dly_tsk(50). Created in both hosted usermains. */
void galaxy_task(INT stacd, void *exinf);

/* the ONE hook call (§2/§4): O(1), no allocation, no semaphore.
 * Hosted builds (-D_TK_HOSTED_LIBC_) link the real ring writer in
 * galaxy.c. Bare-metal targets do NOT compile galaxy.c (galaxy.md §3:
 * v1 is hosted-only — netstack TCP is client-only), so the shared TUs
 * (dmn.c/swim.c/moe.c/drpc.c/r3_incontext.c) get a no-op inline here:
 * the hook costs nothing and the kernels keep linking. The future
 * netstack-tcp-server slice replaces this branch, not the call sites. */
#ifdef _TK_HOSTED_LIBC_
void galaxy_emit(UB type, UB src, UB dst, UH a, UH b);
#else
static inline void galaxy_emit(UB type, UB src, UB dst, UH a, UH b)
{
    (void)type; (void)src; (void)dst; (void)a; (void)b;
}
#endif

/* lapped-consumer / overflow counter, surfaced by /galaxy.json. */
UW   galaxy_dropped(void);

/* ------------------------------------------------------------------ */
/* Transport ABI (§3.2) — implemented by galaxy_posix.c (C ABI; NO     */
/* T-Kernel types). The future netstack-tcp-server slice re-implements  */
/* these over netstack TCP and bare-metal joins without touching        */
/* galaxy.c.                                                            */
/* ------------------------------------------------------------------ */

#define GALAXY_MAX_CLIENTS  4     /* one page + one SSE + slack           */

