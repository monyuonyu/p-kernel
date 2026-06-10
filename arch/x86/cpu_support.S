/*
 *  cpu_support.S (x86)
 *  Task dispatcher and context switch for x86 32-bit protected mode
 *
 *  Stack layout for a switched-out task (ESP points to lowest entry):
 *
 *    [esp+0]  ebx        )
 *    [esp+4]  esi        )  callee-saved registers
 *    [esp+8]  edi        )
 *    [esp+12] ebp        )
 *    [esp+16] taskmode   )  saved task mode word
 *    [esp+20] ret_addr   -> knl_task_entry_trampoline  (new task)
 *                        -> caller inside kernel        (running task)
 *
 *  TCB layout constants (from offset.h, evaluated at compile time):
 *    TCB_TSKCTXB  = TCB_tskctxb
 *    CTXB_SSP     = 0  (ssp is first field of CTXB)
 *  The offset of ssp inside TCB = TCB_TSKCTXB + CTXB_SSP = TCB_TSKCTXB
 */

#define _in_asm_source_

/* Pull in the numeric offset constants */
#include "offset.h"

/* Convenience: byte offset of tskctxb.ssp in TCB */
#define TCB_SSP     (TCB_tskctxb + CTXB_ssp)

    .code32
    .text
    .align 4

/* -----------------------------------------------------------------------
 *  knl_taskmode  - current task's mode word (global variable)
 * --------------------------------------------------------------------- */
    .data
    .globl  knl_taskmode
knl_taskmode:
    .long   0

    .text

/* -----------------------------------------------------------------------
 *  knl_dispatch_to_schedtsk
 *  knl_force_dispatch
 *    Throw away the current task context and dispatch to schedtsk.
 *    Used at system startup and after tk_ext_tsk / tk_exd_tsk.
 * --------------------------------------------------------------------- */
    .globl  knl_dispatch_to_schedtsk
    .globl  knl_force_dispatch
knl_dispatch_to_schedtsk:
knl_force_dispatch:
    cli                         /* disable interrupts */
    movl    $0, (knl_ctxtsk)    /* ctxtsk = NULL (no context to save) */
    jmp     .Ldispatch_loop

/* -----------------------------------------------------------------------
 *  knl_dispatch_entry
 *  knl_dispatch
 *    Save current task context and dispatch to schedtsk.
 *    Called as a regular C function from END_CRITICAL_SECTION.
 * --------------------------------------------------------------------- */
    .globl  knl_dispatch_entry
    .globl  knl_dispatch
knl_dispatch_entry:
knl_dispatch:
    cli                             /* disable interrupts */

    /* Save callee-saved registers + taskmode */
    pushl   (knl_taskmode)
    pushl   %ebp
    pushl   %edi
    pushl   %esi
    pushl   %ebx

    /* Save ESP to ctxtsk->tskctxb.ssp */
    movl    (knl_ctxtsk), %eax
    testl   %eax, %eax
    jz      .Lno_save
    movl    %esp, TCB_SSP(%eax)

    /* Eager FPU save (debt wave): fxsave the outgoing task's
     * x87/MMX/SSE state into its side-table slot (arch/x86/fpu.c).
     * cdecl: %eax/%ecx/%edx are clobbered by the call — none are
     * live here (ESP is already saved; ctxtsk is cleared below).
     * No-op until fpu_init() arms it. */
    pushl   %eax
    call    knl_fpu_save_ctx
    addl    $4, %esp
.Lno_save:
    movl    $0, (knl_ctxtsk)        /* ctxtsk = NULL */

/* -----------------------------------------------------------------------
 *  Dispatcher loop: wait for schedtsk, then switch to it
 * --------------------------------------------------------------------- */
.Ldispatch_loop:
    /* Check if there is a runnable task */
    movl    (knl_schedtsk), %eax
    testl   %eax, %eax
    jz      .Lidle

    /* Switch to schedtsk */
    movl    %eax, (knl_ctxtsk)
    movl    TCB_SSP(%eax), %esp     /* load new task's stack pointer */

    /* Eager FPU restore (debt wave): fxrstor the incoming task's
     * x87/MMX/SSE state (or the clean template on its first run).
     * Runs on the incoming task's stack, below its saved frame —
     * interrupts are still disabled.  %eax is dead after the call
     * (the pops below only use %esp). */
    pushl   %eax
    call    knl_fpu_restore_ctx
    addl    $4, %esp

    /* KILL-TIMER-RACE root-cause (stale TSS.RSP0): refresh RSP0 to the
     * INCOMING task's kernel stack top on EVERY dispatch.  RSP0 was
     * previously set only in user_exec() (initial ring3 launch), never
     * on a context switch.  So after a ring3 task was killed (its kernel
     * stack knl_Ifree'd) or the scheduler switched between/after ring3
     * tasks, RSP0 kept pointing at a PREVIOUS, possibly freed+reused,
     * stack.  The next ring3->ring0 transition (INT 0x80 syscall or an
     * IRQ taken from ring3) makes the CPU push the trap/IRQ frame at
     * that stale RSP0, smashing whatever now occupies that memory ->
     * the intermittent garbage-PC #PF seen during heal-restart of a
     * killed infer_d.elf (NOT a stale timer-queue event: the timer
     * queue was verified intact at the fault).  isstack = top of this
     * task's kernel stack (same value user_exec uses); the write is
     * harmless for pure ring0 tasks (RSP0 unused without a ring3 tenant).
     * NB: this fixes the dominant case; a deeper IRQ-on-deep-stack
     * overflow remains under heavy churn (see gap-ledger follow-up). */
    movl    (knl_ctxtsk), %eax
    pushl   TCB_isstack(%eax)
    call    gdt_set_kernel_stack
    addl    $4, %esp

    /* Enable dispatch */
    movl    $0, (knl_dispatch_disabled)

    /* Restore context */
    popl    %ebx
    popl    %esi
    popl    %edi
    popl    %ebp
    popl    (knl_taskmode)

    sti
    ret                             /* jump to saved/trampoline return address */

.Lidle:
    /* No runnable task – wait for next interrupt */
    sti
    hlt
    cli
    jmp     .Ldispatch_loop

/* -----------------------------------------------------------------------
 *  knl_task_entry_trampoline
 *    "Return address" placed on a new task's stack by setup_context.
 *    When the dispatcher switches to a brand-new task it will "ret" here.
 *
 *    At entry the stack looks like (ESP pointing to lowest):
 *      [esp+0]  &tk_ext_tsk   <- return address for the task function
 *      [esp+4]  stacd          <- 1st argument
 *      [esp+8]  exinf          <- 2nd argument
 *
 *    This is exactly the cdecl frame for task(stacd, exinf).
 *    We just load the task function pointer from TCB and jump.
 * --------------------------------------------------------------------- */
    .globl  knl_task_entry_trampoline
knl_task_entry_trampoline:
    sti                             /* enable interrupts in new task */

    movl    (knl_ctxtsk), %eax      /* eax = current TCB pointer */
    movl    TCB_task(%eax), %eax    /* load function pointer */
    jmp     *%eax                   /* tail-call: task(stacd, exinf) */

/* -----------------------------------------------------------------------
 *  knl_timer_handler_startup
 *    Called from the IRQ0 (PIT timer) interrupt handler.
 *    Enters task-independent mode, calls knl_timer_handler(), then
 *    checks whether a context switch is needed.
 * --------------------------------------------------------------------- */
    .globl  knl_timer_handler_startup
knl_timer_handler_startup:
    /* knl_taskindp++ */
    incl    (knl_taskindp)

    call    knl_timer_handler

    /* knl_taskindp-- */
    decl    (knl_taskindp)

    ret

/* end of cpu_support.S */
