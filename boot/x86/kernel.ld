/* kernel.ld — p-kernel ELF linker script (loaded by kloader)
 *
 * Differs from linker.ld:
 *  - .multiboot section is discarded (kloader has its own Multiboot header)
 *  - .text comes first so _start lands exactly at 0x100000
 *
 * kloader jumps to e_entry (= _start = 0x100000) after loading all PT_LOAD
 * segments.  EAX/EBX contain the original Multiboot magic/info pointer so
 * start.S proceeds identically to a direct Multiboot boot.
 */
ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)

SECTIONS {
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

    .noinit (NOLOAD) : {
        *(.noinit)
    }

    _kernel_end = ALIGN(4096);

    /DISCARD/ : {
        *(.comment)
        *(.multiboot)
    }
}
