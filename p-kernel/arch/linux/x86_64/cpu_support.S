/*
 *  arch/linux/x86_64/cpu_support.S
 *  T-Kernel dispatcher / context switch for the Linux userspace port,
 *  System V AMD64 ABI.
 *
 *  Direct sibling of arch/linux/aarch64/cpu_support.S — same contract,
 *  different ABI surface. The dormant frame layout (64 bytes) and the
 *  TCB::tskctxb.ssp contract are defined in arch/linux/x86_64/include/
 *  cpu_task.h.
 *
 *  Differences from the bare-metal x86 sibling (arch/x86/):
 *    - 64-bit, not 32-bit. The 32-bit code can't be used.
 *    - No privileged instructions. IRQ enable/disable becomes a write
 *      to arch_irq_disabled_flag, defined in preempt.c (Linux SIGALRM
 *      is the timer IRQ).
 *    - No exception/interrupt vector table. Signals replace it.
 *    - Idle path uses sched_yield instead of `hlt`.
 *
 *  Dormant task stack frame (64 bytes, see cpu_task.h):
 *    [rsp+ 0] r12  (= stacd for new task)
 *    [rsp+ 8] r13  (= exinf for new task)
 *    [rsp+16] r14
 *    [rsp+24] r15
 *    [rsp+32] rbx
 *    [rsp+40] rbp
 *    [rsp+48] taskmode
 *    [rsp+56] rip  (popped by the final `ret`; trampoline for new task)
 */

#define _in_asm_source_
#include "offset.h"

    .text
    .balign 16

/* knl_taskmode lives here — shared global, in .data so the linker
 * resolves it without an arch-specific C file. */
    .data
    .global knl_taskmode
    .type   knl_taskmode, @object
    .size   knl_taskmode, 8
knl_taskmode:
    .quad   0
    .text

/* -----------------------------------------------------------------------
 *  IRQ flag helpers — write 0/1 to arch_irq_disabled_flag (a 32-bit int
 *  in preempt.c). We use rax as scratch; it's caller-saved per System V
 *  and isn't argument or return for any function we care about here.
 * --------------------------------------------------------------------- */

    .macro IRQ_DISABLE
    movl    $1, arch_irq_disabled_flag(%rip)
    .endm

    .macro IRQ_FLAG_CLEAR
    /* Bare flag clear — does NOT drain pending. Use only on dispatch
     * exit / trampoline entry, where draining would risk re-entering
     * knl_dispatch from a signal handler. */
    movl    $0, arch_irq_disabled_flag(%rip)
    .endm

/* -----------------------------------------------------------------------
 *  knl_dispatch_to_schedtsk / knl_force_dispatch
 *    Discard current context and switch to schedtsk. Used when the
 *    running task exits or self-terminates — no register save.
 * --------------------------------------------------------------------- */
    .global knl_dispatch_to_schedtsk
    .global knl_force_dispatch
    .type   knl_dispatch_to_schedtsk, @function
    .type   knl_force_dispatch, @function
knl_dispatch_to_schedtsk:
knl_force_dispatch:
    IRQ_DISABLE
    movq    $0, knl_ctxtsk(%rip)        /* ctxtsk = NULL */
    jmp     .Ldispatch_loop

/* -----------------------------------------------------------------------
 *  knl_dispatch_entry / knl_dispatch
 *    Save current context onto its own stack, write ssp to its TCB,
 *    then pick schedtsk and switch to it.
 *
 *    Called as a function — at entry [rsp] holds the caller's return
 *    address, and we keep it where it is. The 56 bytes we allocate
 *    below it plus that existing 8 = 64-byte dormant frame.
 * --------------------------------------------------------------------- */
    .global knl_dispatch_entry
    .global knl_dispatch
    .type   knl_dispatch_entry, @function
    .type   knl_dispatch, @function
knl_dispatch_entry:
knl_dispatch:
    IRQ_DISABLE

    /* Allocate 56 bytes below the caller's pushed RIP. */
    subq    $56, %rsp
    movq    %r12,  0(%rsp)
    movq    %r13,  8(%rsp)
    movq    %r14, 16(%rsp)
    movq    %r15, 24(%rsp)
    movq    %rbx, 32(%rsp)
    movq    %rbp, 40(%rsp)
    movq    knl_taskmode(%rip), %rax
    movq    %rax, 48(%rsp)
    /* The caller's RIP is already at [rsp+56]. We leave it. */

    /* Save sp to ctxtsk->tskctxb.ssp, unless ctxtsk is NULL (dispatched
     * from a non-task context — first ever boot). */
    movq    knl_ctxtsk(%rip), %rax
    testq   %rax, %rax
    jz      .Lno_save
    movq    %rsp, TCB_SSP(%rax)
.Lno_save:
    movq    $0, knl_ctxtsk(%rip)

/* -----------------------------------------------------------------------
 *  Dispatcher loop — pick schedtsk and switch rsp to its dormant frame.
 *  If schedtsk is NULL, idle by yielding the CPU; the SIGALRM handler
 *  may set schedtsk via knl_timer_handler and we'll find it on the
 *  next loop.
 * --------------------------------------------------------------------- */
.Ldispatch_loop:
    movq    knl_schedtsk(%rip), %rax
    testq   %rax, %rax
    jz      .Lidle

    /* ctxtsk = schedtsk */
    movq    %rax, knl_ctxtsk(%rip)

    /* Switch rsp to schedtsk's dormant frame */
    movq    TCB_SSP(%rax), %rsp

    /* Clear dispatch-disabled. We're still IRQ-disabled at this point
     * — the new task expects to run with dispatch enabled, but the
     * trampoline / restored code will re-enable IRQs itself. */
    movq    $0, knl_dispatch_disabled(%rip)

    /* Restore the frame. Note: do the taskmode store BEFORE clobbering
     * caller-saved regs, so we don't lose rax. */
    movq    48(%rsp), %rax
    movq    %rax, knl_taskmode(%rip)
    movq     0(%rsp), %r12
    movq     8(%rsp), %r13
    movq    16(%rsp), %r14
    movq    24(%rsp), %r15
    movq    32(%rsp), %rbx
    movq    40(%rsp), %rbp
    addq    $56, %rsp           /* rsp now points to the saved rip slot */

    /* Re-enable IRQs (lazy — no drain). We must not call out to C
     * between here and `ret` because the return address is on the
     * stack and any extra push would shift it. */
    IRQ_FLAG_CLEAR
    ret                          /* pops rip; rsp += 8; jumps */

.Lidle:
    /* Briefly relinquish the CPU. The SIGALRM handler may set
     * knl_schedtsk; we'll loop and find something. */
    IRQ_FLAG_CLEAR
    call    sched_yield
    IRQ_DISABLE
    jmp     .Ldispatch_loop

    .size knl_dispatch, . - knl_dispatch

/* -----------------------------------------------------------------------
 *  knl_task_entry_trampoline
 *    First-time entry point for newly-dispatched tasks. The dormant
 *    frame initialised by knl_setup_context puts our address into the
 *    rip slot, so the dispatcher's `ret` lands here.
 *
 *    r12 = stacd (task entry's first arg)
 *    r13 = exinf (task entry's second arg)
 *
 *    rsp is 16-aligned at entry — matching the position needed
 *    immediately before a `call`, so we can invoke the task entry
 *    without further alignment work.
 *
 *    If the task ever returns, fall through into tk_ext_tsk.
 * --------------------------------------------------------------------- */
    .global knl_task_entry_trampoline
    .type   knl_task_entry_trampoline, @function
    .balign 16
knl_task_entry_trampoline:
    /* IRQs are still disabled here (dispatcher disabled them on the
     * way out). Tasks expect IRQs enabled. */
    IRQ_FLAG_CLEAR

    /* Look up the task entry: tcb = knl_ctxtsk; entry = tcb->task. */
    movq    knl_ctxtsk(%rip), %rax
    movq    TCB_task(%rax), %rcx       /* task function pointer */

    movq    %r12, %rdi                 /* stacd → arg 1 */
    movq    %r13, %rsi                 /* exinf → arg 2 */

    /* Make the `ret` of the task land in tk_ext_tsk: push its address
     * as the return target. After this push rsp is no longer 16-
     * aligned (it's ≡ 8 mod 16), which is exactly what ABI requires
     * at the moment we hand control to the task function. */
    leaq    tk_ext_tsk(%rip), %rdx
    pushq   %rdx

    /* Jump to the task. Using jmp (not call) because we already put
     * the return target on the stack ourselves. */
    jmp     *%rcx

    .size knl_task_entry_trampoline, . - knl_task_entry_trampoline

/* -----------------------------------------------------------------------
 *  knl_timer_handler_startup
 *    Bridge between the SIGALRM signal handler (preempt.c) and
 *    T-Kernel's knl_timer_handler. Increments knl_taskindp around the
 *    call so in_indp()-style checks return TRUE inside the handler.
 * --------------------------------------------------------------------- */
    .global knl_timer_handler_startup
    .type   knl_timer_handler_startup, @function
    .balign 16
knl_timer_handler_startup:
    pushq   %rbp
    movq    %rsp, %rbp

    incl    knl_taskindp(%rip)
    call    knl_timer_handler
    decl    knl_taskindp(%rip)

    popq    %rbp
    ret

    .size knl_timer_handler_startup, . - knl_timer_handler_startup

/* Non-executable stack note for Linux toolchains. */
    .section .note.GNU-stack, "", @progbits
