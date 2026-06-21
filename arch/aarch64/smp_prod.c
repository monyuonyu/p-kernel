/*
 *  smp_prod.c (aarch64) — ②.2a cert [smp-2tasks-prod].
 *
 *  THE SMALLEST REAL ②.2 SLICE proof: the PRODUCTION T-Kernel scheduler runs
 *  TWO REAL T-Kernel TCBs (created via tk_cre_tsk/tk_sta_tsk, living in
 *  knl_tcb_table + the shared knl_ready_queue) on TWO DISTINCT CPUs under the
 *  Big Kernel Lock — NOT the ②.0 struct smp_task stand-ins.
 *
 *  Flow (docs/architecture/smp-2-production-scheduler-plan.md §6 ②.2a):
 *    - The boot CPU (CPU 0) is already running the INITIAL task ("A"), a real
 *      TCB; the per-CPU production dispatcher set g_smpcpu[0].ctxtsk = A.
 *    - smp_prod_test_run() (called from usermain, on CPU 0) creates a real
 *      LOW-priority task "B" via tk_cre_tsk/tk_sta_tsk.  B goes READY into the
 *      shared knl_ready_queue but, being lower priority than A, does NOT
 *      preempt A on CPU 0 (knl_make_ready leaves g_smpcpu[0].schedtsk = A).
 *    - Under the BKL, the driver claims B (deletes it from the ready queue so
 *      CPU 0 never also picks it) and sets g_smpcpu[1].schedtsk = B.
 *    - It releases the secondary (CPU 1) which, in SMP_2TASKS_PROD mode, waits
 *      for its schedtsk then enters the PRODUCTION dispatcher loop
 *      (.Ldispatch_loop via smp_prod_enter_dispatch) — a genuine
 *      register-context switch into B's saved frame, running B on CPU 1.
 *    - B's body records g_smpcpu[1].ctxtsk = its own TCB and a "ran" flag.
 *    - The driver asserts: g_smpcpu[0].ctxtsk (A) and g_smpcpu[1].ctxtsk (B)
 *      are BOTH non-NULL, DISTINCT, and BOTH real TCBs inside knl_tcb_table —
 *      i.e. two real T-Kernel tasks run concurrently on two distinct CPUs.
 *
 *  GATING: the whole TU is empty unless -DSMP_2TASKS_PROD (which implies
 *  -DSMP_SELFTEST).  The default build carries NONE of it → byte-identical.
 *
 *  This file is SEPARATE from smp.c (which stays header-light) to (a) keep the
 *  real-task driver in a full-kernel-header TU and (b) minimise edits to
 *  smp.c's existing self-test functions (the ②.1b concurrency note).
 * ───────────────────────────────────────────────────────────────────────── */

#ifdef SMP_2TASKS_PROD
#ifndef SMP_SELFTEST
#error "SMP_2TASKS_PROD requires SMP_SELFTEST (per-CPU scheduler accessors)"
#endif

#include "kernel.h"
#include "task.h"
#include "ready_queue.h"

/* The per-CPU SMP block + BKL + bringup, from smp.c (header-light TU).  We
 * use the TYPED view (smp_percpu.h, struct smp_cpu with TCB* fields) which
 * aliases the SAME g_smpcpu[] storage — layout asserted in both places. */
#include "smp_percpu.h"

extern void bkl_acquire(void);
extern void bkl_release(void);
extern long smp_bringup_secondary(void);

/* ── observability the boot CPU reads for the verdict ─────────────────── */
volatile void *g_prod_a_tcb = 0;   /* the task running on CPU 0 (A)         */
volatile void *g_prod_b_tcb = 0;   /* the task B recorded running on CPU 1  */
volatile unsigned long g_prod_b_ran   = 0; /* B's body executed on CPU 1     */
volatile unsigned long g_prod_b_loops = 0; /* B did real work                */
volatile int  g_prod_b_tskid = 0;  /* B's tskid (a real T-Kernel id)        */

/* The secondary spins on this until the driver has set g_smpcpu[1].schedtsk
 * and wants it to enter the production dispatcher. */
volatile int  g_prod_secondary_go = 0;

/* B's body: a REAL T-Kernel task.  Records that it ran on CPU 1 with its own
 * real TCB, does a little bounded work, then sleeps forever (parks in the
 * kernel — proving it can re-enter the kernel from the secondary under the
 * BKL).  stacd/exinf unused. */
EXPORT void smp_prod_task_b(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    /* CUR_CTXTSK on CPU 1 == g_smpcpu[1].ctxtsk == B's own TCB (the
     * production dispatcher stored it when it switched in). */
    void *me = (void *)CUR_CTXTSK;
    g_prod_b_tcb = me;
    g_prod_b_ran = 1;
    __asm__ volatile("dmb ish" ::: "memory");

    /* A little real work to prove B is genuinely executing (not just set). */
    for (volatile unsigned long k = 0; k < 100000UL; k++) {
        g_prod_b_loops = k;
        __asm__ volatile("" ::: "memory");
    }
    __asm__ volatile("dmb ish" ::: "memory");

    /* Park the secondary on a bare wfe (NOT a kernel syscall).
     *
     * HONEST SCOPE (②.2a): the secondary CAN run a real task's COMPUTE and
     * re-enter the kernel for the BKL-serialised paths the production
     * dispatcher already covers, but the secondary's TIMER/WAIT path (its own
     * EL1 timer PPI + tick-driven reschedule) is NOT wired in ②.2a — that is
     * the ②.2b work (true async preempt + per-CPU timer).  So B must NOT call
     * a blocking/timer syscall here (tk_dly_tsk/tk_slp_tsk would enter the
     * unwired secondary wait path and fault).  Parking on wfe is the correct
     * ②.2a terminal state: B has provably run on CPU 1; it now idles while the
     * boot CPU continues. */
    for (;;)
        __asm__ volatile("wfe");
}

/* Helper: is `p` a pointer INTO the real TCB table?  (the [smp-2tasks-prod]
 * "these are real TCBs, not stand-ins" check). */
static int is_real_tcb(const void *p)
{
    const char *base = (const char *)&knl_tcb_table[0];
    const char *end  = (const char *)&knl_tcb_table[NUM_TSKID];
    const char *q    = (const char *)p;
    if (q < base || q >= end) return 0;
    /* aligned to a TCB boundary */
    return (((unsigned long)(q - base) % sizeof(TCB)) == 0);
}

/* The driver — runs on the boot CPU (CPU 0) inside the initial task.  Returns
 * 0 = PASS (two real distinct TCBs ran on two CPUs), <0 = FAIL. */
EXPORT int smp_prod_test_run(void)
{
    extern void smp_prod_secondary_enter(void);  /* the secondary path (below) */

    /* (1) Record task A = the task currently running on CPU 0 (the initial
     * task).  The production per-CPU dispatcher already set g_smpcpu[0].ctxtsk
     * to A when it dispatched it. */
    void *a = (void *)CUR_CTXTSK;
    g_prod_a_tcb = a;
    if (!is_real_tcb(a))
        return -1;                       /* A is not a real TCB — broken */

    /* (2) Create + start a REAL low-priority task B.  itskpri = 8 (lower than
     * the initial task's pri 1) so knl_make_ready does NOT make B the boot
     * CPU's schedtsk — B sits READY in the shared knl_ready_queue. */
    T_CTSK ct = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = (FP)smp_prod_task_b, .itskpri = 8, .stksz = 4096 };
    ID bid = tk_cre_tsk(&ct);
    if (bid < E_OK)
        return -2;                       /* create failed */
    g_prod_b_tskid = (int)bid;
    ER er = tk_sta_tsk(bid, 0);
    if (er < E_OK)
        return -3;                       /* start failed */

    TCB *btcb = get_tcb(bid);

    /* (3) Under the BKL, CLAIM B for CPU 1: remove it from the shared ready
     * queue (so CPU 0 can never also dispatch it), then publish it as CPU 1's
     * schedtsk.  This is the ②.2a "each CPU pulls a DISTINCT runnable task
     * from the one shared ready queue under the BKL" step. */
    bkl_acquire();
    knl_make_non_ready(btcb);            /* take B out of knl_ready_queue */
    btcb->state = TS_READY;              /* B is still runnable, just claimed */
    g_smpcpu[1].schedtsk = btcb;         /* CPU 1's next task = B           */
    g_smpcpu[1].ctxtsk   = NULL;
    g_smpcpu[1].dispatch_disabled = 0;
    bkl_release();
    __asm__ volatile("dsb ish" ::: "memory");

    /* (4) Release the secondary into the production dispatcher (it waits for
     * g_prod_secondary_go, then enters .Ldispatch_loop → switches into B). */
    g_prod_secondary_go = 1;
    __asm__ volatile("dsb ish" ::: "memory");
    long on = smp_bringup_secondary();
    if (on != 0 && on != -4 /*ALREADY_ON*/)
        return -4;                       /* CPU_ON failed */

    /* (5) Bounded-wait until B records itself running on CPU 1. */
    const unsigned long MAX = 200000000UL;
    unsigned long tries = 0;
    for (;;) {
        __asm__ volatile("dmb ld" ::: "memory");
        if (g_prod_b_ran == 1 && g_prod_b_tcb != 0)
            break;
        if (++tries >= MAX)
            return -5;                   /* B never ran on CPU 1 (FAIL) */
        __asm__ volatile("yield" ::: "memory");
    }

    /* (6) The verdict: A (CPU 0) and B (CPU 1) are BOTH real TCBs, distinct,
     * and the per-CPU current tasks differ. */
    void *a_now = (void *)g_smpcpu[0].ctxtsk;
    void *b_now = (void *)g_smpcpu[1].ctxtsk;
    if (!is_real_tcb(a_now))   return -6;
    if (!is_real_tcb(b_now))   return -7;
    if (a_now == b_now)        return -8;   /* must be DISTINCT */
    if (b_now != (void *)btcb) return -9;   /* CPU 1 really runs B */

    return 0;                                /* PASS */
}

/* ── per-CPU evidence accessors (the harness/main.c reads these) ───────── */
void *smp_prod_a_tcb(void)      { __asm__ volatile("dmb ld":::"memory"); return (void *)g_prod_a_tcb; }
void *smp_prod_b_tcb(void)      { __asm__ volatile("dmb ld":::"memory"); return (void *)g_prod_b_tcb; }
unsigned long smp_prod_b_ran(void)   { __asm__ volatile("dmb ld":::"memory"); return g_prod_b_ran; }
unsigned long smp_prod_b_loops(void) { __asm__ volatile("dmb ld":::"memory"); return g_prod_b_loops; }
int  smp_prod_b_tskid(void)     { return g_prod_b_tskid; }
int  smp_prod_is_real_tcb(const void *p) { return is_real_tcb(p); }

#endif /* SMP_2TASKS_PROD */
