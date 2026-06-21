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
#ifdef BOARD_RPI3
    /* Linux AArch64 image header — QEMU raspi3b checks the magic at
     * offset 56 to decide between AArch32 and AArch64 boot.  We keep
     * the structure exactly, with code0 doing a forward branch to the
     * real entry below so the first executed instruction is sensible
     * regardless of whether the firmware boots us via the header path
     * or by jumping straight to 0x80000.  Docs: kernel.org
     *   Documentation/arm64/booting.rst (Image header layout). */
    b   _rpi3_start                 /* code0  */
    .long 0                          /* code1 (PE COFF stub offset)   */
    .quad 0x00080000                 /* text_offset  (image load off) */
    .quad _kernel_end - _start       /* image_size                    */
    .quad 0                          /* flags (page-size etc — 4K LE) */
    .quad 0                          /* res2                          */
    .quad 0                          /* res3                          */
    .quad 0                          /* res4                          */
    .ascii "ARM\x64"                 /* magic at offset 56            */
    .long 0                          /* res5 (PE-COFF header offset)  */

_rpi3_start:
    /* Fall through into the common path below — header done, real boot
     * starts here. */
#endif
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

    /* Enable FP/SIMD access at EL1 — without this, the first FP op
     * (e.g. dtr_init weight initialisation) traps to _vec_el1_sync
     * and the kernel hangs silently. CPACR_EL1.FPEN[21:20] = 0b11
     * disables trapping at both EL0 and EL1. */
    mrs x0, cpacr_el1
    orr x0, x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    /* Jump to C main */
    bl  main

    /* Should never return */
_halt:
    wfe
    b _halt

/* -----------------------------------------------------------------------
 *  Secondary core parking loop
 *
 *  Reached at RESET by every non-primary core (mpidr Aff0 != 0). These
 *  cores have done NONE of the primary's EL2->EL1 setup. MC-2.0 does not
 *  wake cores from here — it releases them via PSCI CPU_ON to the fresh
 *  landing pad _secondary_worker below. This park stays as the safety
 *  net for any core not explicitly released.
 * --------------------------------------------------------------------- */
_secondary_park:
    wfe
    b _secondary_park

/* -----------------------------------------------------------------------
 *  MC-2.0 secondary worker bringup (docs/architecture/
 *  mc2-baremetal-smp-plan.md §3.2, §7).
 *
 *  Entered via PSCI CPU_ON (mc2_smp.c). The firmware (QEMU's PSCI impl)
 *  starts this core at the boot EL (EL2 on QEMU virt) with:
 *      x0 = context_id  = &g_cpu[1]   (the per-CPU struct ptr)
 *      MMU off, caches off, no stack, no vectors.
 *
 *  We must: preserve x0, drop EL2->EL1 (matching the primary), set up
 *  this core's own EL1 (SCTLR caches + I-cache, CPACR FP, vbar, and
 *  CRUCIALLY CPUECTLR.SMPEN=1 for inner-shareable coherency), load this
 *  core's own stack from the per-CPU block, then enter the C worker.
 *
 *  The primary boot path (_from_el2 / _el1_entry) is UNTOUCHED — this is
 *  a parallel path that re-derives the SAME EL1 bits (NOT a branch into
 *  the primary's BSS-clearing sequence, which a secondary must skip).
 * --------------------------------------------------------------------- */
    .global _secondary_worker
_secondary_worker:
    /* x0 holds context_id (&g_cpu[slot]); preserve it across the EL drop
     * in a callee-stable register the EL1 setup does not clobber. */
    mov x19, x0

    /* ---- Drop to EL1 if needed (mirrors the primary's EL check) ---- */
    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.eq _sec_from_el2
    cmp x0, #3
    b.eq _sec_from_el3
    /* Already at EL1 — fall through to the EL1 setup. */
    b   _secondary_el1_setup

_sec_from_el3:
    mov x0, #0x531              /* NS|EA|FIQ|IRQ|RW(AArch64) — as primary */
    msr scr_el3, x0
    mov x0, #0x3C5              /* SPSR: EL1h, DAIF masked */
    msr spsr_el3, x0
    adr x0, _secondary_el1_setup
    msr elr_el3, x0
    isb
    eret

_sec_from_el2:
    mov x0, #(1 << 31)          /* HCR_EL2.RW: EL1 is AArch64 — as primary */
    msr hcr_el2, x0
    mov x0, #0x0800             /* SCTLR_EL1 minimal — as primary :81 */
    orr x0, x0, #(1 << 11)     /* EOS — as primary :82 */
    msr sctlr_el1, x0
    mov x0, #0x3C5              /* SPSR_EL2: EL1h, DAIF masked — as primary */
    msr spsr_el2, x0
    adr x0, _secondary_el1_setup
    msr elr_el2, x0
    isb
    eret

/* -----------------------------------------------------------------------
 *  _secondary_el1_setup — bring this core's EL1 to the SAME config the
 *  primary set in _el1_entry (caches + I-cache + FP + vbar), PLUS
 *  CPUECTLR.SMPEN=1 (§1.3). The secondary must NOT clear BSS (the primary
 *  already did, and BSS is shared) and must NOT call main().
 *
 *  On entry: x19 = context_id (&g_cpu[slot]), now at EL1h.
 * --------------------------------------------------------------------- */
    .global _secondary_el1_setup
_secondary_el1_setup:
    /* Install the SAME EL1 exception vectors the primary uses. */
    ldr x0, =el1_vectors
    msr vbar_el1, x0
    isb

    /* Enable D-cache + I-cache — IDENTICAL bits to the primary
     * (_el1_entry :118-121): SCTLR_EL1.C (bit 2) and .I (bit 12). MMU
     * (bit 0, M) stays CLEAR — VA==PA, no TTBR. */
    mrs x0, sctlr_el1
    orr x0, x0, #(1 << 2)      /* C — data cache enable */
    orr x0, x0, #(1 << 12)     /* I — instruction cache enable */
    msr sctlr_el1, x0
    isb

    /* Enable FP/SIMD access at EL1 — IDENTICAL to the primary
     * (_el1_entry :128-130): CPACR_EL1.FPEN[21:20] = 0b11. The C worker
     * may touch FP/SIMD in future tiles; match the primary now. */
    mrs x0, cpacr_el1
    orr x0, x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    /* CPUECTLR_EL1.SMPEN (S3_1_C15_C2_1, bit 6) — join the
     * inner-shareable coherency domain so dsb ish / dmb are sound
     * across cores (§1.3). The primary sets its own bit in
     * mc2_smp_release_one() (C). */
    mrs x0, S3_1_C15_C2_1
    orr x0, x0, #(1 << 6)
    msr S3_1_C15_C2_1, x0
    isb

    /* Load THIS core's own stack from the per-CPU block (x19 =
     * &g_cpu[slot]; offset PK_CPU_STACK_TOP == 0). stack_top is the TOP
     * of the region; SP grows down. */
    ldr x0, [x19, #0]          /* PK_CPU_STACK_TOP */
    mov sp, x0

    /* Enter the C worker loop — never returns. (It reads its own slot
     * via g_cpu[1] in C; x19 is no longer needed by the worker.) */
    bl  pk_smp_worker_loop

    /* Defensive: if the worker ever returns, park forever. */
1:  wfe
    b   1b

/* -----------------------------------------------------------------------
 *  ②.0 full-SMP slice — secondary DISPATCHER bringup.
 *
 *  Gated by SMP_SELFTEST: with no flag, NONE of this is assembled and the
 *  bringup above (_secondary_worker / _secondary_el1_setup) is unchanged.
 *
 *  _secondary_dispatch_entry is a NEW landing pad (ADDED — it does NOT
 *  touch _secondary_worker). PSCI CPU_ON releases the secondary here with
 *      x0 = context_id = &g_smpcpu[1]
 *  (arch/aarch64/smp.c:smp_bringup_secondary). It drops EL2->EL1 exactly
 *  like _secondary_worker, runs the SAME _secondary_el1_setup body up to
 *  the stack load, then — instead of the tile worker — sets its OWN
 *  dispatcher stack (g_smp_sec_stack_top, smp.c) and branches into the
 *  per-CPU dispatcher smp_dispatch_loop (cpu_support.S). §5 of the plan:
 *  "secondaries run the DISPATCHER, not a tile loop."
 * --------------------------------------------------------------------- */
#ifdef SMP_SELFTEST
    .global _secondary_dispatch_entry
_secondary_dispatch_entry:
    /* Preserve context_id across the EL drop (as _secondary_worker). */
    mov x19, x0

    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.eq _sec_disp_from_el2
    cmp x0, #3
    b.eq _sec_disp_from_el3
    b   _secondary_disp_el1_setup

_sec_disp_from_el3:
    mov x0, #0x531
    msr scr_el3, x0
    mov x0, #0x3C5
    msr spsr_el3, x0
    adr x0, _secondary_disp_el1_setup
    msr elr_el3, x0
    isb
    eret

_sec_disp_from_el2:
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    mov x0, #0x0800
    orr x0, x0, #(1 << 11)
    msr sctlr_el1, x0
    mov x0, #0x3C5
    msr spsr_el2, x0
    adr x0, _secondary_disp_el1_setup
    msr elr_el2, x0
    isb
    eret

_secondary_disp_el1_setup:
    /* SAME EL1 bits the primary/worker set (vbar, caches, FP, SMPEN). */
    ldr x0, =el1_vectors
    msr vbar_el1, x0
    isb

    mrs x0, sctlr_el1
    orr x0, x0, #(1 << 2)      /* C */
    orr x0, x0, #(1 << 12)     /* I */
    msr sctlr_el1, x0
    isb

    mrs x0, cpacr_el1
    orr x0, x0, #(3 << 20)     /* FPEN */
    msr cpacr_el1, x0
    isb

    mrs x0, S3_1_C15_C2_1
    orr x0, x0, #(1 << 6)      /* CPUECTLR.SMPEN */
    msr S3_1_C15_C2_1, x0
    isb

    /* Load THIS CPU's own dispatcher stack (g_smp_sec_stack_top, smp.c).
     * Distinct region from the primary — the two cores never share a
     * stack (linker.ld _stack_top_cpu1, reused). */
    ldr x0, =g_smp_sec_stack_top
    ldr x0, [x0]
    mov sp, x0

    /* Enter the per-CPU dispatcher. Never returns. */
    b   smp_dispatch_loop
#endif /* SMP_SELFTEST */

    .size _start, . - _start
