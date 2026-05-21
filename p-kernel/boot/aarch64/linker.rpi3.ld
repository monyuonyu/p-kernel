/* AArch64 Raspberry Pi 3 linker script
 * Load address: 0x80000 — the BCM2837 firmware drops the AArch64
 * payload here when config.txt sets arm_64bit=1 + kernel=kernel8.img.
 * Stack: grows down from end of image + 64 KB.
 */
ENTRY(_start)
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)

SECTIONS {
    . = 0x80000;

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

    /* 64 KB stack */
    . = ALIGN(16);
    _stack_start = .;
    . += 0x10000;
    _stack_top = .;

    . = ALIGN(4096);
    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
