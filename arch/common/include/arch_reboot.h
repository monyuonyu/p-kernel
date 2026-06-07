/*
 *  arch_reboot.h
 *  Architecture-supplied full-system reboot.
 *
 *  Each architecture provides arch_reboot() in its own arch/<arch>/arch_reboot.c.
 *  - x86: ACPI reset register (port 0xCF9 ← 0x06) on QEMU/PC.
 *  - aarch64: PSCI SYSTEM_RESET via HVC #0 on QEMU virt and Raspberry Pi.
 *  The function does not return.
 */

#pragma once

void arch_reboot(void) __attribute__((noreturn));
