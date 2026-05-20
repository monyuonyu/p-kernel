/*
 *  arch_reboot.c (x86)
 *  ACPI reset register on QEMU/PC: write 0x06 to I/O port 0xCF9.
 */

#include "arch_reboot.h"

void arch_reboot(void)
{
    __asm__ volatile(
        "movw $0xCF9, %%dx\n\t"
        "movb $0x06, %%al\n\t"
        "outb %%al, %%dx\n\t"
        :: : "eax", "edx"
    );
    for (;;);
}
