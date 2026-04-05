/* kloader.ld — stage-1 kernel loader, linked at 0x50000 */
ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)

SECTIONS {
    . = 0x50000;

    .text      : {
        *(.multiboot)
        *(.text)
    }
    .rodata    : { *(.rodata) }
    .data      : { *(.data) }
    .bss       : {
        *(COMMON)
        *(.bss)
    }

    _kloader_end = ALIGN(4096);
}
