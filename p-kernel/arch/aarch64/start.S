/*
 *  start.S (aarch64)
 *  Bare-metal entry point for QEMU virt / RPi3 AArch64
 *
 *  QEMU -M virt starts CPUs at EL2 (or EL3 with secure=on).
 *  RPi3 primary core also starts at EL2 (secondary cores parked at EL2).
 *
 *  Flow: EL2 setup → drop to EL1 → stack → BSS clear → main()
 */

#define _in_asm_source_

    .section .text.start, "ax"
    .global _start

_start:
    /* Silence all secondary cores immediately */
    mrs x0, mpidr_el1
    and x0, x0, #0xFF           /* extract CPU ID (Aff0) */
    cbnz x0, _secondary_park

    /* ---- Check current EL ---- */
    mrs x0, CurrentEL
    lsr x0, x0, #2

    cmp x0, #3
    b.eq _from_el3

    cmp x0, #2
    b.eq _from_el2

    /* Already at EL1 — just go */
    b _el1_entry

/* -----------------------------------------------------------------------
 *  EL3 → EL1 (uncommon on QEMU virt, but handles secure= configs)
 * --------------------------------------------------------------------- */
_from_el3:
    /* Route FIQ/IRQ/SError to Non-Secure EL1 */
    mov x0, #0x531              /* NS|EA|FIQ|IRQ|RW(AArch64) */
    msr scr_el3, x0
    mov x0, #0x3C5              /* SPSR: EL1h, DAIF masked */
    msr spsr_el3, x0
    adr x0, _el1_entry
    msr elr_el3, x0
    isb
    eret

/* -----------------------------------------------------------------------
 *  EL2 → EL1
 * --------------------------------------------------------------------- */
_from_el2:
    /* Allow AArch64 EL1 execution */
    mov x0, #(1 << 31)          /* RW bit: EL1 is AArch64 */
    msr hcr_el2, x0

    /* SCTLR_EL1: disable MMU/cache for now */
    mov x0, #0x0800             /* SCTLR minimal value */
    orr x0, x0, #(1 << 11)     /* EOS */
    msr sctlr_el1, x0

    /* SPSR_EL2: enter EL1h with all interrupts masked */
    mov x0, #0x3C5
    msr spsr_el2, x0
    adr x0, _el1_entry
    msr elr_el2, x0
    isb
    eret

/* -----------------------------------------------------------------------
 *  EL1 entry
 * --------------------------------------------------------------------- */
_el1_entry:
    /* Set up EL1 stack (defined by linker as _stack_top) */
    ldr x0, =_stack_top
    mov sp, x0

    /* Install EL1 exception vectors */
    ldr x0, =el1_vectors
    msr vbar_el1, x0
    isb

    /* Clear BSS */
    ldr x0, =_bss_start
    ldr x1, =_bss_end
    mov x2, #0
.Lbss_loop:
    cmp x0, x1
    b.ge .Lbss_done
    str x2, [x0], #8
    b .Lbss_loop
.Lbss_done:

    /* Enable caches and instruction cache at EL1 */
    mrs x0, sctlr_el1
    orr x0, x0, #(1 << 2)      /* C — data cache enable */
    orr x0, x0, #(1 << 12)     /* I — instruction cache enable */
    msr sctlr_el1, x0
    isb

    /* Jump to C main */
    bl  main

    /* Should never return */
_halt:
    wfe
    b _halt

/* -----------------------------------------------------------------------
 *  Secondary core parking loop
 * --------------------------------------------------------------------- */
_secondary_park:
    wfe
    b _secondary_park

    .size _start, . - _start
