/* AArch64 QEMU virt linker script
 * Load address: 0x40000000 (QEMU -M virt loads ELF at this address)
 * Stack:        grows down from 0x40100000 (1MB above kernel start)
 */
ENTRY(_start)
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)

SECTIONS {
    . = 0x40000000;

    .text : {
        *(.text.start)      /* start.S _start must be first */
        *(.text)
        *(.text.*)
    }

    . = ALIGN(2048);        /* EL1 exception vectors need 2KB alignment */

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    . = ALIGN(8);

    .data : {
        *(.data)
        *(.data.*)
    }

    . = ALIGN(8);

    _bss_start = .;
    .bss (NOLOAD) : {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }
    . = ALIGN(8);
    _bss_end = .;

    /* 64KB stack */
    . = ALIGN(16);
    _stack_start = .;
    . += 0x10000;
    _stack_top = .;         /* SP initialized here in start.S */

    /* MC-2.0: a dedicated 16KB stack for secondary CPU 1's worker.
     * The worker runs a leaf tile-loop (no deep call chain), so 16KB
     * is ample. Kept separate from the primary's _stack_top so the
     * two cores never share a stack. VA==PA (MMU off). */
    . = ALIGN(16);
    _stack_start_cpu1 = .;
    . += 0x4000;
    _stack_top_cpu1 = .;

    /* MC-2.1 (docs/architecture/mc2-1-ncore-equiv-plan.md §3.5):
     * dedicated 16KB stacks for secondary CPUs 2 and 3, mirroring the
     * cpu1 region exactly (contiguous, no overlap, below _kernel_end).
     * Each worker runs a leaf matmul body (no deep call chain); the
     * synthetic W/x/y operands live in static BSS, NOT on these stacks. */
    . = ALIGN(16);
    _stack_start_cpu2 = .;
    . += 0x4000;
    _stack_top_cpu2 = .;
    . = ALIGN(16);
    _stack_start_cpu3 = .;
    . += 0x4000;
    _stack_top_cpu3 = .;

    /* ②.N8 (the real phone target = 8 physical cores): secondary CPUs 4,5,6,7
     * each need their OWN 16KB dispatcher stack too. We DELIBERATELY do NOT
     * carve them here: extending the linker reservation would move _kernel_end
     * (it is ALIGN(4096) right after these), and _kernel_end's address is
     * embedded in the SHIPPED kernel's .text (cpu_init.c reads it as the heap
     * base) — so growing it would break the DEFAULT-build byte-identity
     * constraint. Instead the cpu4..7 stacks are static, 16-aligned BSS arrays
     * in arch/aarch64/smp.c, fully SMP_SELFTEST-gated: in the default build
     * smp.c emits ZERO bytes, so _bss_end / _kernel_end / .text are unchanged;
     * only the -DSMP_SELFTEST build allocates them. 8 CPUs is the GICv2 ceiling
     * (the GICD_SGIR target-list is 8 bits, 1 per CPU); past 8 needs GICv3. */

    . = ALIGN(4096);
    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
