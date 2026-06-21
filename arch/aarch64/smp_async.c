/*
 *  smp_async.c (aarch64) — ②.2b-i cert [smp-async-preempt].
 *
 *  THE SMALLEST REAL ②.2b-i SLICE proof: a REAL T-Kernel task running on the
 *  SECONDARY CPU is preempted ASYNCHRONOUSLY MID-COMPUTATION — it spins a
 *  tight loop that NEVER checks any flag (the load-bearing difference from
 *  ②.1a's cooperative-at-a-checkpoint preempt) — by a higher-priority task,
 *  via a GIC SGI whose IRQ-return path performs a REAL register-context switch
 *  (cpu_support.S _vec_el1_irq → smp_irq_need_resched → knl_dispatch, the
 *  ②.2b-i hook).  The low-prio task is then proven SUSPENDED MID-LOOP and
 *  later RESUMED CORRECTLY: its loop counter continues from where it was
 *  interrupted, register/PC state intact (round-tripped through ELR_EL1 /
 *  SPSR_EL1 / x0..x18, restored by .Lirq_resume_tramp).
 *
 *  Flow (driver on CPU 0):
 *    (1) create REAL tasks L (low-prio, the tight no-poll loop) and H
 *        (high-prio, the preemptor) via tk_cre_tsk/tk_sta_tsk; CLAIM both out
 *        of the shared knl_ready_queue so CPU 0 never runs them.
 *    (2) publish g_smpcpu[1].schedtsk = L and release the secondary into the
 *        PRODUCTION dispatcher (smp_prod_enter_dispatch → .Ldispatch_loop) — a
 *        genuine register-context switch into L on CPU 1.  L unmasks IRQ and
 *        spins, bumping g_async_counter every iteration, NEVER polling a flag.
 *    (3) wait until g_async_counter is observably ADVANCING (L is genuinely
 *        mid-loop), then publish g_smpcpu[1].schedtsk = H and
 *        smp_send_reschedule(1).
 *    (4) the SGI fires → _vec_el1_irq → smp_resched_sgi_handler sets pending →
 *        the ②.2b-i IRQ-RETURN HOOK (smp_irq_need_resched: ctxtsk=L != sched=H)
 *        performs the async switch: knl_dispatch SAVES L's mid-loop context
 *        (resume PC = .Lirq_resume_tramp) and RESTORES H.  NO cooperation, NO
 *        flag-check in L.
 *    (5) H runs on CPU 1: records g_async_observed_counter = g_async_counter
 *        (the value L had reached AT the instant of preemption) and
 *        g_async_highprio_ran = 1; then, under the BKL, publishes
 *        g_smpcpu[1].schedtsk = L and calls knl_dispatch() — a cooperative
 *        switch BACK to L.  knl_dispatch restores L's saved context, .Ldispatch
 *        _loop `ret`s into .Lirq_resume_tramp, which eret's L back to the exact
 *        interrupted PC.
 *    (6) L RESUMES mid-loop: its counter continues from g_async_observed_counter
 *        up to the CAP, records g_async_final_counter, then parks on wfe.
 *
 *  PASS (driver, watchdog-bounded):
 *    - g_async_highprio_ran == 1                         (H ran on CPU 1)
 *    - 0 < g_async_observed_counter < ASYNC_LOOP_CAP     (preempt landed
 *                                                         MID-loop)
 *    - g_async_final_counter == ASYNC_LOOP_CAP           (L RESUMED + finished)
 *    - g_async_final_counter > g_async_observed_counter  (counter continued)
 *    - smp_sgi_taken(1) >= 1                             (an SGI was delivered)
 *  → "SMP-ASYNC-PREEMPT: PASS".
 *
 *  FALSIFIER -DSMP_NO_ASYNC: the IRQ-return hook reverts to the ②.1a
 *  flag-set-only behaviour (smp_irq_need_resched compiled to always-return-0),
 *  so the SGI is TAKEN (sgi_taken>=1) but performs NO switch; L has no flag-
 *  check so it is NEVER preempted → H never runs → g_async_highprio_ran == 0 →
 *  watchdog → "SMP-ASYNC-PREEMPT: FAIL".  Proves the mid-loop preempt happens
 *  ONLY because of the real IRQ-return context switch.
 *
 *  GATING: the whole TU is empty unless -DSMP_ASYNC_PREEMPT (which implies
 *  -DSMP_SELFTEST).  The default build carries NONE of it → byte-identical.
 *
 *  HONESTY (inherited): QEMU TCG models memory strongly and may MASK a
 *  missing-barrier / real-SGI-timing race; the IAR-slot non-clobber +
 *  EOIR-ordering + frame-nesting on weakly-ordered silicon are only fully
 *  [live] on RPi3.  A QEMU green proves the async switch is correctly plumbed
 *  and load-bearing, NOT the barrier discipline on weak silicon.
 *
 *  SCOPE: this is ②.2b-i (the async register-context preempt) ONLY.  ②.2b-ii
 *  (the secondary's own CNTP PPI 30 tick + the cross-CPU wake gap so a
 *  secondary task can tk_dly_tsk/tk_slp_tsk and WAKE — the [smp-secondary-
 *  sleep] cert) is HONESTLY DEFERRED: it is a second, independent mechanism
 *  (per-CPU banked timer enable + making knl_make_ready IPI-aware) with its
 *  own fault surface, and the conservative call is to land the verified
 *  ②.2b-i (the named CORE / "single most C-ABI-fault-prone wave") clean rather
 *  than rush a second fault-prone path on top of it.  See the report + the
 *  smp-2b plan §4 for the ②.2b-ii design that is ready to implement next.
 * ───────────────────────────────────────────────────────────────────────── */

#ifdef SMP_ASYNC_PREEMPT
#ifndef SMP_SELFTEST
#error "SMP_ASYNC_PREEMPT requires SMP_SELFTEST (per-CPU scheduler accessors)"
#endif

#include "kernel.h"
#include "task.h"
#include "ready_queue.h"

/* The per-CPU SMP block + BKL + bringup + SGI send, from smp.c.  We use the
 * TYPED view (smp_percpu.h, struct smp_cpu with TCB* fields) which aliases the
 * SAME g_smpcpu[] storage — layout asserted in both places. */
#include "smp_percpu.h"

extern void bkl_acquire(void);
extern void bkl_release(void);
extern long smp_bringup_cpu(unsigned long cpu);
extern void smp_set_smpen_pub(void);              /* primary SMPEN (wrapper) */
extern void smp_send_reschedule(int cpu);
extern void smp_gic_selftest_setup(void);
extern unsigned long smp_sgi_taken(int cpu);
extern void knl_dispatch(void);                    /* cpu_support.S */

/* ── the cert state (BSS, VA==PA; observed by the driver on CPU 0) ──────── */
/* L's tight-loop counter — incremented with NO flag-check (the whole point). */
volatile unsigned long g_async_counter          = 0;
/* H records the counter value AT preemption (proves the preempt was MID-loop). */
volatile unsigned long g_async_observed_counter = 0;
/* L records its final counter after resuming (proves it continued + finished). */
volatile unsigned long g_async_final_counter    = 0;
/* H sets this when it runs on CPU 1 (proves the async switch reached H). */
volatile unsigned long g_async_highprio_ran     = 0;
/* L sets this once it has RESUMED past the preempt point. */
volatile unsigned long g_async_resumed          = 0;
/* the TCBs (so the driver/handler can identify ctxtsk). */
volatile void *g_async_lo_tcb = 0;
volatile void *g_async_hi_tcb = 0;
/* the secondary go-flag (smp.c's smp_dispatch_run waits on it). */
volatile int  g_async_secondary_go = 0;

/* The loop cap.  Large enough that the SGI reliably arrives MID-loop (well
 * before L finishes) — the observed preempt point was ~100k with a 100k
 * advance threshold, so a few-million cap leaves comfortable headroom — yet
 * small enough that, after RESUMING, L counts to the cap within the watchdog
 * bound.  L bumps g_async_counter exactly ASYNC_LOOP_CAP times across the
 * preempt boundary (the resume continues the SAME loop variable). */
#define ASYNC_LOOP_CAP   10000000UL    /* 10M: mid-loop preempt + fast finish */

/* Blind UART writer (QEMU virt PL011 @ 0x09000000) — either CPU can announce
 * progress (the secondary has no other console).  Blind (no FR busy-wait). */
#ifdef BOARD_RPI3
#  define ASYNC_UART_DR  0x3F201000UL
#else
#  define ASYNC_UART_DR  0x09000000UL
#endif
static void async_dbg(const char *s)
{
    volatile unsigned int *dr = (volatile unsigned int *)ASYNC_UART_DR;
    for (; *s; s++)
        *dr = (unsigned int)(unsigned char)*s;
}

/* ── L: the LOW-prio tight loop.  A REAL T-Kernel task.  Runs on CPU 1.
 *
 *  Unmask IRQ/FIQ (so the SGI can be taken asynchronously), then spin a TIGHT
 *  compute loop that increments g_async_counter and does NOTHING ELSE — NO
 *  g_resched_pending poll, NO checkpoint.  The ONLY way this loop yields the
 *  CPU is the async IRQ-return switch (cpu_support.S hook).  When L is later
 *  resumed (after H hands control back), the loop continues from wherever the
 *  preempt interrupted it — the counter value is monotonic and reaches the CAP,
 *  which is impossible unless the register/PC state round-tripped. */
EXPORT void smp_async_lowprio_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    g_async_lo_tcb = (void *)CUR_CTXTSK;     /* L's own TCB (per-CPU ctxtsk) */
    __asm__ volatile("dmb ish" ::: "memory");

    async_dbg("[SMP] L: low-prio loop task RUNNING on cpu1\r\n");

    /* Enable THIS CPU's GIC CPU interface + unmask IRQ/FIQ for the cert window
     * so the SGI can be taken asynchronously mid-loop. */
    extern void smp_gic_cpuif_init(void);
    smp_gic_cpuif_init();
    __asm__ volatile("msr daifclr, #0x3; isb" ::: "memory");  /* unmask I+F */
    async_dbg("[SMP] L: IRQ unmasked, entering tight no-poll loop\r\n");

    /* THE TIGHT NO-POLL LOOP.  volatile g_async_counter is reloaded/stored
     * each iteration; there is NO flag read.  The async preempt suspends this
     * loop MID-iteration; the resume continues the SAME loop variable. */
    while (g_async_counter < ASYNC_LOOP_CAP) {
        g_async_counter++;
        /* Mark "we got here past the preempt point" once H has run — purely
         * observational (still no resched-flag poll).  This proves L RESUMED
         * and kept counting after the async switch round-trip. */
        if (g_async_highprio_ran && !g_async_resumed) {
            g_async_resumed = 1;
            __asm__ volatile("dmb st" ::: "memory");
        }
    }

    g_async_final_counter = g_async_counter;
    __asm__ volatile("dmb st; sev" ::: "memory");

    /* Mask IRQ again + park (the driver reaps via the counters). */
    __asm__ volatile("msr daifset, #0x3; isb" ::: "memory");
    for (;;)
        __asm__ volatile("wfe");
}

/* ── H: the HIGH-prio preemptor.  A REAL T-Kernel task.  Switched-to MID-loop
 *  by the async IRQ-return hook.  It records the mid-loop counter value, then
 *  hands the CPU BACK to L (cooperative knl_dispatch) so L resumes mid-loop. */
EXPORT void smp_async_highprio_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    g_async_hi_tcb = (void *)CUR_CTXTSK;     /* H's own TCB */

    /* Record the counter value L had reached AT THE INSTANT it was preempted.
     * (0,CAP) ⇒ the preempt landed MID-loop, not before it started / after it
     * finished. */
    g_async_observed_counter = g_async_counter;
    g_async_highprio_ran     = 1;
    __asm__ volatile("dmb ish; sev" ::: "memory");

    /* Hand the CPU BACK to L so it RESUMES mid-loop.  Under the BKL publish
     * g_smpcpu[1].schedtsk = L (ctxtsk is still H → ctxtsk != schedtsk), then
     * knl_dispatch(): SAVE H's context, RESTORE L's saved (mid-loop) context.
     * knl_dispatch `ret`s into .Lirq_resume_tramp which eret's L back to the
     * exact interrupted PC — the async round-trip completes here. */
    bkl_acquire();
    g_smpcpu[1].schedtsk = (TCB *)g_async_lo_tcb;
    bkl_release();
    __asm__ volatile("dsb ish" ::: "memory");

    knl_dispatch();                          /* switch back to L (never returns) */

    /* If knl_dispatch ever returned (it must not), park. */
    for (;;)
        __asm__ volatile("wfe");
}

/* Helper: claim a started task OUT of the shared ready queue (so CPU 0 never
 * dispatches it) while keeping it runnable, exactly as smp_prod.c does. */
static void async_claim_for_secondary(TCB *tcb)
{
    bkl_acquire();
    knl_make_non_ready(tcb);                 /* remove from knl_ready_queue */
    tcb->state = TS_READY;                   /* still runnable, just claimed */
    bkl_release();
}

/* ── The driver — runs on the boot CPU (CPU 0) inside the initial task.
 *  Returns 0 = PASS (L preempted mid-loop + resumed), <0 = FAIL. */
EXPORT int smp_async_test_run(void)
{
    /* (0) Reset cert state. */
    g_async_counter          = 0;
    g_async_observed_counter = 0;
    g_async_final_counter    = 0;
    g_async_highprio_ran     = 0;
    g_async_resumed          = 0;
    g_async_lo_tcb           = 0;
    g_async_hi_tcb           = 0;
    g_async_secondary_go     = 0;
    __asm__ volatile("dsb ish" ::: "memory");

    /* (1) GIC: distributor + boot CPU interface + SGI handler in knl_intvec[0]
     * + gicc_base_ptr — BEFORE the secondary can take an SGI. */
    smp_gic_selftest_setup();

    /* (2) Create + start the two REAL tasks.
     *   L = low prio (itskpri 8) so it sits READY but does NOT preempt the
     *       boot CPU's initial task; we claim it for CPU 1.
     *   H = HIGHER prio than L (itskpri 4) — the preemptor.  Both are claimed
     *       out of the ready queue so only CPU 1's async switch runs them. */
    T_CTSK ctl = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                   .task = (FP)smp_async_lowprio_task,  .itskpri = 8, .stksz = 8192 };
    ID lid = tk_cre_tsk(&ctl);
    if (lid < E_OK) return -1;
    if (tk_sta_tsk(lid, 0) < E_OK) return -2;
    TCB *ltcb = get_tcb(lid);

    T_CTSK cth = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                   .task = (FP)smp_async_highprio_task, .itskpri = 4, .stksz = 8192 };
    ID hid = tk_cre_tsk(&cth);
    if (hid < E_OK) return -3;
    if (tk_sta_tsk(hid, 0) < E_OK) return -4;
    TCB *htcb = get_tcb(hid);

    g_async_lo_tcb = ltcb;
    g_async_hi_tcb = htcb;

    /* Claim BOTH out of the shared ready queue (CPU 0 must never run them). */
    async_claim_for_secondary(ltcb);
    async_claim_for_secondary(htcb);

    /* (3) Publish L as CPU 1's first task + release the secondary into the
     * PRODUCTION dispatcher (it switches into L, which unmasks IRQ + spins). */
    bkl_acquire();
    g_smpcpu[1].schedtsk          = ltcb;
    g_smpcpu[1].ctxtsk            = NULL;
    g_smpcpu[1].dispatch_disabled = 0;
    bkl_release();
    g_async_secondary_go = 1;
    __asm__ volatile("dsb ish" ::: "memory");

    smp_set_smpen_pub();                       /* primary SMPEN */
    g_smpcpu[0].cpu_id = 0;
    long on = smp_bringup_cpu(1);
    if (on != 0 && on != -4 /*ALREADY_ON*/)
        return -5;                             /* CPU_ON failed */

    /* (4) Wait until L is observably MID-loop (counter advancing past a small
     * threshold) — so the preempt we send lands mid-computation, not before L
     * has even started. */
    {
        const unsigned long MAX = 200000000UL;
        unsigned long tries = 0;
        const unsigned long ADVANCE = 100000UL;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");
            if (g_async_counter >= ADVANCE)
                break;                         /* L is genuinely mid-loop */
            if (++tries >= MAX)
                return -6;                     /* L never started spinning */
            __asm__ volatile("yield" ::: "memory");
        }
    }

    /* (5) Publish H as CPU 1's next task and send the reschedule SGI to CPU 1.
     * ctxtsk(L) != schedtsk(H) ⇒ smp_irq_need_resched() returns 1 ⇒ the IRQ-
     * return hook performs the async switch MID-loop. */
    bkl_acquire();
    g_smpcpu[1].schedtsk = htcb;
    bkl_release();
    __asm__ volatile("dsb ish" ::: "memory");
    smp_send_reschedule(1);

    /* (6) Bounded-wait for the full async round-trip evidence:
     *   - H ran on CPU 1 (g_async_highprio_ran)
     *   - L RESUMED and finished its CAP (g_async_final_counter == CAP)
     *   - the resume continued past the observed mid-loop value. */
    {
        const unsigned long MAX = 600000000UL;
        unsigned long tries = 0;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");
            if (g_async_highprio_ran == 1 &&
                g_async_final_counter == ASYNC_LOOP_CAP)
                break;                         /* PASS evidence complete */
            if (++tries >= MAX)
                return -7;                     /* no mid-loop preempt / no resume */
            __asm__ volatile("yield" ::: "memory");
        }
    }

    /* (7) The verdict gates (the driver/main.c re-asserts these too). */
    if (g_async_highprio_ran != 1)                       return -8;
    if (g_async_observed_counter == 0)                   return -9;  /* before loop */
    if (g_async_observed_counter >= ASYNC_LOOP_CAP)      return -10; /* after loop */
    if (g_async_final_counter != ASYNC_LOOP_CAP)         return -11; /* never finished */
    if (g_async_final_counter <= g_async_observed_counter) return -12; /* didn't continue */
    if (smp_sgi_taken(1) < 1)                            return -13; /* no SGI delivered */

    return 0;                                  /* PASS */
}

/* ── observability accessors (main.c reads these for the verdict print) ──── */
unsigned long smp_async_counter(void)          { __asm__ volatile("dmb ld":::"memory"); return g_async_counter; }
unsigned long smp_async_observed(void)         { __asm__ volatile("dmb ld":::"memory"); return g_async_observed_counter; }
unsigned long smp_async_final(void)            { __asm__ volatile("dmb ld":::"memory"); return g_async_final_counter; }
unsigned long smp_async_highprio_ran(void)     { __asm__ volatile("dmb ld":::"memory"); return g_async_highprio_ran; }
unsigned long smp_async_loop_cap(void)         { return ASYNC_LOOP_CAP; }

#endif /* SMP_ASYNC_PREEMPT */
