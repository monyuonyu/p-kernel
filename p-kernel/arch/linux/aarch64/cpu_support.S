/*
 *  arch/linux/aarch64/cpu_support.S
 *  T-Kernel dispatcher / context switch for the Linux userspace port.
 *
 *  Adapted from arch/aarch64/cpu_support.S. The dormant task stack
 *  frame layout (112 bytes) and TCB::tskctxb.ssp contract are
 *  identical to the bare-metal port — that is the whole point: same
 *  T-Kernel internals, different arch backend.
 *
 *  Differences from the bare-metal sibling:
 *    - No EL1 privileged instructions (msr daifset/clr, eret, wfe).
 *      IRQ enable/disable becomes a write to arch_irq_disabled_flag,
 *      defined in preempt.c.
 *    - No EL1 exception vector table. Signals replace it; SIGALRM
 *      delivery is handled in preempt.c.
 *    - Idle path uses a syscall-free pause instead of wfe.
 *
 *  Dormant task stack frame (112 bytes, see cpu_task.h):
 *    [sp+  0]  x19  (= stacd for new task)
 *    [sp+  8]  x20  (= exinf for new task)
 *    [sp+ 16]  x21        [sp+ 56]  x26       [sp+ 96]  taskmode
 *    [sp+ 24]  x22        [sp+ 64]  x27       [sp+104]  padding
 *    [sp+ 32]  x23        [sp+ 72]  x28
 *    [sp+ 40]  x24        [sp+ 80]  x29 (fp)
 *    [sp+ 48]  x25        [sp+ 88]  x30 (lr)  ← trampoline / caller
 */

#define _in_asm_source_
#include "offset.h"

    .text
    .align 4

/* knl_taskmode is shared global — defined here (BSS via .data 0) so
 * that the linker resolves it without an arch-specific C file. */
    .data
    .global knl_taskmode
knl_taskmode:
    .quad   0
    .text

/* -----------------------------------------------------------------------
 *  Internal helpers — inline IRQ flag manipulation using x16/x17 which
 *  are designated "intra-procedure call scratch" registers in AAPCS64
 *  (caller-saved, not used for argument passing). Touching them does
 *  not perturb anything visible to T-Kernel C code.
 * --------------------------------------------------------------------- */

    .macro IRQ_DISABLE
    adrp x16, arch_irq_disabled_flag
    add  x16, x16, :lo12:arch_irq_disabled_flag
    mov  w17, #1
    str  w17, [x16]
    .endm

    .macro IRQ_FLAG_CLEAR
    /* Bare flag clear — does NOT drain pending. Use only when the
     * caller knows there are no pending signals to deliver, or when
     * lazy delivery is acceptable. The dispatcher uses this on its
     * exit path because draining would risk re-entering knl_dispatch
     * from a signal handler. */
    adrp x16, arch_irq_disabled_flag
    add  x16, x16, :lo12:arch_irq_disabled_flag
    str  wzr, [x16]
    .endm

/* -----------------------------------------------------------------------
 *  knl_dispatch_to_schedtsk / knl_force_dispatch
 *    Discard current context and switch to schedtsk.
 *    Used when the running task exits or self-terminates — no need
 *    to save its register state.
 * --------------------------------------------------------------------- */
    .global knl_dispatch_to_schedtsk
    .global knl_force_dispatch
knl_dispatch_to_schedtsk:
knl_force_dispatch:
    IRQ_DISABLE
    adrp x0, knl_ctxtsk
    add  x0, x0, :lo12:knl_ctxtsk
    str  xzr, [x0]              /* ctxtsk = NULL */
    b   .Ldispatch_loop

/* -----------------------------------------------------------------------
 *  knl_dispatch_entry / knl_dispatch
 *    Save current context onto its own stack, write SSP to its TCB,
 *    then pick schedtsk and switch to it.
 * --------------------------------------------------------------------- */
    .global knl_dispatch_entry
    .global knl_dispatch
knl_dispatch_entry:
knl_dispatch:
    IRQ_DISABLE

    /* Push the 112-byte dormant frame onto current sp. */
    sub sp, sp, #112
    stp x19, x20, [sp,  #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    adrp x0, knl_taskmode
    add  x0, x0, :lo12:knl_taskmode
    ldr  x0, [x0]
    str  x0,       [sp, #96]

    /* Save current sp to ctxtsk->tskctxb.ssp. Skip if ctxtsk is NULL
     * (we were dispatched from a non-task context — first ever boot). */
    adrp x0, knl_ctxtsk
    add  x0, x0, :lo12:knl_ctxtsk
    ldr  x0, [x0]
    cbz  x0, .Lno_save
    mov  x1, sp
    str  x1, [x0, #TCB_SSP]
.Lno_save:
    adrp x0, knl_ctxtsk
    add  x0, x0, :lo12:knl_ctxtsk
    str  xzr, [x0]

/* -----------------------------------------------------------------------
 *  Dispatcher loop — pick schedtsk and switch sp to its dormant frame.
 *  If schedtsk is NULL we idle until something becomes runnable; in
 *  this userspace port "idle" is sched_yield (the timer signal will
 *  still arrive and may set schedtsk via knl_timer_handler).
 * --------------------------------------------------------------------- */
.Ldispatch_loop:
    adrp x0, knl_schedtsk
    add  x0, x0, :lo12:knl_schedtsk
    ldr  x0, [x0]
    cbz  x0, .Lidle

    /* ctxtsk = schedtsk */
    adrp x1, knl_ctxtsk
    add  x1, x1, :lo12:knl_ctxtsk
    str  x0, [x1]

    /* Switch sp to schedtsk's dormant frame */
    ldr  x1, [x0, #TCB_SSP]
    mov  sp, x1

    /* Clear dispatch-disabled while we still have it disabled (the
     * incoming task expects to start with dispatch enabled). */
    adrp x1, knl_dispatch_disabled
    add  x1, x1, :lo12:knl_dispatch_disabled
    str  xzr, [x1]

    /* Restore context from the frame */
    ldp x19, x20, [sp,  #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldr x0,       [sp, #96]
    add sp, sp, #112

    /* Store taskmode (x0) into knl_taskmode global */
    adrp x1, knl_taskmode
    add  x1, x1, :lo12:knl_taskmode
    str  x0, [x1]

    /* Re-enable IRQs (lazy — no drain). x30 already holds the resume
     * address loaded from the frame; we must not call out to C from
     * here because that would clobber it. */
    IRQ_FLAG_CLEAR
    ret

.Lidle:
    /* Briefly relinquish the CPU. When the signal arrives our handler
     * may set knl_schedtsk; we'll loop again and find something. */
    IRQ_FLAG_CLEAR
    bl  sched_yield
    IRQ_DISABLE
    b .Ldispatch_loop

/* -----------------------------------------------------------------------
 *  knl_task_entry_trampoline
 *    Entry point for newly-dispatched tasks. The dormant frame
 *    initialised by knl_setup_context puts our address into the x30
 *    slot, so the first `ret` in the dispatcher above lands here.
 *
 *    x19 = stacd (task entry's first arg)
 *    x20 = exinf (task entry's second arg)
 *
 *    Read the task's entry function from its TCB and call it.
 *    If the entry ever returns, fall through to tk_ext_tsk.
 * --------------------------------------------------------------------- */
    .global knl_task_entry_trampoline
knl_task_entry_trampoline:
    /* IRQs are still disabled here (we entered via the dispatcher
     * which disabled them). Tasks expect to run with IRQs enabled. */
    IRQ_FLAG_CLEAR

    adrp x16, knl_ctxtsk
    add  x16, x16, :lo12:knl_ctxtsk
    ldr  x16, [x16]
    ldr  x17, [x16, #TCB_task]

    mov  x0, x19                /* stacd */
    mov  x1, x20                /* exinf */

    adrp x30, tk_ext_tsk
    add  x30, x30, :lo12:tk_ext_tsk

    br   x17                    /* call task(stacd, exinf) */

/* -----------------------------------------------------------------------
 *  knl_timer_handler_startup
 *    Bridge between the SIGALRM signal handler (preempt.c) and
 *    T-Kernel's knl_timer_handler. Increments taskindp counter so
 *    T-Kernel code that checks in_indp() returns TRUE while we run
 *    the timer handler.
 * --------------------------------------------------------------------- */
    .global knl_timer_handler_startup
knl_timer_handler_startup:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    adrp x0, knl_taskindp
    add  x0, x0, :lo12:knl_taskindp
    ldr  w1, [x0]
    add  w1, w1, #1
    str  w1, [x0]

    bl  knl_timer_handler

    adrp x0, knl_taskindp
    add  x0, x0, :lo12:knl_taskindp
    ldr  w1, [x0]
    sub  w1, w1, #1
    str  w1, [x0]

    ldp x29, x30, [sp], #16
    ret

/* Non-executable stack note for Linux toolchains. */
    .section .note.GNU-stack, "", %progbits
