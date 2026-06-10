/*
 *  ushim.c — ring3-core Wave C: the user-side shim for core_mind.elf
 *  (docs/architecture/ring3-core.md III.1.3 — the EXHAUSTIVE table)
 *
 *  arch/common/moe.c and arch/common/dtr.c are dual-compiled WHOLE-FILE
 *  into the user ELF (--gc-sections cuts the link to what _start
 *  reaches).  The live sections still reference a handful of symbols
 *  the kernel normally provides; this file is that surface — an
 *  INTERFACE layer like plibc.h, containing ZERO math.
 *
 *  Anti-fork rule (III.1.1, build invariant): no file under samples/
 *  or userland/ may contain a transformer, a softmax, or a gate.  The
 *  shim may only: set data symbols, return constants, or issue int 0x80.
 *
 *  Disposition key (III.1.3):
 *    SYSCALL      — forwards to int 0x80
 *    STUB-DEAD    — satisfies the LINKER only; unreachable at runtime
 *                   behind the drpc_my_node==0xFF guard (moe.c
 *                   select_expert takes the "reflex local-only" early
 *                   return BEFORE touching any of these)
 *    STUB-LIVE    — executed, must be semantics-preserving
 */

#include "drpc.h"       /* DNODE, DNODE_MAX, drpc_my_node, dtk_infer  */
#include "swim.h"       /* swim_rtt_ms                                */
#include "world.h"      /* world_note_firing, world_peer_pressure/threat */
#include "region.h"     /* region_recompute, region_is_member         */
#include "reflex.h"     /* reflex_on_inference, reflex_threat_level   */
#include "retrieval.h"  /* ret_set, ret_blend                         */
#include "p_syscall.h"  /* SYS_WRITE, SYS_MIND_NOTE                   */

/* ---- the one mechanism this file is allowed: int 0x80 ------------- */
static inline int ushim_sc(int nr, int a0, int a1, int a2)
{
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "b"(a0), "c"(a1), "d"(a2)
                     : "memory");
    return ret;
}

/* ---- SYSCALL: console — all [moe] prints go to serial via ring-0 -- */
void sio_send_frame(const UB *buf, INT size)
{
    ushim_sc(SYS_WRITE, 1, (int)buf, size);
}

/* ---- data: THE guard --------------------------------------------- */
/* 0xFF = single-node identity.  select_expert (moe.c) takes the
 * "reflex local-only (no node id)" early return before reading any of
 * the STUB-DEAD symbols below; routing degenerates to local (honest
 * bound III.5).                                                       */
UB drpc_my_node = 0xFF;

/* ---- STUB-DEAD: kernel-state inputs of select_expert -------------- */
DNODE dnode_table[DNODE_MAX];                 /* zero-init: no peers   */

UW swim_rtt_ms(UB node)
{
    (void)node;
    return 0xFFFFFFFFu;                       /* "unreachable"         */
}

INT world_peer_pressure(UB node) { (void)node; return -1; }
INT world_peer_threat(UB node)   { (void)node; return -1; }

void region_recompute(void) { }
BOOL region_is_member(UB node) { (void)node; return FALSE; }

UB reflex_threat_level(void) { return 0; }

/* STUB-DEAD: remote delegation is a later wave (CDN-3 transport).
 * Never executed (local-only early return => expert == me); returns
 * != E_OK so even a stray call falls back to the local class.         */
ER dtk_infer(UB node_id, W sensor_packed, UB *class_out, TMO tmout)
{
    (void)node_id; (void)sensor_packed; (void)class_out; (void)tmout;
    return E_NOSPT;
}

/* ---- SYSCALL-BACK (CDN-7): ring-3 inferences stay visible --------- */
/* world map firing indicator — 可視化 is a mechanism, not decoration. */
void world_note_firing(UB gate_class)
{
    ushim_sc(SYS_MIND_NOTE, 0, (int)gate_class, 0);
}

/* the GUARD hook: the reflex ACTION table stays ring-0; only the hook
 * crosses the boundary.  src_node is dropped — the kernel side passes
 * its own (real) drpc_my_node.                                        */
void reflex_on_inference(UB threat_class, UB confidence, UB src_node)
{
    (void)src_node;
    ushim_sc(SYS_MIND_NOTE, 1, (int)threat_class | ((int)confidence << 8), 0);
}

/* ---- STUB-LIVE: retrieval forced OFF ------------------------------ */
/* dtr_forward_probs forces retrieval OFF around the forward
 * (ret_set(0) ... ret_set(rprev), dtr.c) — so a no-op here computes
 * the IDENTICAL function.  A future ring-3 retrieval goes through
 * SYS_READ, never a re-implementation (III.1.1).                      */
UB ret_set(UB on) { (void)on; return 0; }

INT ret_blend(const B input[DTR_SEQ_LEN], float logits[DTR_OUT_DIM])
{
    (void)input; (void)logits;
    return 0;                                 /* no votes added        */
}
