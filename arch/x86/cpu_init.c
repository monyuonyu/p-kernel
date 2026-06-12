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

    /* Set up kernel memory area for T-Kernel imalloc
     * lowmem_top: just after kernel image, aligned to 4-byte boundary
     * lowmem_limit: SYSTEMAREA_END (64MB)
     *
     * KILL-CHURN-CRASH investigation note (gap-ledger): an earlier
     * hypothesis blamed an EXTERNAL ring3 clobber of kernel task stacks
     * — imalloc hands out task stacks just above _kernel_end (~0xD8xxxx),
     * which overlaps the x86 ring3-writable Region A (0x400000–0xFFFFFF,
     * see arch/x86/paging.c) — and floated this base above 16 MB to
     * dodge it.  That was DISPROVEN by the closure stress: relocating the
     * heap moved the faulting stacks 0x00D8xxxx -> 0x0100xxxx but left the
     * ~42% fault rate unchanged, because the real corruption lives in the
     * GLOBAL timer LINKED LIST (a freed TCB's still-armed wtmeb), not at a
     * fixed VA.  The real fix is in kernel/common/task_manage.c
     * (knl_del_tsk now unlinks tcb->wtmeb before freeing).  The base is
     * therefore left at master's value.  (A genuine-but-separate hardening
     * — kernel stacks should not alias ring3 memory — is deferred so this
     * row's diff is exactly the proven cure.) */
    knl_lowmem_top   = (void *)(((UW)_kernel_end + 3) & ~3UL);
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
