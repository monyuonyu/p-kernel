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

    . = ALIGN(4096);
    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
