/*
 *  paging.h (x86)
 *  Per-process address-space isolation using IA-32e 2MB huge-page tables
 *
 *  Memory layout (2MB huge pages):
 *    PD[0]  0x000000–0x1FFFFF  legacy/BIOS + kernel BSS   U/S=0  kernel only
 *    PD[1]  0x200000–0x3FFFFF  (reserved / unmapped)      U/S=0  kernel only
 *    PD[2]  0x400000–0x5FFFFF  user code + user stack      U/S=1  in proc CR3
 *    PD[3+] device/extended kernel                         U/S=0  kernel only
 *
 *  User ELF is linked at 0x400000 (user.ld).
 *  Initial user ESP = USER_STACK_TOP = 0x600000 (defined in gdt_user.h);
 *  stack grows down into 0x5F0000–0x5FFFFF — still within PD[2].
 */
#pragma once
#include "kernel.h"

/* Maximum number of task IDs tracked */
#define PAGING_MAX_TASKS  32

/*
 * paging_init()
 *   Strip U/S from all PD entries in the boot page tables so ring-3
 *   cannot access kernel memory.  Call once from usermain() before any
 *   ring-3 task is created.  Saves the result as the "kernel CR3".
 */
void paging_init(void);

/*
 * paging_proc_create()
 *   Allocate a new set of page tables for a user process.
 *   Copies the kernel PD (all U/S=0) and grants U/S=1 for PD[2]
 *   (0x400000–0x5FFFFF) so the user ELF code and stack are accessible
 *   from ring-3.
 *   Returns the physical address of the new PML4 (= process CR3),
 *   or 0 on allocation failure.
 */
UW paging_proc_create(void);

/*
 * paging_proc_destroy()
 *   Return the three page tables allocated by paging_proc_create() to
 *   the internal pool.  The caller must have already switched back to
 *   the kernel CR3 before calling this.
 */
void paging_proc_destroy(UW cr3);

/* Per-task CR3 registry */
void paging_set_task_cr3(ID tid, UW cr3);
UW   paging_get_task_cr3(ID tid);   /* returns kernel_cr3 if tid unknown */

/* Returns the kernel CR3 established by paging_init() */
UW paging_get_kernel_cr3(void);

/* Reload CR3 — flushes the entire TLB */
static inline void paging_switch(UW cr3)
{
    asm volatile("movl %0, %%cr3" :: "r"(cr3) : "memory");
}
