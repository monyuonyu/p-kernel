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

#ifdef KCC_DIAG
extern void print(const char *str);
static void kcc_hex(unsigned int v)
{
    char b[9]; for (int i=7;i>=0;i--){int d=v&0xF;b[i]=d<10?'0'+d:'A'+d-10;v>>=4;} b[8]=0; print(b);
}
extern void *knl_ctxtsk;
static unsigned int kcc_curtid(void)
{
    /* TCB.tskid is at byte offset 8 (offset.h TCB_tskid). */
    void *c = knl_ctxtsk;
    if (!c) return 0;
    return *(volatile unsigned int *)((char *)c + 8);
}
#endif

/* KILL-CHURN-CRASH ROOT FIX (gap-ledger): the page-table pool's alloc/free
 * MUST be interrupt-atomic.  elf_exec() (paging_proc_create) is called
 * concurrently from BOTH the shell/churn task (pri 2) and the heal
 * watchdog (pri 4), and dproc_kill_by_name's teardown (paging_proc_destroy
 * -> pool_free) races them — none of these run in a critical section.  A
 * preemption inside the claim/zero window let two processes get the SAME
 * slot, or freed-then-zeroed a slot under a live CR3, producing the ring0
 * #PF (CR3 == a pt_pool slot, CR2 garbage).  Bracketing the claim and the
 * release with cli/popf makes slot ownership consistent. */
static inline unsigned long kcc_irq_save(void)
{
    unsigned long fl;
    __asm__ volatile ("pushf\n\tpop %0\n\tcli" : "=r"(fl) :: "memory");
    return fl;
}
static inline void kcc_irq_restore(unsigned long fl)
{
    __asm__ volatile ("push %0\n\tpopf" :: "r"(fl) : "memory", "cc");
}

static PTE *pool_alloc(void)
{
    unsigned long fl = kcc_irq_save();
    for (INT i = 0; i < POOL_SIZE; i++) {
        if (!pt_used[i]) {
            pt_used[i] = TRUE;
            kcc_irq_restore(fl);
            /* Zero outside the lock is fine: the slot is now privately
             * owned (pt_used==TRUE) and not yet handed to any CR3. */
            for (INT j = 0; j < PT_ENTRIES; j++)
                pt_pool[i][j] = 0;
            return pt_pool[i];
        }
    }
    kcc_irq_restore(fl);
    return NULL;
}

static void pool_free(PTE *pt)
{
    for (INT i = 0; i < POOL_SIZE; i++) {
        if (pt_pool[i] == pt) {
#ifdef KCC_DIAG
            /* Is this slot the CURRENTLY ACTIVE CR3? Freeing the table the
             * CPU is translating through == the convicted hazard. */
            {
                unsigned int cur;
                asm volatile ("mov %%cr3, %0" : "=r"(cur));
                if ((cur & ~0xFFFu) == ((unsigned int)(unsigned long)pt & ~0xFFFu)) {
                    print("[KCC] pool_free of ACTIVE CR3 slot=0x"); kcc_hex(i);
                    print(" pt=0x"); kcc_hex((unsigned int)(unsigned long)pt);
                    print(" curtid=0x"); kcc_hex(kcc_curtid()); print("\r\n");
                }
            }
#endif
            {
                unsigned long fl = kcc_irq_save();
                pt_used[i] = FALSE;
                kcc_irq_restore(fl);
            }
            return;
        }
    }
}

/* ----------------------------------------------------------------- */
/* Pool observability — `dproc test` leak gate (debt wave)            */
/* Number of page-table pool slots currently in use.  Every live user */
/* process holds exactly 3 (PML4+PDPT+PD); after a full teardown the  */
/* count must return to its pre-exec baseline.                        */
/* ----------------------------------------------------------------- */

W paging_pool_used(void)
{
    W n = 0;
    for (INT i = 0; i < POOL_SIZE; i++)
        if (pt_used[i]) n++;
    return n;
}

/* ----------------------------------------------------------------- */
/* State                                                             */
/* ----------------------------------------------------------------- */

static UW kernel_cr3;
static UW task_cr3_table[PAGING_MAX_TASKS];
static UW task_brk_table[PAGING_MAX_TASKS];   /* per-task heap end (brk) */

/* ----------------------------------------------------------------- */
/* paging_init                                                       */
/* ----------------------------------------------------------------- */

void paging_init(void)
{
    INT i;

    /* Initialise pool and per-task table */
    for (i = 0; i < POOL_SIZE;         i++) pt_used[i]          = FALSE;
    for (i = 0; i < PAGING_MAX_TASKS;  i++) task_cr3_table[i]   = 0;
    for (i = 0; i < PAGING_MAX_TASKS;  i++) task_brk_table[i]   = 0;

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
     * Grant ring-3 access to two regions:
     *
     * Region A — p-kernel native ELFs (user.ld at 0x400000):
     *   PD[2..7]  0x400000–0xFFFFFF   12 MB  code/BSS/heap/stack
     *
     * Region B — Linux-standard ELFs (musl/glibc load at 0x08048000):
     *   PD[64..71] 0x08000000–0x08FFFFFF  16 MB  text/data/heap
     *   PD[319]    0x27E00000–0x27FFFFFF  2 MB   Linux stack area
     *
     * With identity mapping (VA=PA), we just enable the huge-page
     * entries in the process PD for both regions.
     */

    /* Region A: p-kernel native (0x400000–0xFFFFFF) */
    for (INT pd_i = 2; pd_i <= 7; pd_i++)
        proc_pd[pd_i] = (PTE)((UW)pd_i * 0x200000UL)
                        | PTE_P | PTE_RW | PTE_US | PTE_PS;

    /* Region B: Linux standard load address (0x08000000–0x08FFFFFF) */
    for (INT pd_i = 64; pd_i <= 71; pd_i++)
        proc_pd[pd_i] = (PTE)((UW)pd_i * 0x200000UL)
                        | PTE_P | PTE_RW | PTE_US | PTE_PS;

    /* Note: Linux stack uses PD[7] (0xE00000-0xFFFFFF) which is already
     * mapped in Region A above.  No separate stack mapping needed. */

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

/* ----------------------------------------------------------------- */
/* KILL-CHURN-CRASH ROOT FIX (gap-ledger) — dispatch-time CR3 reload. */
/*                                                                    */
/* Mechanism (convicted from a KCC_DIAG ring0 #PF dump):              */
/*   The dispatcher (cpu_support.S) switched kernel STACKS but NEVER  */
/*   CR3.  Only user_exec() ever loaded a process CR3, and it stayed  */
/*   the live CR3 across context switches into OTHER tasks (the       */
/*   kernel is mapped in every address space, so this "worked").      */
/*   When dproc_kill_by_name/user_proc_teardown then destroyed that   */
/*   process (paging_proc_destroy -> pool_free of its PML4/PDPT/PD    */
/*   slots) and the next elf_exec re-allocated the SAME pool slots    */
/*   (pool_alloc zeroes all 512 entries before re-filling), the still-*/
/*   active CR3 of whatever task was running translated kernel        */
/*   addresses through a mid-rewrite / freed table -> a non-present   */
/*   ring0 #PF with CR2 in garbage (e.g. 0xF000FF53) and the saved    */
/*   EIP wherever that task happened to be (classically               */
/*   knl_make_wait_reltim, via tk_dly_tsk).  Proof: at the fault      */
/*   CR3 == &pt_pool[0] (a process table), NOT kernel_cr3.            */
/*                                                                    */
/* Fix: on EVERY dispatch, reload CR3 to the INCOMING task's own      */
/* address space — kernel_cr3 for a ring0 task (no registered CR3),   */
/* or its live process CR3 for a ring3 tenant.  A ring0 task then     */
/* never runs on a recyclable process table, and a tenant runs only   */
/* on its OWN live table (which cannot be freed while it is the       */
/* running ctxtsk — the killer is the one running).  Idempotent and   */
/* cheap (a CR3 reload == a TLB flush, already paid by user_exec).    */
/* Called from .Ldispatch_loop with the incoming task's tid.          */
void knl_dispatch_set_cr3(ID tid)
{
    /* paging_init() runs LATE (inside usermain(), after the first
     * knl_force_dispatch), so kernel_cr3 is 0 during early boot
     * dispatches.  Loading CR3=0 would fault; until paging is armed,
     * leave the boot CR3 (set by start.S) in place.  Likewise never
     * load a 0 task CR3. */
    UW cr3 = paging_get_task_cr3(tid);
#ifdef KCC_DIAG
    /* Catch a dispatch onto a process CR3 whose pool slot is FREE
     * (pt_used==FALSE) — i.e. the table was destroyed but the task is
     * still being dispatched on it.  This is the convicted hazard, caught
     * at the exact dispatch. */
    if (cr3 != 0 && cr3 != kernel_cr3) {
        for (INT i = 0; i < POOL_SIZE; i++) {
            if ((UW)(unsigned long)pt_pool[i] == (cr3 & ~0xFFFUL)) {
                if (!pt_used[i]) {
                    print("[KCC] DISPATCH onto FREED CR3 tid=0x"); kcc_hex((unsigned int)tid);
                    print(" cr3=0x"); kcc_hex((unsigned int)cr3);
                    print(" slot=0x"); kcc_hex((unsigned int)i); print("\r\n");
                }
                break;
            }
        }
    }
#endif
    if (cr3 != 0)
        paging_switch(cr3);
}

/* ----------------------------------------------------------------- */
/* Per-task brk (heap end) registry — used by SYS_BRK (Linux #45)  */
/* ----------------------------------------------------------------- */

void paging_set_task_brk(ID tid, UW brk)
{
    if (tid >= 1 && tid < PAGING_MAX_TASKS)
        task_brk_table[tid] = brk;
}

UW paging_get_task_brk(ID tid)
{
    if (tid >= 1 && tid < PAGING_MAX_TASKS)
        return task_brk_table[tid];
    return 0;
}
