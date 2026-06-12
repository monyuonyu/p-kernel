/*
 *  galaxy.h — 銀河の観測窓 v1 (docs/architecture/galaxy.md)
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
/* Event ring (§4) — fixed-width only (the world.h LP64 rule)          */
/* ------------------------------------------------------------------ */

typedef struct {
    U4 ms;          /* uptime ms (wraps ~49 days; the page handles wrap) */
    U1 type;        /* EV_* below                                        */
    U1 src, dst;    /* node ids; 0xFF = none                             */
    U1 _pad;
    U2 a, b;        /* type-specific detail (class, topic hash16, len...) */
} GALAXY_EV;        /* 12 bytes */

_Static_assert(sizeof(GALAXY_EV) == 12, "GALAXY_EV must be 12 bytes (LP64-safe)");

#define GALAXY_RING         256   /* 3 KB static; power of two            */
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
                             /* b = peers folded. ONE site: mw_fold_region     */
                             /* (living-mind.md XI.7) — the region's stars     */
                             /* pulse in unison (collective sleep).            */

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

