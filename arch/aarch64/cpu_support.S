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
 *  knl_dispatch_to_schedtsk / knl_force_dispatch
 *    Discard current context and switch to schedtsk.
 * --------------------------------------------------------------------- */
    .global knl_dispatch_to_schedtsk
    .global knl_force_dispatch
knl_dispatch_to_schedtsk:
knl_force_dispatch:
    msr daifset, #0x3           /* disable IRQ+FIQ */
    ldr x0, =knl_ctxtsk
    str xzr, [x0]               /* ctxtsk = NULL */
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
    ldr x0, =knl_ctxtsk
    ldr x0, [x0]
    cbz x0, .Lno_save
    mov x1, sp
    str x1, [x0, #TCB_SSP]
.Lno_save:
    ldr x0, =knl_ctxtsk
    str xzr, [x0]

/* -----------------------------------------------------------------------
 *  Dispatcher loop — pick schedtsk and switch to it
 * --------------------------------------------------------------------- */
.Ldispatch_loop:
    ldr x0, =knl_schedtsk
    ldr x0, [x0]
    cbz x0, .Lidle

    ldr x1, =knl_ctxtsk
    str x0, [x1]

    ldr x1, [x0, #TCB_SSP]
    mov sp, x1

    ldr x1, =knl_dispatch_disabled
    str xzr, [x1]

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

/* -----------------------------------------------------------------------
 *  knl_task_entry_trampoline
 *    Entry point for newly-dispatched tasks.
 *    Restored x19 = stacd, x20 = exinf (set by knl_setup_stacd).
 * --------------------------------------------------------------------- */
    .global knl_task_entry_trampoline
knl_task_entry_trampoline:
    msr daifclr, #0x3           /* enable IRQ+FIQ in new task */

    ldr x16, =knl_ctxtsk
    ldr x16, [x16]
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

    restore_caller_regs
    eret

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
