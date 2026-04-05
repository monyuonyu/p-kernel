/* kloader.ld — Stage-1 Kernel Loader linker script
 * kloader.bin is loaded by QEMU (Multiboot) at 0x50000.
 * It is OS-independent: no kernel headers, no T-Kernel.
 * Stack lives just below at 0x4F000.
 */
ENTRY(_kloader_start)
OUTPUT_FORMAT(elf32-i386)

SECTIONS {
    . = 0x50000;

    .text : {
        *(.multiboot)
        *(.text)
    }

    .rodata : { *(.rodata) }
    .data   : { *(.data)   }
    .bss    : { *(COMMON) *(.bss) }

    /DISCARD/ : { *(.comment) }
}
