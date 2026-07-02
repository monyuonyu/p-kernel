/*
 *  cpu_support.S (aarch64)
 *  Task dispatcher, context switch, EL1 exception vectors, timer startup
 *
 *  Dormant task stack frame (112 bytes, see cpu_task.h):
 *    [sp+  0]  x19  (= stacd for new task)
 *    [sp+  8]  x20  (= exinf for new task)
 *    [sp+ 16]  x21
 *    [sp+ 24]  x22
 *    [sp+ 32]  x23
 *    [sp+ 40]  x24
 *    [sp+ 48]  x25
 *    [sp+ 56]  x26
 *    [sp+ 64]  x27
 *    [sp+ 72]  x28
 *    [sp+ 80]  x29 (fp)
 *    [sp+ 88]  x30 (lr)  ← trampoline addr for new, caller addr for running
 *    [sp+ 96]  taskmode
 *    [sp+104]  (padding)
 *
 *  TCB field offsets (AArch64, extracted via offsetof — see offset.h):
 *    TCB_task = 40
 *    TCB_SSP  = 200
 */

#define _in_asm_source_
#include "offset.h"

    .text
    .align 4

/* -----------------------------------------------------------------------
 *  knl_taskmode — current task's mode word (global)
 * --------------------------------------------------------------------- */
    .data
    .global knl_taskmode
knl_taskmode:
    .quad   0

    .text

/* -----------------------------------------------------------------------
 *  ②.2a — per-CPU scheduler-pointer ADDRESS macros.
 *
 *  The production dispatcher below references "this CPU's current task" and
 *  "this CPU's next task".  On a uniprocessor those are the single globals
 *  knl_ctxtsk / knl_schedtsk / knl_dispatch_disabled.  Under SMP each CPU has
 *  its own slot in g_smpcpu[] (arch/aarch64/smp.c; struct smp_cpu off 0 / 8 /
 *  56, mirrored by the SMPCPU_* macros below).  These assembler macros load
 *  the ADDRESS of the appropriate per-CPU field into \reg, computing
 *  &g_smpcpu[mpidr_aff0()] via a scratch register.
 *
 *  WITH SMP OFF (the default build) every macro expands to the ORIGINAL
 *  `ldr \reg, =knl_*` — so the dispatcher below is BYTE-IDENTICAL to the
 *  shipped uniprocessor kernel.  (SMPCPU_* are #defined in the SMP_SELFTEST
 *  block at the bottom of this file; the macros are only USED there too.) */
/* struct smp_cpu field offsets + size — KEEP IN SYNC with arch/aarch64/smp.c
 * (guarded there by _Static_assert) and arch/aarch64/include/smp_percpu.h.
 * Defined here (before the dispatcher uses them) AND mirrored in the
 * SMP_SELFTEST block at the bottom for smp_cur_tcb_load. */
#define SMPCPU_CTXTSK      0
#define SMPCPU_SCHEDTSK    8
#define SMPCPU_STACK_TOP  56       /* ②.1b: per-CPU secondary dispatcher SP  */
#define SMPCPU_DISPDIS    64       /* ②.2a: per-CPU dispatch-disable flag    */
#define SMPCPU_SIZE       72       /* ②.1b/②.2a merge: 56 +8 stack +4 disp+pad */

    .macro LD_PERCPU_BASE reg, scratch
    mrs  \scratch, mpidr_el1
    and  \scratch, \scratch, #0xFF        /* MPIDR Aff0 = this CPU id      */
    ldr  \reg, =g_smpcpu
    /* + id * 72 (SMPCPU_SIZE).  72 is not a power of two, so id*72 =
     * id*64 + id*8 = (id<<6) + (id<<3) — two shift-adds, no mul/extra reg. */
    add  \reg, \reg, \scratch, lsl #6      /* + id*64 */
    add  \reg, \reg, \scratch, lsl #3      /* + id*8  => id*72 */
    .endm

/* -----------------------------------------------------------------------
 *  knl_dispatch_to_schedtsk / knl_force_dispatch
 *    Discard current context and switch to schedtsk.
 * --------------------------------------------------------------------- */
    .global knl_dispatch_to_schedtsk
    .global knl_force_dispatch
knl_dispatch_to_schedtsk:
knl_force_dispatch:
    msr daifset, #0x3           /* disable IRQ+FIQ */
#ifdef SMP_SELFTEST
    LD_PERCPU_BASE x0, x2       /* x0 = &g_smpcpu[me] (ctxtsk @ off 0) */
    str xzr, [x0, #SMPCPU_CTXTSK] /* this CPU's ctxtsk = NULL */
#else
    ldr x0, =knl_ctxtsk
    str xzr, [x0]               /* ctxtsk = NULL */
#endif
    b   .Ldispatch_loop

/* -----------------------------------------------------------------------
 *  knl_dispatch_entry / knl_dispatch
 *    Save current context and switch to schedtsk.
 * --------------------------------------------------------------------- */
    .global knl_dispatch_entry
    .global knl_dispatch
knl_dispatch_entry:
knl_dispatch:
    msr daifset, #0x3           /* disable IRQ+FIQ */

    /* Save 112-byte context frame */
    sub sp, sp, #112
    stp x19, x20, [sp,  #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    ldr x0, =knl_taskmode
    ldr x0, [x0]
    str x0,       [sp, #96]

    /* Save SP to ctxtsk->tskctxb.ssp (SP cannot be stored directly) */
#ifdef SMP_SELFTEST
    LD_PERCPU_BASE x2, x3       /* x2 = &g_smpcpu[me] */
    ldr x0, [x2, #SMPCPU_CTXTSK] /* x0 = this CPU's ctxtsk */
    cbz x0, .Lno_save
    mov x1, sp
    str x1, [x0, #TCB_SSP]
.Lno_save:
    str xzr, [x2, #SMPCPU_CTXTSK] /* this CPU's ctxtsk = NULL */
#else
    ldr x0, =knl_ctxtsk
    ldr x0, [x0]
    cbz x0, .Lno_save
    mov x1, sp
    str x1, [x0, #TCB_SSP]
.Lno_save:
    ldr x0, =knl_ctxtsk
    str xzr, [x0]
#endif

/* -----------------------------------------------------------------------
 *  Dispatcher loop — pick schedtsk and switch to it
 * --------------------------------------------------------------------- */
.Ldispatch_loop:
#ifdef SMP_SELFTEST
    LD_PERCPU_BASE x2, x3       /* x2 = &g_smpcpu[me] */
    ldr x0, [x2, #SMPCPU_SCHEDTSK] /* x0 = this CPU's schedtsk */
    cbz x0, .Lsmp_idle

    /* ②.2b-ii §LENS-C guard: NEVER switch into a TCB that is already ANOTHER
     * CPU's LIVE ctxtsk.  The shipped ② slice shares ONE knl_ready_queue (per-CPU
     * run-queues are ②.3 deferred), so when a secondary's task BLOCKS,
     * knl_make_non_ready's re-selection (knl_ready_queue_top) can hand this CPU a
     * task that is RUNNING on another core — switching into its stale, live SP
     * would wild-jump.  Scan all CPUs: if g_smpcpu[c].ctxtsk == schedtsk for any
     * c != me, this task is live elsewhere → IDLE instead (it stays READY; the
     * owning CPU keeps it; when it later truly frees we re-select).  SMP_SELFTEST
     * -gated, so the production dispatcher is byte-identical. */
    mrs x5, mpidr_el1
    and x5, x5, #0xFF                 /* x5 = me */
    ldr x3, =g_smpcpu                 /* x3 = &g_smpcpu[0] */
    mov x4, #0                        /* x4 = c = 0 */
.Lscan_live:
    cmp x4, #8                        /* SMP_MAX_CPUS */
    b.ge .Lnot_live
    cmp x4, x5
    b.eq .Lscan_next                  /* skip me */
    ldr x6, [x3, #SMPCPU_CTXTSK]      /* g_smpcpu[c].ctxtsk */
    cmp x6, x0
    b.eq .Lsmp_idle                   /* schedtsk is live on CPU c → idle */
.Lscan_next:
    add x3, x3, #SMPCPU_SIZE          /* &g_smpcpu[++c] */
    add x4, x4, #1
    b   .Lscan_live
.Lnot_live:

    str x0, [x2, #SMPCPU_CTXTSK]   /* this CPU's ctxtsk = schedtsk */

    ldr x1, [x0, #TCB_SSP]
    mov sp, x1

    str xzr, [x2, #SMPCPU_DISPDIS] /* this CPU's dispatch_disabled = 0 */
#else
    ldr x0, =knl_schedtsk
    ldr x0, [x0]
    cbz x0, .Lidle

    ldr x1, =knl_ctxtsk
    str x0, [x1]

    ldr x1, [x0, #TCB_SSP]
    mov sp, x1

    ldr x1, =knl_dispatch_disabled
    str wzr, [x1]                     /* knl_dispatch_disabled は INT(32bit)。
                                       * str xzr だと隣接シンボルまで 0 埋め
                                       * する潜在バグだったため wzr に修正 */
#endif

    /* Restore context */
    ldp x19, x20, [sp,  #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldr x0,       [sp, #96]
    add sp, sp, #112

    ldr x1, =knl_taskmode
    str x0, [x1]

    msr daifclr, #0x3
    ret                         /* branch to x30 = trampoline or caller */

.Lidle:
    msr daifclr, #0x3
    wfe
    msr daifset, #0x3
    b .Ldispatch_loop

#ifdef SMP_SELFTEST
/* ②.2b-ii SMP idle: a secondary with no runnable task of its OWN must NOT hold
 * the BKL while idling.  A blocking syscall reaches END_CRITICAL_SECTION, which
 * calls knl_dispatch (switching the CPU away) BEFORE BKL_RELEASE — so a task
 * that blocks leaves THIS CPU holding the BKL at depth 1.  If the CPU then
 * idles still holding it, another CPU's bkl_acquire spins FOREVER (the half-(ii)
 * cross-CPU wake deadlock).  So: DROP the BKL (smp_idle_bkl_drop, fully releases
 * iff we own it) before wfe, and RE-ACQUIRE it (smp_idle_bkl_reacquire, to depth
 * 1) before re-checking the dispatch loop — so the task we resume finds the BKL
 * held at the depth its own END_CRITICAL_SECTION's BKL_RELEASE expects.  x19/x20
 * are callee-saved across the C calls; we use the dispatcher stack (SP is the
 * per-CPU dispatcher stack here, valid). */
.Lsmp_idle:
    bl   smp_idle_bkl_drop         /* release the BKL iff this CPU owns it */
    msr  daifclr, #0x3             /* unmask IRQ so the wake (tick/SGI) lands */
    wfe
    msr  daifset, #0x3
    bl   smp_idle_bkl_reacquire    /* re-take the BKL (depth 1) before dispatch */
    b    .Ldispatch_loop
#endif

/* -----------------------------------------------------------------------
 *  knl_task_entry_trampoline
 *    Entry point for newly-dispatched tasks.
 *    Restored x19 = stacd, x20 = exinf (set by knl_setup_stacd).
 * --------------------------------------------------------------------- */
    .global knl_task_entry_trampoline
knl_task_entry_trampoline:
    msr daifclr, #0x3           /* enable IRQ+FIQ in new task */

#ifdef SMP_SELFTEST
    LD_PERCPU_BASE x16, x17     /* x16 = &g_smpcpu[me] */
    ldr x16, [x16, #SMPCPU_CTXTSK] /* x16 = this CPU's ctxtsk (the new task) */
#else
    ldr x16, =knl_ctxtsk
    ldr x16, [x16]
#endif
    ldr x17, [x16, #TCB_task]

    mov x0, x19                 /* 1st arg: stacd */
    mov x1, x20                 /* 2nd arg: exinf */

    ldr x30, =tk_ext_tsk        /* task's return address */

    br  x17                     /* call task(stacd, exinf) */

/* -----------------------------------------------------------------------
 *  knl_timer_handler_startup
 *    Called from the GIC timer-PPI handler in tkdev_init.c.
 * --------------------------------------------------------------------- */
    .global knl_timer_handler_startup
knl_timer_handler_startup:
    /* Save lr — we make a BL below which would otherwise overwrite x30,
     * leaving us no way to return to the IRQ vector. */
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    ldr x0, =knl_taskindp
    ldr w1, [x0]
    add w1, w1, #1
    str w1, [x0]

    bl  knl_timer_handler

    ldr x0, =knl_taskindp
    ldr w1, [x0]
    sub w1, w1, #1
    str w1, [x0]

    ldp x29, x30, [sp], #16
    ret

/* -----------------------------------------------------------------------
 *  EL1 Exception Vectors
 *  Each slot is 128 bytes (.align 7); table aligned to 2KB (.align 11).
 * --------------------------------------------------------------------- */

    .macro save_caller_regs
    sub sp, sp, #160
    stp x0,  x1,  [sp,   #0]
    stp x2,  x3,  [sp,  #16]
    stp x4,  x5,  [sp,  #32]
    stp x6,  x7,  [sp,  #48]
    stp x8,  x9,  [sp,  #64]
    stp x10, x11, [sp,  #80]
    stp x12, x13, [sp,  #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x30, [sp, #144]
    .endm

    .macro restore_caller_regs
    ldp x0,  x1,  [sp,   #0]
    ldp x2,  x3,  [sp,  #16]
    ldp x4,  x5,  [sp,  #32]
    ldp x6,  x7,  [sp,  #48]
    ldp x8,  x9,  [sp,  #64]
    ldp x10, x11, [sp,  #80]
    ldp x12, x13, [sp,  #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x30, [sp, #144]
    add sp, sp, #160
    .endm

    .macro ventry label
    .align 7
    b \label
    .endm

    .align 11
    .global el1_vectors
el1_vectors:
    /* Current EL with SP0 */
    ventry  _vec_sp0_sync
    ventry  _vec_sp0_irq
    ventry  _vec_sp0_fiq
    ventry  _vec_sp0_serr
    /* Current EL with SPx */
    ventry  _vec_el1_sync
    ventry  _vec_el1_irq
    ventry  _vec_el1_fiq
    ventry  _vec_el1_serr
    /* Lower EL AArch64 */
    ventry  _vec_el0_sync
    ventry  _vec_el0_irq
    ventry  _vec_el0_fiq
    ventry  _vec_el0_serr
    /* Lower EL AArch32 */
    ventry  _vec_el0_sync
    ventry  _vec_el0_irq
    ventry  _vec_el0_fiq
    ventry  _vec_el0_serr

_vec_el1_irq:
    save_caller_regs
    /* Reserve a 16-byte stack slot for the ack-time value we need to
     * remember across the handler call (the GIC IAR on virt, a placeholder
     * 0 on RPi 3). w1-w18 are call-clobbered per the AArch64 ABI, so
     * stashing it in a register would lose it across the BLR. */
    sub sp, sp, #16

#ifdef BOARD_RPI3
    /* BCM2837 ARM Local Interrupt Controller: read CORE0_IRQ_SOURCE
     * (0x40000060). Bit 1 = nCNTPNSIRQ. For now the EL1 phys timer
     * is the only enabled source, so any IRQ here is INTID 30. */
    movz x0, #0x0060
    movk x0, #0x4000, lsl #16
    ldr w1, [x0]
    str w1, [sp]
    tbz w1, #1, .Lirq_no_dispatch    /* timer bit clear → spurious */
    mov w2, #30                       /* INTNO_TIMER_GIC */
#else
    /* GICv2: read IAR, mask off the CPU-ID, drop spurious 1023. */
    ldr x0, =gicc_base_ptr
    ldr x0, [x0]
    ldr w1, [x0, #0x0C]         /* GICC_IAR */
    str w1, [sp]
    and w2, w1, #0x3FF
    mov w3, #512
    cmp w2, w3
    b.ge .Lirq_eoi
#endif

    ldr x3, =knl_intvec
    lsl x4, x2, #3
    add x3, x3, x4
    ldr x3, [x3]
    cbz x3, .Lirq_no_dispatch
    blr x3

.Lirq_no_dispatch:
.Lirq_eoi:
    ldr w1, [sp]                /* restore ack-time value */
    add sp, sp, #16
#ifndef BOARD_RPI3
    /* GICv2 needs EOIR to deactivate. On BCM2837 the timer ack is
     * done by writing CNTP_TVAL_EL0 in timer_irq_handler — no EOI. */
    ldr x0, =gicc_base_ptr
    ldr x0, [x0]
    str w1, [x0, #0x10]         /* GICC_EOIR */
#endif

#ifdef SMP_SELFTEST
    /* ═══════════════════════════════════════════════════════════════════
     *  ②.2b-i — TRUE ASYNCHRONOUS register-context preempt (Option A).
     *
     *  The SGI/timer has now been EOIR'd (trap #1: deactivate on the OLD
     *  context's behalf BEFORE we abandon this stack).  SP points at the top
     *  of the interrupted task's 160-byte save_caller_regs frame (the 16-byte
     *  IAR slot was just popped).  If a reschedule is pending on THIS CPU and
     *  it is SAFE to switch (CUR_CTXTSK != CUR_SCHEDTSK, dispatch not disabled,
     *  not task-independent, NOT holding the BKL mid-syscall — §5.4), we
     *  perform a REAL register-context switch FROM interrupt context, reusing
     *  the proven knl_dispatch save/restore, instead of returning to the
     *  interrupted task.
     *
     *  This whole block is SMP_SELFTEST-gated: with no flag the vector falls
     *  straight through to the original `restore_caller_regs; eret` below, so
     *  the production IRQ path is BYTE-IDENTICAL to the shipped kernel.
     *
     *  smp_irq_need_resched() is a C leaf (ABI-clean): it clobbers x0..x18,
     *  but those live in the 160-byte frame on this stack and are restored
     *  either by `restore_caller_regs` (no-switch) or by .Lirq_resume_tramp
     *  (switch path), so the clobber is harmless. */
    bl   smp_irq_need_resched   /* -> 1 = switch this CPU, 0 = normal return */
    cbz  x0, .Lirq_return        /* no switch needed → original epilogue     */

    /* SWITCH PATH (A-ii two-frame nesting).  A mid-loop preempt has NO
     * call-boundary x30 to return to — its true resume PC is ELR_EL1 and its
     * processor state is SPSR_EL1.  Capture both onto the interrupted task's
     * stack (BELOW the 160-byte caller frame), then hand knl_dispatch a
     * synthetic x30 = &.Lirq_resume_tramp so that, when this task is later
     * re-dispatched, .Ldispatch_loop `ret`s into the trampoline which unwinds
     * ELR/SPSR + the 160-byte frame and `eret`s to the exact interrupted PC. */
    mrs  x0, elr_el1
    mrs  x1, spsr_el1
    stp  x0, x1, [sp, #-16]!     /* push ELR_EL1/SPSR_EL1 (16-aligned)        */
    ldr  x30, =.Lirq_resume_tramp /* knl_dispatch saves THIS as the resume PC */
    b    knl_dispatch            /* SAVE interrupted ctx; RESTORE schedtsk     */
    /* knl_dispatch never returns here; it switches stacks into schedtsk. */

.Lirq_return:
#endif /* SMP_SELFTEST */

    restore_caller_regs
    eret

#ifdef SMP_SELFTEST
/* -----------------------------------------------------------------------
 *  .Lirq_resume_tramp — the resume path for an ASYNC-preempted task (②.2b-i).
 *
 *  Reached when an async-preempted task is later re-dispatched: knl_dispatch's
 *  .Ldispatch_loop has restored the task's 112-byte callee-saved frame and
 *  `ret`'d into here (with IRQ/FIQ UNMASKED by .Ldispatch_loop's daifclr).
 *  SP points at the [ELR_EL1|SPSR_EL1] pair we pushed at preempt time, sitting
 *  ON TOP OF the original 160-byte save_caller_regs frame.  We:
 *    1. re-mask IRQ/FIQ (eret will restore the real DAIF from SPSR anyway, but
 *       we must not take a nested IRQ between here and eret),
 *    2. pop + restore ELR_EL1/SPSR_EL1,
 *    3. restore_caller_regs (unwind the 160-byte frame → x0..x18,x30),
 *    4. eret to the exact interrupted PC.
 * --------------------------------------------------------------------- */
    .global _irq_resume_tramp
.Lirq_resume_tramp:
_irq_resume_tramp:
    msr  daifset, #0x3          /* mask I+F until the eret restores SPSR.DAIF */
    ldp  x0, x1, [sp], #16      /* pop ELR_EL1/SPSR_EL1                        */
    msr  elr_el1, x0
    msr  spsr_el1, x1
    restore_caller_regs         /* unwind the 160-byte caller frame           */
    eret                        /* return to the exact interrupted PC         */
#endif /* SMP_SELFTEST */

_vec_el1_sync:
    /* Dump ESR_EL1 + ELR_EL1 + FAR_EL1 to PL011 for diagnosis.
     * Blind writes — no FR.TXFF busy wait, because some fault paths
     * leave the chip in a state where TXFF stays asserted forever and
     * we'd hang inside the diag itself. The 16-byte FIFO catches
     * enough chars to identify the fault; later ones may drop. */
#ifdef BOARD_RPI3
    movz x0, #0x1000               /* low 16 of 0x3F201000 */
    movk x0, #0x3F20, lsl #16
#else
    movz x0, #0x0000
    movk x0, #0x0900, lsl #16    /* QEMU virt PL011 = 0x09000000 */
#endif
    mov w1, #'S'; str w1, [x0]
    mov w1, #'Y'; str w1, [x0]
    mov w1, #'N'; str w1, [x0]
    mov w1, #'C'; str w1, [x0]
    mov w1, #' '; str w1, [x0]
    mrs x2, esr_el1
    mov w1, #'E'; str w1, [x0]
    mov w1, #'='; str w1, [x0]
    bl .Lhex64
    mov w1, #' '; str w1, [x0]
    mrs x2, elr_el1
    mov w1, #'P'; str w1, [x0]
    mov w1, #'='; str w1, [x0]
    bl .Lhex64
    mov w1, #' '; str w1, [x0]
    mrs x2, far_el1
    mov w1, #'A'; str w1, [x0]
    mov w1, #'='; str w1, [x0]
    bl .Lhex64
    mov w1, #'\r'; str w1, [x0]
    mov w1, #'\n'; str w1, [x0]
    b .

/* Hex64 helper: prints x2 as 16 hex digits to UARTDR (x0). */
.Lhex64:
    stp x4, x30, [sp, #-16]!
    mov x4, #16
.Lhex_loop:
    sub x4, x4, #1
    lsl x3, x4, #2
    lsr x3, x2, x3
    and x3, x3, #0xF
    cmp x3, #9
    b.le 1f
    add x3, x3, #('A' - '0' - 10)
1:  add x3, x3, #'0'
    mov w1, w3
    str w1, [x0]
    cbnz x4, .Lhex_loop
    ldp x4, x30, [sp], #16
    ret

_vec_sp0_sync:
_vec_sp0_irq:
_vec_sp0_fiq:
_vec_sp0_serr:
_vec_el1_fiq:
_vec_el1_serr:
_vec_el0_sync:
_vec_el0_irq:
_vec_el0_fiq:
_vec_el0_serr:
    wfe
    b .

    .data
    .global gicc_base_ptr
    .align 3
gicc_base_ptr:
    .quad 0

/* =======================================================================
 *  ②.0 full-SMP slice — the PER-CPU dispatcher entry/load.
 *
 *  Gated by SMP_SELFTEST: with no flag, NONE of this is assembled and the
 *  production dispatcher above (.Ldispatch_loop / knl_dispatch) is
 *  BYTE-IDENTICAL to the shipped uniprocessor kernel.
 *
 *  This is the asm side of the ② change the plan §3/§5 calls for: each CPU
 *  dispatches its OWN per-CPU current task. Where the production dispatcher
 *  (cpu_support.S:90-96) loads the single GLOBAL knl_schedtsk/knl_ctxtsk,
 *  the SMP path loads g_smpcpu[mpidr_aff0()].{schedtsk,ctxtsk} — the
 *  per-CPU scheduler state (arch/aarch64/smp.c). The offsets SMPCPU_* are
 *  mirrored from smp.c (guarded there by _Static_assert).
 *
 *  smp_dispatch_loop is the landing pad start.S branches a secondary into
 *  after EL1 setup; it just calls the C orchestrator smp_dispatch_run()
 *  (arch/aarch64/smp.c), which runs the per-CPU pull-and-run loop under the
 *  BKL. The orchestration is in C for safety (the full task-stack context
 *  switch is DEFERRED — ②.0's cooperative pull, §4.3); this asm provides
 *  the per-CPU current-task LOAD so the per-CPU mechanism is exercised in
 *  asm exactly where the production dispatcher would be generalized.
 * ===================================================================== */
#ifdef SMP_SELFTEST
    .text
    .align 4

/* struct smp_cpu offsets (SMPCPU_CTXTSK/SCHEDTSK/STACK_TOP/DISPDIS/SIZE) are
 * #defined near the TOP of this file (before the per-CPU dispatcher), KEPT IN
 * SYNC with arch/aarch64/smp.c + smp_percpu.h.  ②.1b/②.2a merge: stack_top@56,
 * dispatch_disabled@64, SMPCPU_SIZE 72.  smp_cur_tcb_load below uses madd so
 * the non-power-of-two SIZE is fine; the start.S landing pad reads stack_top
 * at SMPCPU_STACK_TOP via the context_id. */

/* unsigned long smp_this_cpu(void) — defined in smp.c (MPIDR Aff0). We
 * re-derive it inline here to avoid a call in the leaf load helper. */

/* TCB* smp_cur_tcb_load(void)
 *   Return THIS CPU's per-CPU current task (g_smpcpu[me].ctxtsk).
 *   This is the per-CPU generalization of the production dispatcher's
 *   `ldr =knl_ctxtsk; ldr [x0]` (cpu_support.S:84-85,95-96). Proves the
 *   per-CPU current-task read happens in asm, indexed by MPIDR. */
    .global smp_cur_tcb_load
smp_cur_tcb_load:
    mrs  x1, mpidr_el1
    and  x1, x1, #0xFF              /* x1 = MPIDR Aff0 = this CPU id */
    ldr  x0, =g_smpcpu
    mov  x2, #SMPCPU_SIZE
    madd x0, x1, x2, x0             /* x0 = &g_smpcpu[me] */
    ldr  x0, [x0, #SMPCPU_CTXTSK]   /* x0 = g_smpcpu[me].ctxtsk */
    ret

/* void smp_dispatch_loop(void)
 *   The per-CPU dispatcher landing pad (start.S branches the secondary
 *   here after EL1 setup; the primary calls it too). Runs the C
 *   per-CPU pull-and-run loop. Never returns (parks if it ever does). */
    .global smp_dispatch_loop
smp_dispatch_loop:
    bl   smp_dispatch_run          /* C orchestrator (arch/aarch64/smp.c) */
1:  wfe
    b    1b

#if defined(SMP_2TASKS_PROD) || defined(SMP_ASYNC_PREEMPT) || defined(SMP_DEADLOCK_TEST) || defined(SMP_ONE_MIND) || defined(SMP_SECONDARY_WAIT)
/* void smp_prod_enter_dispatch(void)   [②.2a — [smp-2tasks-prod];
 *                                       ②.2b-i — [smp-async-preempt];
 *                                       ②.2b   — [smp-no-deadlock];
 *                                       ②.2c   — [smp-one-mind];
 *                                       ②.2b-ii — [smp-secondary-wait]]
 *   Enter the PRODUCTION per-CPU dispatcher loop (.Ldispatch_loop above) on
 *   THIS CPU.  The loop reads g_smpcpu[me].schedtsk (a REAL T-Kernel TCB the
 *   driver pre-set), stores it into g_smpcpu[me].ctxtsk, loads the TCB's
 *   saved SP (tskctxb.ssp), restores its 112-byte frame, and `ret`s into the
 *   real task — a genuine register-context switch into a real TCB on the
 *   secondary CPU.  Never returns to the caller (it switches stacks).  IRQ+FIQ
 *   are masked on entry exactly as knl_dispatch does.  This is how the
 *   secondary runs a REAL task pulled from the shared knl_ready_queue, not a
 *   struct smp_task stand-in. */
    .global smp_prod_enter_dispatch
smp_prod_enter_dispatch:
    msr  daifset, #0x3             /* mask IRQ+FIQ (as knl_dispatch) */
    b    .Ldispatch_loop           /* never returns — switches into the TCB */
#endif /* SMP_2TASKS_PROD || SMP_ASYNC_PREEMPT || SMP_DEADLOCK_TEST || SMP_ONE_MIND || SMP_SECONDARY_WAIT */
#endif /* SMP_SELFTEST */
