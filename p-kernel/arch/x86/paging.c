/*
 *  paging.c (x86)
 *  Per-process address-space isolation
 *
 *  The boot loader (start.S) builds a flat identity-mapped IA-32e page table:
 *    PML4[0] → PDPT[0] → PD[0..511]  each entry = n*2MB | 0x87 (P+RW+U/S+PS)
 *
 *  paging_init() strips the U/S bit from all PD entries so ring-3 cannot
 *  access kernel memory.  It saves the modified table as "kernel_cr3".
 *
 *  paging_proc_create() allocates a private PML4+PDPT+PD for each user
 *  process, copies the kernel PD (U/S=0 everywhere), then sets PD[2]
 *  (0x400000–0x5FFFFF) to U/S=1 so the ELF code and user stack are
 *  accessible from ring-3.
 *
 *  Since we use identity mapping (VA=PA), C pointer values are physical
 *  addresses — no translation needed.
 */

#include "paging.h"
#include <tmonitor.h>

/* 64-bit page table entry */
typedef unsigned long long PTE;

#define PTE_P   (1ULL << 0)   /* Present                  */
#define PTE_RW  (1ULL << 1)   /* Read/Write               */
#define PTE_US  (1ULL << 2)   /* User/Supervisor          */
#define PTE_PS  (1ULL << 7)   /* Page Size (2 MB huge)    */

#define PT_ENTRIES  512        /* entries per 4 KB table   */

/* ----------------------------------------------------------------- */
/* Page-table pool (3 tables per process × 4 processes = 12 slots)  */
/* ----------------------------------------------------------------- */

#define POOL_SIZE  24

static PTE  pt_pool[POOL_SIZE][PT_ENTRIES] __attribute__((aligned(4096)));
static BOOL pt_used[POOL_SIZE];

static PTE *pool_alloc(void)
{
    for (INT i = 0; i < POOL_SIZE; i++) {
        if (!pt_used[i]) {
            pt_used[i] = TRUE;
            for (INT j = 0; j < PT_ENTRIES; j++)
                pt_pool[i][j] = 0;
            return pt_pool[i];
        }
    }
    return NULL;
}

static void pool_free(PTE *pt)
{
    for (INT i = 0; i < POOL_SIZE; i++) {
        if (pt_pool[i] == pt) {
            pt_used[i] = FALSE;
            return;
        }
    }
}

/* ----------------------------------------------------------------- */
/* State                                                             */
/* ----------------------------------------------------------------- */

static UW kernel_cr3;
static UW task_cr3_table[PAGING_MAX_TASKS];

/* ----------------------------------------------------------------- */
/* paging_init                                                       */
/* ----------------------------------------------------------------- */

void paging_init(void)
{
    INT i;

    /* Initialise pool and per-task table */
    for (i = 0; i < POOL_SIZE;         i++) pt_used[i]          = FALSE;
    for (i = 0; i < PAGING_MAX_TASKS;  i++) task_cr3_table[i]   = 0;

    /* Read current CR3 (= &pml4_table, set by start.S) */
    UW cr3;
    asm volatile("movl %%cr3, %0" : "=r"(cr3));
    kernel_cr3 = cr3;

    /* Walk PML4[0] → PDPT[0] → PD and clear U/S from every present entry */
    PTE *pml4 = (PTE *)(UW)(cr3       & ~0xFFFUL);
    PTE *pdpt = (PTE *)(UW)((UW)(pml4[0]) & ~0xFFFUL);
    PTE *pd   = (PTE *)(UW)((UW)(pdpt[0]) & ~0xFFFUL);

    for (i = 0; i < PT_ENTRIES; i++) {
        if (pd[i] & PTE_P)
            pd[i] &= ~PTE_US;
    }

    /* Strip U/S from upper levels as well (defence in depth) */
    pml4[0] &= ~PTE_US;
    pdpt[0]  &= ~PTE_US;

    /* Flush TLB */
    asm volatile("movl %0, %%cr3" :: "r"(cr3) : "memory");

    tm_putstring((UB *)"[paging] kernel CR3 ready (ring-3 kernel access denied)\r\n");
}

/* ----------------------------------------------------------------- */
/* paging_proc_create                                                */
/* ----------------------------------------------------------------- */

UW paging_proc_create(void)
{
    PTE *proc_pml4 = pool_alloc();
    PTE *proc_pdpt = pool_alloc();
    PTE *proc_pd   = pool_alloc();

    if (!proc_pml4 || !proc_pdpt || !proc_pd) {
        tm_putstring((UB *)"[paging] page table pool exhausted\r\n");
        if (proc_pml4) pool_free(proc_pml4);
        if (proc_pdpt) pool_free(proc_pdpt);
        if (proc_pd)   pool_free(proc_pd);
        return 0;
    }

    /* Copy kernel PD (all U/S=0) into the new process PD */
    {
        PTE *kpml4 = (PTE *)(UW)(kernel_cr3       & ~0xFFFUL);
        PTE *kpdpt = (PTE *)(UW)((UW)(kpml4[0])   & ~0xFFFUL);
        PTE *kpd   = (PTE *)(UW)((UW)(kpdpt[0])   & ~0xFFFUL);
        for (INT i = 0; i < PT_ENTRIES; i++)
            proc_pd[i] = kpd[i];
    }

    /*
     * Grant ring-3 access to PD[2] (0x400000–0x5FFFFF):
     *   - ELF text/data/bss loaded at 0x400000 (user.ld)
     *   - User stack grows down from 0x600000 into 0x5F0000–0x5FFFFF
     */
    proc_pd[2] = (PTE)(0x400000UL) | PTE_P | PTE_RW | PTE_US | PTE_PS;

    /* Wire up PDPT and PML4 with U/S=1 so the MMU can walk them */
    proc_pdpt[0] = (PTE)(UW)proc_pd   | PTE_P | PTE_RW | PTE_US;
    proc_pml4[0] = (PTE)(UW)proc_pdpt | PTE_P | PTE_RW | PTE_US;

    return (UW)proc_pml4;   /* VA = PA (identity mapping) */
}

/* ----------------------------------------------------------------- */
/* paging_proc_destroy                                               */
/* ----------------------------------------------------------------- */

void paging_proc_destroy(UW cr3)
{
    if (!cr3) return;

    PTE *proc_pml4 = (PTE *)(UW)(cr3                  & ~0xFFFUL);
    PTE *proc_pdpt = (PTE *)(UW)((UW)(proc_pml4[0])   & ~0xFFFUL);
    PTE *proc_pd   = (PTE *)(UW)((UW)(proc_pdpt[0])   & ~0xFFFUL);

    pool_free(proc_pd);
    pool_free(proc_pdpt);
    pool_free(proc_pml4);
}

/* ----------------------------------------------------------------- */
/* Per-task CR3 registry                                             */
/* ----------------------------------------------------------------- */

void paging_set_task_cr3(ID tid, UW cr3)
{
    if (tid >= 1 && tid < PAGING_MAX_TASKS)
        task_cr3_table[tid] = cr3;
}

UW paging_get_task_cr3(ID tid)
{
    if (tid >= 1 && tid < PAGING_MAX_TASKS && task_cr3_table[tid])
        return task_cr3_table[tid];
    return kernel_cr3;
}

UW paging_get_kernel_cr3(void) { return kernel_cr3; }
