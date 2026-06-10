/*
 *  fpu.c (x86)
 *  Per-task x87/MMX/SSE context save — eager FXSAVE/FXRSTOR
 *
 *  Debt wave (RING3-C follow-up, ring3-core.md III.5 honest bound):
 *  the dispatcher (cpu_support.S) switched ONLY the callee-saved
 *  integer registers; the port had ZERO fxsave/fnsave/CR0.TS
 *  handling, so two tasks computing floats concurrently corrupted
 *  each other's x87 state silently (gate: `fpu test` — the unfixed
 *  kernel fails it deterministically).
 *
 *  Design: EAGER, unconditional fxsave/fxrstor at every dispatch.
 *    - Chosen over lazy CR0.TS + #NM because it is far simpler (no
 *      new exception path through the ring0/ring3-aware handler) and
 *      the cost (~tens of ns per switch on anything modern; TCG is
 *      I/O-bound anyway) is irrelevant at this scale.
 *    - The 512-byte FXSAVE areas live in a SIDE TABLE indexed by
 *      tskid, NOT in the TCB: TCB field offsets are baked into
 *      cpu_support.S/offset.h asm (the AArch64 TCB-offset trap), so
 *      the TCB layout is deliberately untouched.
 *      Cost: (CFN_MAX_TSKID+1) x 512 B = ~66 KB of BSS.
 *    - FXSAVE covers x87 + MMX + SSE/XMM + MXCSR in one shot
 *      (CR4.OSFXSR is set in fpu_init()).  AVX/xsave state is NOT
 *      covered — nothing in the port emits AVX (-m32, no -mavx).
 *
 *  First-run semantics: a task with no saved image (or one reset via
 *  fpu_task_reset) is restored from a clean fninit template, so a
 *  fresh/reused tid never inherits a dead task's registers.
 *
 *  The hooks are called from cpu_support.S with interrupts disabled,
 *  on the outgoing task's stack (save) / incoming task's stack
 *  (restore).  Until fpu_init() runs they are no-ops — exactly the
 *  pre-fix behavior — which keeps the early-boot dispatches (before
 *  usermain) byte-identical.
 */

#include "kernel.h"
#include "task.h"
#include <tmonitor.h>

#define FPU_SLOTS  (CFN_MAX_TSKID + 1)   /* tskid 1..CFN_MAX_TSKID */

static UB fpu_area[FPU_SLOTS][512] __attribute__((aligned(16)));
static UB fpu_valid[FPU_SLOTS];
static UB fpu_init_image[512]      __attribute__((aligned(16)));
static volatile UB fpu_ready;            /* hooks armed?            */

void fpu_init(void)
{
    UW r;

    /* CR4.OSFXSR (bit 9): FXSAVE/FXRSTOR manage XMM+MXCSR too, and
     * SSE instructions are legal should anything emit them.        */
    asm volatile("movl %%cr4, %0" : "=r"(r));
    r |= (1UL << 9);
    asm volatile("movl %0, %%cr4" :: "r"(r));

    /* CR0: EM=0 (bit 2, x87 executes natively), TS=0 (bit 3, no #NM
     * — eager switching never sets TS).                            */
    asm volatile("movl %%cr0, %0" : "=r"(r));
    r &= ~((1UL << 2) | (1UL << 3));
    asm volatile("movl %0, %%cr0" :: "r"(r));

    /* Clean template: masked exceptions, empty stack, MXCSR default */
    asm volatile("fninit");
    asm volatile("fxsave (%0)" :: "r"(fpu_init_image) : "memory");

    for (INT i = 0; i < FPU_SLOTS; i++) fpu_valid[i] = 0;
    fpu_ready = 1;

    tm_putstring((UB *)"[fpu] eager FXSAVE/FXRSTOR armed (512B x ");
    {
        char b[8]; INT i = 7; b[i] = '\0';
        UW v = FPU_SLOTS;
        while (v > 0 && i > 0) { b[--i] = (char)('0' + v % 10); v /= 10; }
        tm_putstring((UB *)&b[i]);
    }
    tm_putstring((UB *)" task slots)\r\n");
}

void fpu_task_reset(ID tid)
{
    if (tid >= 1 && tid < FPU_SLOTS) fpu_valid[tid] = 0;
}

/* ------------------------------------------------------------------ */
/* Dispatcher hooks — called ONLY from cpu_support.S (cli held)        */
/* ------------------------------------------------------------------ */

void knl_fpu_save_ctx(TCB *tcb)
{
    if (!fpu_ready || tcb == NULL) return;
    ID tid = tcb->tskid;
    if (tid < 1 || tid >= FPU_SLOTS) return;
    asm volatile("fxsave (%0)" :: "r"(fpu_area[tid]) : "memory");
    fpu_valid[tid] = 1;
}

void knl_fpu_restore_ctx(TCB *tcb)
{
    if (!fpu_ready || tcb == NULL) return;
    ID tid = tcb->tskid;
    if (tid < 1 || tid >= FPU_SLOTS) return;
    if (fpu_valid[tid])
        asm volatile("fxrstor (%0)" :: "r"(fpu_area[tid]) : "memory");
    else
        asm volatile("fxrstor (%0)" :: "r"(fpu_init_image) : "memory");
}
