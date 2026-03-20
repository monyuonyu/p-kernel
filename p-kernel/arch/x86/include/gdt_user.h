/*
 *  gdt_user.h (x86)
 *  GDT ring-3 extension: user-mode code/data segments + 64-bit TSS
 *
 *  Selectors (after gdt_init_userspace()):
 *    0x20  — 32-bit user code  (DPL=3)  → CS  for ring3 tasks
 *    0x28  — 32-bit user data  (DPL=3)  → DS/ES/FS/GS/SS for ring3 tasks
 *    0x30  — 64-bit TSS        (DPL=0)  → loaded with ltr (16-byte descr.)
 *
 *  RPL-qualified selectors (with | 3):
 *    USER_CS = 0x23
 *    USER_DS = 0x2B
 *
 *  User-space memory map (fits inside QEMU default 128 MB):
 *    USER_CODE_BASE  0x00400000  ELF segments loaded here  (4 MB)
 *    USER_STACK_TOP  0x00600000  user stack top            (6 MB)
 *    USER_STACK_SIZE 0x00010000  64 KB stack
 */
#pragma once
#include "kernel.h"

#define USER_CS     0x23    /* ring3 code selector  (0x20 | RPL3) */
#define USER_DS     0x2B    /* ring3 data selector  (0x28 | RPL3) */
#define KERN_CS     0x08    /* ring0 32-bit compat code selector   */
#define KERN_DS     0x10    /* ring0 data selector                 */
#define TSS_SEL     0x30    /* 64-bit TSS selector                 */

/* User-space process memory map */
#define USER_CODE_BASE  0x00400000UL    /* ELF loads here  (4 MB)  */
#define USER_STACK_TOP  0x00600000UL    /* stack top       (6 MB)  */
#define USER_STACK_SIZE 0x00010000UL    /* 64 KB user stack        */

/*
 * Set up ring-3 GDT entries and 64-bit TSS.
 * Call once from usermain() before any ring-3 task is created.
 */
void gdt_init_userspace(void);

/*
 * Update TSS.RSP0 (ring-0 stack pointer used on ring3→ring0 switch).
 * Must be called before each IRET to ring-3 so that subsequent INT/trap
 * from user code finds a valid kernel stack.
 */
void gdt_set_kernel_stack(UW esp0);
