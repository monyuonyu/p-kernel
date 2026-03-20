/*
 *  user.ld — linker script for p-kernel user-mode ELF32 programs
 *
 *  Code is placed at USER_CODE_BASE = 0x00400000 (4 MB).
 *  This address must lie within QEMU's physical memory (default 128 MB).
 *  The flat identity-mapped page tables in start.S cover all 4 GB, and
 *  gdt_user.c sets the US bit so ring-3 can access this memory.
 */

OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
    . = 0x00400000;

    .text   : { *(.text*)   }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*)   }
    .bss    : { *(COMMON) *(.bss*) }

    /* Strip metadata that inflates the binary */
    /DISCARD/ : {
        *(.note*)
        *(.eh_frame*)
        *(.comment*)
        *(.gnu*)
    }
}
