/*
 *  arch_reboot.c (aarch64)
 *  PSCI SYSTEM_RESET on QEMU virt and ARM Trusted Firmware-equipped boards.
 *
 *  QEMU virt exposes PSCI through HVC by default (psci-conduit=hvc).
 *  Raspberry Pi 3/4 booted bare-metal do NOT have PSCI; on those we
 *  would need a board-specific watchdog reset. For now this covers
 *  the QEMU path; an RPi board override can be added in Phase 3.
 */

#include "arch_reboot.h"

#define PSCI_SYSTEM_RESET  0x84000009UL  /* SMCCC function ID, 32-bit */

void arch_reboot(void)
{
    register unsigned long x0 __asm__("x0") = PSCI_SYSTEM_RESET;
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
    /* Should not return. Fall through to halt loop in case PSCI absent. */
    for (;;) {
        __asm__ volatile("wfi");
    }
}
