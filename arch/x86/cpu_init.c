/*
 *  cpu_init.c (x86)
 *  CPU-dependent initialization for x86 T-Kernel
 */

#include "kernel.h"
#include "cpu_insn.h"
#include <subsystem.h>
#include "memory.h"
#include "task.h"
IMPORT ER knl_init_Imalloc(void);

/* Interrupt vector table - indexed by vector number (0..255) */
FP knl_intvec[256];

/* Task-independent part counter */
W knl_taskindp = 0;

/* Kernel memory area (set by knl_cpu_initialize based on _kernel_end) */
EXPORT void *knl_lowmem_top;
EXPORT void *knl_lowmem_limit;

/* Linker symbol: end of kernel image (defined in linker.ld) */
extern char _kernel_end[];

/*
 * knl_cpu_initialize
 *   Called from knl_t_kernel_main() (tkstart.c) during kernel startup.
 *   On x86 without USE_TRAP, no SVC vectors need registering.
 *   The IDT is already set up by boot/x86/idt.c.
 */
EXPORT ER knl_cpu_initialize(void)
{
    /* Clear vector table */
    for (INT i = 0; i < 256; i++) {
        knl_intvec[i] = NULL;
    }

    /* Set up kernel memory area for T-Kernel imalloc.
     *
     * KILL-CHURN-CRASH ROOT FIX (gap-ledger): imalloc hands out the
     * kernel-side TASK STACKS (tk_cre_tsk -> knl_Imalloc).  The x86
     * ring3 grant (paging_proc_create, Region A) maps 0x400000-0xFFFFFF
     * — the SAME 4..16 MB window — as PTE_US|PTE_RW into EVERY user
     * address space, and the p-kernel native user stack top is
     * USER_STACK_TOP = 0x1000000 (16 MB).  With _kernel_end ~= 13.5 MB,
     * imalloc previously started INSIDE that ring3-writable window, so
     * task stacks landed at ~0x00D8xxxx (just above _kernel_end).  Under
     * kill/heal churn of infer_d.elf, a ring3 user write (its own user
     * stack/heap, all in the shared Region A) CLOBBERS a live task's
     * kernel stack -> the saved dispatch frame is corrupted -> the
     * dispatcher "ret"s into a garbage PC == the intermittent #PF whose
     * EIP is always inside knl_make_wait_reltim (CS=0x08).  This is NOT
     * a TCB use-after-free and NOT a stack overflow (it reproduces at
     * 2/8/16 KB stacks because the clobber is EXTERNAL); the prior
     * waves' framing is corrected here, proven by a dispatch-trace dump
     * showing the faulting task stacks at 0x00D8xxxx in the ring3 window.
     *
     * Fix: float imalloc's base ABOVE the ring3-reachable window (>= 16
     * MB) so kernel task stacks can NEVER alias user-writable memory.
     * The kernel image/BSS still lives below 16 MB; only the heap moves
     * up.  Region A's top (USER_REGION_TOP) is the 16 MB granted by
     * paging_proc_create; keep these two numbers in lock-step. */
    {
        UW after_image = ((UW)_kernel_end + 3) & ~3UL;
        UW above_ring3 = 0x01000000UL;   /* 16 MB == top of Region A */
        knl_lowmem_top = (void *)(after_image > above_ring3
                                  ? after_image : above_ring3);
    }
    knl_lowmem_limit = (void *)SYSTEMAREA_END;

    /* Initialize internal memory allocator */
    knl_init_Imalloc();

    return E_OK;
}

#if USE_CLEANUP
/*
 * knl_cpu_shutdown
 *   Called when the kernel shuts down.
 */
EXPORT void knl_cpu_shutdown(void)
{
    /* Mask all IRQs */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}
#endif /* USE_CLEANUP */

/*
 * knl_no_support - stub for unsupported T-Kernel system calls
 */
EXPORT INT knl_no_support(void *pk_para, FN fncd)
{
    (void)pk_para;
    (void)fncd;
    return E_NOSPT;
}

/* ------------------------------------------------------------------ */
/* KILL-CHURN-CRASH proof tool: dispatcher poisoned-schedtsk catch.   */
/*                                                                    */
/* The dispatcher (.Ldispatch_loop in cpu_support.S) calls this with  */
/* the about-to-run TCB's `state` byte BEFORE it loads tskctxb.ssp    */
/* and "ret"s into the task.  A healthy schedtsk is always RUNNABLE   */
/* (TS_READY); a TS_NONEXIST (== 0, the poison stamped by knl_del_tsk */
/* when a TCB returns to the FreeQue) means the kill/heal churn freed */
/* the very TCB we are about to dispatch -> the historic garbage-PC   */
/* #PF in knl_make_wait_reltim.  Converting that rare UAF into a LOUD, */
/* deterministic halt is the audit-recommended proof tool: any        */
/* occurrence prints a unique signature the gate greps for.  With the */
/* tk_del_tsk guard in place this must NEVER fire — it is the         */
/* falsification net, not the fix.                                    */
extern void print(const char *str);

static void print_hex32_local(unsigned int v)
{
    char buf[9];
    for (int i = 7; i >= 0; i--) {
        int d = (int)(v & 0xF);
        buf[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    }
    buf[8] = '\0';
    print(buf);
}

/* DIAGNOSTIC dispatch trace ring (temporary, behind KCC_TRACE). */
#ifdef KCC_TRACE
#define KCC_TRACE_N 16
volatile unsigned int kcc_trace[KCC_TRACE_N][3];  /* tid, ssp, retaddr */
volatile unsigned int kcc_trace_pos;
#endif

void knl_dispatch_poison_check(TCB *sched)
{
    if (sched == NULL) return;

#ifdef KCC_TRACE
    {
        unsigned int ssp = (unsigned int)sched->tskctxb.ssp;
        unsigned int ret = ssp ? *(volatile unsigned int *)(ssp + 20) : 0;
        unsigned int p = kcc_trace_pos % KCC_TRACE_N;
        kcc_trace[p][0] = (unsigned int)sched->tskid;
        kcc_trace[p][1] = ssp;
        kcc_trace[p][2] = ret;
        kcc_trace_pos++;
    }
#endif

    /* (1) TS_NONEXIST == 0: a TCB returned to the FreeQue by
     *     knl_del_tsk.  A freed TCB must never be a dispatch target. */
    if (sched->state == TS_NONEXIST) {
        print("\r\n[KILL-CHURN-CRASH] dispatcher reached a FREED schedtsk "
              "(TS_NONEXIST) tid=0x");
        print_hex32_local((unsigned int)sched->tskid);
        print("\r\n[kill-churn] CAUGHT\r\n");
        for (;;) { asm volatile ("cli; hlt"); }
    }

    /* (2) ssp sanity: the saved stack pointer MUST lie inside this
     *     task's own kernel stack [isstack - sstksz, isstack].  A
     *     value outside == the kill/heal churn freed+reused the stack
     *     under this TCB; the dispatcher's "ret" would jump through a
     *     corrupt/freed frame == the historic garbage-PC #PF in
     *     knl_make_wait_reltim.  Catching it HERE (before the ret)
     *     turns the rare UAF into a deterministic, grep-able halt. */
    {
        unsigned int ssp  = (unsigned int)sched->tskctxb.ssp;
        unsigned int top  = (unsigned int)sched->isstack;
        unsigned int base = top - (unsigned int)sched->sstksz;
        if (top != 0 && (ssp > top || ssp < base)) {
            print("\r\n[KILL-CHURN-CRASH] dispatcher schedtsk has OUT-OF-RANGE "
                  "ssp -- stack UAF  tid=0x");
            print_hex32_local((unsigned int)sched->tskid);
            print(" ssp=0x");   print_hex32_local(ssp);
            print(" base=0x");  print_hex32_local(base);
            print(" top=0x");   print_hex32_local(top);
            print("\r\n[kill-churn] CAUGHT\r\n");
            for (;;) { asm volatile ("cli; hlt"); }
        }
    }
}
