/*
 *  arch_reboot.c (aarch64)
 *
 *  QEMU virt: PSCI SYSTEM_RESET via HVC. QEMU exposes PSCI through
 *             HVC by default (psci-conduit=hvc).
 *  RPi 3:     no PSCI — BCM2837 boot has no secure firmware to hold
 *             the SMC handler. Use the on-SoC watchdog: arm a 1-tick
 *             timeout and trip a full reset via PM_RSTC.
 */

#include "arch_reboot.h"

#ifdef BOARD_RPI3

/* BCM2837 Power Management block — base 0x3F100000.
 *
 * Every write needs the password 0x5A in the top byte; without it the
 * peripheral silently drops the access. Sequence taken from the Linux
 * bcm2835_wdt driver:
 *   1. Arm the watchdog with a 1-tick timeout (~16us).
 *   2. Write PM_RSTC with WRCFG=FULL_RESET; the SoC resets when the
 *      watchdog expires.
 */
#define PM_RSTC         (*(volatile unsigned int *)0x3F10001CUL)
#define PM_WDOG         (*(volatile unsigned int *)0x3F100024UL)
#define PM_PASSWORD     0x5A000000U
#define PM_RSTC_WRCFG_MASK       0x00000030U
#define PM_RSTC_WRCFG_FULL_RESET 0x00000020U

void arch_reboot(void)
{
    PM_WDOG = PM_PASSWORD | 1U;
    unsigned int rstc = PM_RSTC;
    PM_RSTC = PM_PASSWORD
            | (rstc & ~PM_RSTC_WRCFG_MASK)
            | PM_RSTC_WRCFG_FULL_RESET;
    /* The watchdog fires ~16us later — wait for it. */
    for (;;) {
        __asm__ volatile("wfi");
    }
}

#else

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

#endif /* BOARD_RPI3 */
