/*
 *  arch/linux/aarch64/arch_reboot.c
 *  "Reboot" the Linux-hosted p-kernel by exiting the process. The
 *  shell wrapper can relaunch ./p-kernel to get a fresh boot.
 */

#include "arch_reboot.h"

/* Avoid <stdlib.h> in this TU — it collides with T-Kernel placeholders.
 * exit(3) is documented to be noreturn since SUSv2; just forward-declare. */
extern void exit(int) __attribute__((noreturn));

void arch_reboot(void)
{
    exit(0);
}
