/* QEMU x86ブートローダー用リンカースクリプト */
ENTRY(_start)
OUTPUT_FORMAT(binary)

SECTIONS {
    . = 0x7c00;

    .text : {
        *(.text)
        *(.bootsect)
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

    /DISCARD/ : {
        *(.comment)
    }
}
