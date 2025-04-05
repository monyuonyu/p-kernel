/* QEMU x64用リンカースクリプト */
ENTRY(_start)

SECTIONS {
    /* カーネルは1MB以上のアドレスにロードされる */
    . = 0x100000;

    .text : {
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

    /DISCARD/ : {
        *(.comment)
    }
}
