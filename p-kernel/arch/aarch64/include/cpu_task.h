/*
 *  cpu_task.h (aarch64)
 *  Task context setup for AArch64
 *
 *  Dormant task stack layout (SSP = lowest address, grows up):
 *
 *    ssp+  0: x19  ← stacd (overwritten by setup_stacd)
 *    ssp+  8: x20  ← exinf (overwritten by setup_stacd)
 *    ssp+ 16: x21 = 0
 *    ssp+ 24: x22 = 0
 *    ssp+ 32: x23 = 0
 *    ssp+ 40: x24 = 0
 *    ssp+ 48: x25 = 0
 *    ssp+ 56: x26 = 0
 *    ssp+ 64: x27 = 0
 *    ssp+ 72: x28 = 0
 *    ssp+ 80: x29 (fp) = 0
 *    ssp+ 88: x30 (lr) = &knl_task_entry_trampoline
 *    ssp+ 96: taskmode = 0
 *    ssp+104: (padding)
 *   ─── Total: 112 bytes (7 × 16, 16-byte aligned) ───
 *
 *  knl_dispatch saves/restores this 112-byte frame and does `ret` to x30.
 *  For a new task, x30 → trampoline, which reads stacd from x19, exinf
 *  from x20, then calls task(stacd, exinf).
 */

#ifndef _CPU_TASK_
#define _CPU_TASK_

#include "cpu_insn.h"

/* Frame size written by setup_context */
#define DORMANT_STACK_SIZE  112

IMPORT void knl_task_entry_trampoline(void);

Inline void knl_setup_context(TCB *tcb)
{
    /* AArch64 requires sp to be 16-byte aligned for every load/store
     * that uses sp as the base register. knl_Imalloc only guarantees
     * 8-byte alignment for the task stack, so the naïve
     * (isstack - 112) can land 8-aligned but not 16-aligned. Round
     * the top of stack down to 16 here. Bare-metal builds get the
     * same guarantee for free; AArch64-Linux ones rely on it. */
    unsigned long base = ((unsigned long)tcb->isstack) & ~0xFUL;
    UW *ssp = (UW *)(base - DORMANT_STACK_SIZE);

    /* Zero the entire frame */
    for (INT i = 0; i < DORMANT_STACK_SIZE / (INT)sizeof(UW); i++) {
        ssp[i] = 0;
    }

    /* x30 slot (offset 88, = index 11 in 8-byte units) → trampoline */
    ((void **)ssp)[11] = (void *)knl_task_entry_trampoline;

    tcb->tskctxb.ssp = ssp;
}

Inline void knl_setup_stacd(TCB *tcb, INT stacd)
{
    /* x19 slot (index 0) = stacd, x20 slot (index 1) = exinf */
    void **ssp = (void **)tcb->tskctxb.ssp;
    ssp[0] = (void *)(UW)stacd;
    ssp[1] = tcb->exinf;
}

Inline void knl_cleanup_context(TCB *tcb)
{
    (void)tcb;
}

#endif /* _CPU_TASK_ */
