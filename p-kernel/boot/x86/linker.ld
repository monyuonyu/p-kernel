/* QEMU 完全64ビットカーネル用リンカースクリプト */
ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)

SECTIONS {
    . = 0x100000;

    .text : {
        *(.multiboot)
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(COMMON)
        *(.bss)
    }

    .noinit (NOLOAD) : {
        *(.noinit)
    }

    _kernel_end = ALIGN(4096);  /* ページ境界に丸めたカーネル終端アドレス */

    /DISCARD/ : {
        *(.comment)
    }
}
