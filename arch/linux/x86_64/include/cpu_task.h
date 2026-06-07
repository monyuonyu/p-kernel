/*
 *  arch/linux/x86_64/include/cpu_task.h
 *  Task context setup for x86_64-linux.
 *
 *  Dormant task stack layout (SSP = lowest address, grows up):
 *
 *    ssp+ 0: r12        ← stacd (overwritten by setup_stacd)
 *    ssp+ 8: r13        ← exinf (overwritten by setup_stacd)
 *    ssp+16: r14 = 0
 *    ssp+24: r15 = 0
 *    ssp+32: rbx = 0
 *    ssp+40: rbp = 0
 *    ssp+48: taskmode = 0
 *    ssp+56: rip = &knl_task_entry_trampoline   ← popped by `ret`
 *   ─── Total: 64 bytes (4 × 16, 16-byte aligned) ───
 *
 *  Frame size is smaller than aarch64's 112 because System V AMD64 has
 *  fewer callee-saved GPRs (6 vs 12). The "return address" slot at
 *  offset 56 takes the role of aarch64's x30 — `ret` in the dispatcher
 *  pops it and jumps. For a fresh task that's the trampoline.
 *
 *  ABI alignment: stack_top is 16-aligned by knl_Imalloc + the round-
 *  down in this header. ssp = stack_top - 64, also 16-aligned. The
 *  dispatcher restores regs, does `add $56, %rsp; ret`; after `ret`
 *  rsp == stack_top, which is exactly the 16-aligned position the
 *  trampoline (and the entry function it calls) need before their
 *  next `call` instruction.
 */

#ifndef _CPU_TASK_
#define _CPU_TASK_

#include "cpu_insn.h"

/* Frame size written by knl_setup_context */
#define DORMANT_STACK_SIZE  64

IMPORT void knl_task_entry_trampoline(void);

Inline void knl_setup_context(TCB *tcb)
{
    /* Round stack top down to a 16-byte boundary. knl_Imalloc only
     * guarantees 8-byte alignment, so the naïve (isstack - 64) can
     * land at an 8-mod-16 address. System V AMD64 requires 16-byte
     * stack alignment immediately before any `call`, so any task
     * that ever calls a function with XMM args would crash without
     * this round-down. (Even tasks that don't touch XMM benefit from
     * a sane sp.) */
    unsigned long base = ((unsigned long)tcb->isstack) & ~0xFUL;
    UW *ssp = (UW *)(base - DORMANT_STACK_SIZE);

    /* Zero the entire frame */
    for (INT i = 0; i < DORMANT_STACK_SIZE / (INT)sizeof(UW); i++) {
        ssp[i] = 0;
    }

    /* "Return address" slot (offset 56, index 7 in 8-byte units) →
     * trampoline. The dispatcher's `ret` pops this on first dispatch. */
    ((void **)ssp)[7] = (void *)knl_task_entry_trampoline;

    tcb->tskctxb.ssp = ssp;
}

Inline void knl_setup_stacd(TCB *tcb, INT stacd)
{
    /* r12 slot (index 0) = stacd, r13 slot (index 1) = exinf.
     * The trampoline reads these into rdi/rsi before calling task(). */
    void **ssp = (void **)tcb->tskctxb.ssp;
    ssp[0] = (void *)(UW)stacd;
    ssp[1] = tcb->exinf;
}

Inline void knl_cleanup_context(TCB *tcb)
{
    (void)tcb;
}

#endif /* _CPU_TASK_ */
