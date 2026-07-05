/*
 *  arch/windows/x86_64/arch_reboot.c
 *  "Reboot" the native Windows p-kernel by exiting the process. A shell
 *  wrapper can relaunch p-kernel.exe for a fresh boot.
 */

#include "arch_reboot.h"

/* Avoid <stdlib.h> here — it collides with T-Kernel placeholders. exit(3)
 * is noreturn since SUSv2; forward-declare it. */
extern void exit(int) __attribute__((noreturn));

void arch_reboot(void)
{
    exit(0);
}
