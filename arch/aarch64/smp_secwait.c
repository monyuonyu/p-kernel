/*
 *  smp_secwait.c (aarch64) — ②.2b-ii cert [smp-secondary-wait].
 *
 *  THE ②.2b-ii SLICE proof: a REAL T-Kernel task running on the SECONDARY
 *  (CPU 1) can BLOCK and WAKE — proving (A) the secondary programs + takes its
 *  OWN EL1 generic-timer interrupt (CNTP PPI 30) and (B) the cross-CPU
 *  wake-dispatch gap in knl_make_ready is closed.  Two halves:
 *
 *    (i)  SELF-TIMER WAKE: a task Bdly on CPU 1 calls tk_dly_tsk(DLY_MS) and is
 *         woken BY CPU 1's OWN timer tick.  NON-VACUITY (design §1.3): the
 *         driver on CPU 0 holds its OWN timer service OUT of the window — it
 *         polls with IRQ MASKED (disint), so CPU 0's knl_timer_handler can NEVER
 *         walk the shared timer queue during the measurement.  The ONLY core
 *         that can expire Bdly's wtmeb is CPU 1.  The POSITIVE WITNESS is
 *         knl_current_time advancing (it advances ONLY inside knl_timer_handler,
 *         timer.c:184) — so a PASS literally cannot be claimed unless CPU 1 took
 *         at least DLY_MS/CFN_TIMER_PERIOD of its OWN ticks.  Under
 *         -DSMP_NO_SEC_TIMER the secondary's CNTP is unprogrammed → no core
 *         ticks → knl_current_time never advances → Bdly hangs → FAIL.
 *
 *    (ii) CROSS-CPU WAKE: a task Bsem on CPU 1 blocks on tk_wai_sem(sid, 1,
 *         TMO_FEVR) — an INFINITE wait, so NO timer can wake it; ONLY a signal
 *         can.  The driver on CPU 0 then tk_sig_sem(sid, 1).  The wake path
 *         (tk_sig_sem → knl_wait_release_ok → knl_make_ready →
 *         knl_smp_wake_hook(Bsem) → knl_smp_wake: g_smpcpu[1].schedtsk = Bsem +
 *         smp_send_reschedule(1) → the SGI's IRQ-return hits the ②.2b-i async
 *         switch → CPU 1 re-dispatches Bsem).  Because TMO_FEVR makes the waiter
 *         IMPOSSIBLE to wake by any tick, the only thing that can pass half (ii)
 *         is the REAL cross-CPU signal+IPI path.  Under -DSMP_NO_XWAKE the IPI
 *         is suppressed → CPU 1 is never told to re-dispatch → Bsem never wakes
 *         → FAIL.
 *
 *  PASS (driver, watchdog-bounded) → "SMP-SECONDARY-WAIT: PASS":
 *    (i)  g_sec_slept==1 && g_sec_woke_i==1 && (woke_time - slept_time) >= DLY
 *    (ii) g_sem_blocked==1 && g_sem_woke==1 && smp_sgi_taken(1) >= 1
 *
 *  GATING: the whole TU is empty unless -DSMP_SECONDARY_WAIT (which implies
 *  -DSMP_SELFTEST).  Its object is in SMP_CERT_EXCLUDE so the DEFAULT build
 *  never links it → byte-identical.
 *
 *  HONESTY (inherited): QEMU TCG models memory strongly and may MASK a missing
 *  barrier / real SGI-or-tick-latency race; the teeth (barrier discipline on
 *  weakly-ordered silicon, RPi3's BCM2837 per-core timer/mailbox) are only
 *  [live] on RPi3.  A QEMU green proves the secondary timer + cross-CPU wake
 *  are correctly plumbed and LOAD-BEARING, not the weak-memory discipline.
 *
 *  This file is SEPARATE from smp.c (which stays header-light) to keep the
 *  real-task driver in a full-kernel-header TU, mirroring smp_async.c.
 * ───────────────────────────────────────────────────────────────────────── */

#ifdef SMP_SECONDARY_WAIT
#ifndef SMP_SELFTEST
#error "SMP_SECONDARY_WAIT requires SMP_SELFTEST (per-CPU scheduler accessors)"
#endif

#include "kernel.h"
#include "task.h"
#include "ready_queue.h"
#include "timer.h"          /* knl_current_time (the §1.3 positive witness) */
#include "cpu_insn.h"       /* disint()/enaint() — hold CPU 0's timer out (§1.3) */

/* The per-CPU SMP block + BKL + bringup + SGI send, from smp.c (header-light
 * TU).  Use the TYPED view (smp_percpu.h, struct smp_cpu with TCB* fields)
 * aliasing the SAME g_smpcpu[] storage — layout asserted in both places. */
#include "smp_percpu.h"

extern void bkl_acquire(void);
extern void bkl_release(void);
extern long smp_bringup_cpu(unsigned long cpu);
extern void smp_set_smpen_pub(void);              /* primary SMPEN (wrapper) */
extern void smp_gic_selftest_setup(void);
extern void smp_send_reschedule(int cpu);
extern unsigned long smp_sgi_taken(int cpu);

/* The cross-CPU wake arming hooks (smp.c): knl_smp_wake is INERT unless armed
 * for a specific target task — so only THIS cert's half (ii) drives the
 * cross-CPU IPI; every other SMP_SELFTEST path leaves it dormant. */
extern volatile unsigned long  g_xwake_armed;
extern volatile void          *g_xwake_tcb;
extern volatile int            g_xwake_target;

/* ── cert state (BSS, VA==PA; observed by the driver on CPU 0) ──────────── */
/* Half (i): self-timer wake. */
volatile unsigned long g_sec_slept       = 0;  /* Bdly recorded "about to dly" */
volatile unsigned long g_sec_woke_i      = 0;  /* Bdly woke from the dly       */
volatile long          g_sec_slept_time  = 0;  /* knl_current_time before dly  */
volatile long          g_sec_woke_time   = 0;  /* knl_current_time after wake  */
volatile int           g_sec_dly_ercd    = 1;  /* tk_dly_tsk return code (E_OK=0)*/
/* Half (ii): cross-CPU wake. */
volatile unsigned long g_sem_blocked     = 0;  /* Bsem blocked on the sem      */
volatile unsigned long g_sem_woke        = 0;  /* Bsem woke from the sem       */
volatile int           g_sem_wai_ercd    = 1;  /* tk_wai_sem return code        */
/* the secondary go-flag (smp.c's smp_dispatch_run waits on it). */
volatile int  g_secwait_secondary_go = 0;
/* the TCBs / sem id the driver publishes. */
volatile void *g_sec_bdly_tcb = 0;
volatile void *g_sec_bsem_tcb = 0;
volatile int   g_sec_sem_id   = 0;

/* The delay, in ms.  CFN_TIMER_PERIOD is 10 ms/tick (TIMER_HZ=100), so this is
 * ~5 ticks of CPU 1's OWN timer — comfortably observable, fast within the
 * watchdog. */
#define SEC_DLY_MS   50

/* Blind UART writer (QEMU virt PL011 @ 0x09000000) — the secondary has no
 * other console. */
#ifdef BOARD_RPI3
#  define SECW_UART_DR  0x3F201000UL
#else
#  define SECW_UART_DR  0x09000000UL
#endif
static void secw_dbg(const char *s)
{
    volatile unsigned int *dr = (volatile unsigned int *)SECW_UART_DR;
    for (; *s; s++)
        *dr = (unsigned int)(unsigned char)*s;
}

/* ── Bdly: half (i).  A REAL T-Kernel task.  Runs on CPU 1.  Sleeps via
 *  tk_dly_tsk and must be woken by CPU 1's OWN tick. ─────────────────────── */
EXPORT void smp_secwait_dly_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    g_sec_bdly_tcb = (void *)CUR_CTXTSK;
    secw_dbg("[SMP] Bdly: dly task RUNNING on cpu1\r\n");

    /* Record the system time BEFORE the delay (the §1.3 positive witness:
     * knl_current_time advances ONLY when a tick runs knl_timer_handler). */
    g_sec_slept_time = (long)knl_current_time;
    g_sec_slept = 1;
    __asm__ volatile("dmb st; sev" ::: "memory");

    secw_dbg("[SMP] Bdly: tk_dly_tsk (only cpu1's OWN tick can wake me)...\r\n");
    ER er = tk_dly_tsk((RELTIM)SEC_DLY_MS);   /* BLOCK on the secondary */

    /* Woke up — record evidence.  This line only runs if CPU 1's timer tick
     * walked the shared queue and expired our wtmeb. */
    g_sec_dly_ercd  = (int)er;
    g_sec_woke_time = (long)knl_current_time;
    g_sec_woke_i    = 1;
    __asm__ volatile("dmb st; sev" ::: "memory");
    secw_dbg("[SMP] Bdly: WOKE from tk_dly_tsk (cpu1's own tick fired)\r\n");

    /* §LENS-B(d) — knl_taskindp is a single GLOBAL W (NOT per-CPU), and the asm
     * timer-startup shim brackets it with knl_taskindp++/-- OUTSIDE the BKL.  If
     * CPU 1 kept ticking during half (ii) (where CPU 0 also ticks), the two
     * startup shims could overlap → a lost-update race on the global.  We make
     * the two CPUs' task-independent brackets PROVABLY NON-OVERLAPPING by
     * disabling CPU 1's OWN CNTP here (half (i) is done; half (ii) is the
     * cross-CPU SIGNAL path and needs NO CPU-1 tick).  This is option (b) of the
     * design §7.5 / LENS-B(d): the global knl_taskindp per-CPU-ization is
     * ledgered as a ②.3 sharpening; this cert closes the window by construction.
     * CNTP_CTL_EL0 is per-CPU-banked, so this masks ONLY CPU 1's timer. */
    __asm__ volatile("msr cntp_ctl_el0, %0; isb" :: "r"((unsigned long)0));

    for (;;)
        __asm__ volatile("wfe");                 /* park; driver reaps */
}

/* ── Bsem: half (ii).  A REAL T-Kernel task.  Runs on CPU 1.  Blocks forever on
 *  a semaphore that ONLY CPU 0's tk_sig_sem can release (cross-CPU wake). ─── */
EXPORT void smp_secwait_sem_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    g_sec_bsem_tcb = (void *)CUR_CTXTSK;
    secw_dbg("[SMP] Bsem: sem task RUNNING on cpu1\r\n");

    g_sem_blocked = 1;
    __asm__ volatile("dmb st; sev" ::: "memory");

    secw_dbg("[SMP] Bsem: tk_wai_sem(TMO_FEVR) (only cpu0's sig can wake me)...\r\n");
    ER er = tk_wai_sem(g_sec_sem_id, 1, TMO_FEVR);   /* BLOCK forever */

    /* Woke up — only the cross-CPU signal+IPI path can have done this (TMO_FEVR
     * means no tick can mask the cross-CPU path). */
    g_sem_wai_ercd = (int)er;
    g_sem_woke     = 1;
    __asm__ volatile("dmb st; sev" ::: "memory");
    secw_dbg("[SMP] Bsem: WOKE from tk_wai_sem (cpu0's cross-CPU wake fired)\r\n");

    for (;;)
        __asm__ volatile("wfe");                 /* park; driver reaps */
}

/* Helper: claim a started task OUT of the shared ready queue (so CPU 0 never
 * dispatches it) while keeping it runnable, exactly as smp_async.c does. */
static void secw_claim_for_secondary(TCB *tcb)
{
    bkl_acquire();
    knl_make_non_ready(tcb);                 /* remove from knl_ready_queue */
    tcb->state = TS_READY;                   /* still runnable, just claimed */
    bkl_release();
}

/* Release CPU 1 into the production dispatcher with `tcb` as its first task. */
static void secw_release_secondary(TCB *tcb)
{
    bkl_acquire();
    g_smpcpu[1].schedtsk          = tcb;
    g_smpcpu[1].ctxtsk            = NULL;
    g_smpcpu[1].dispatch_disabled = 0;
    bkl_release();
    g_secwait_secondary_go = 1;
    __asm__ volatile("dsb ish" ::: "memory");
}

/* ── The driver — runs on the boot CPU (CPU 0) inside the initial task.
 *  Returns 0 = PASS, <0 = FAIL (the code identifies which half/step). ────── */
EXPORT int smp_secwait_test_run(void)
{
    /* (0) Reset cert state. */
    g_sec_slept = g_sec_woke_i = 0;
    g_sec_slept_time = g_sec_woke_time = 0;
    g_sec_dly_ercd = 1;
    g_sem_blocked = g_sem_woke = 0;
    g_sem_wai_ercd = 1;
    g_secwait_secondary_go = 0;
    g_xwake_armed = 0;
    __asm__ volatile("dsb ish" ::: "memory");

    /* (1) GIC: distributor + boot CPU interface + SGI handler in knl_intvec[0]
     * + gicc_base_ptr — BEFORE the secondary can take an SGI. */
    smp_gic_selftest_setup();

    /* §LENS-C shared-ready-queue note: the shipped ② slice has ONE shared
     * knl_ready_queue (per-CPU run-queues are ②.3 deferred).  CPU 0's initial
     * task (this driver) is the TOP of that queue.  When the secondary's task
     * BLOCKS (tk_dly_tsk / tk_wai_sem), knl_make_non_ready re-selects
     * knl_ready_queue_top for CPU 1 — which would return CPU 0's RUNNING initial
     * task and make CPU 1 try to switch into a context LIVE on CPU 0 (stale SP →
     * wild jump).  The fix is the SMP_SELFTEST-only guard in .Ldispatch_loop
     * (cpu_support.S): a secondary NEVER switches into a TCB that is already
     * another CPU's live ctxtsk — it idles instead.  No de-queue of CPU 0's
     * running task (which would corrupt CPU 0's own scheduling). */
    int rc = 0;

    /* ───────────────────────── HALF (i): self-timer wake ──────────────── */
    /* (2) Create + start Bdly (low prio so it does NOT preempt the boot CPU's
     * initial task), claim it for CPU 1, release CPU 1 (which programs its OWN
     * CNTP in smp_dispatch_run, then switches into Bdly). */
    T_CTSK ctd = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                   .task = (FP)smp_secwait_dly_task, .itskpri = 8, .stksz = 8192 };
    ID did = tk_cre_tsk(&ctd);
    if (did < E_OK)               { rc = -1; goto done; }
    if (tk_sta_tsk(did, 0) < E_OK){ rc = -2; goto done; }
    TCB *dtcb = get_tcb(did);
    g_sec_bdly_tcb = dtcb;
    secw_claim_for_secondary(dtcb);

    /* Arm the directed wake for Bdly → CPU 1.  Bdly is LOWER priority than
     * CPU 0's running initial task, so the shared knl_ready_queue's top is
     * always cpu0_tsk; when CPU 1's OWN timer tick readies Bdly, knl_make_ready
     * will NOT make Bdly CPU 1's schedtsk by priority (it doesn't outrank
     * cpu0_tsk).  The armed knl_smp_wake EXPLICITLY publishes
     * g_smpcpu[1].schedtsk = Bdly (SELF-wake: target==me==1, no IPI) so CPU 1
     * re-dispatches Bdly after the tick.  This is the same directed mechanism
     * half (ii) uses cross-CPU. */
    g_xwake_tcb    = dtcb;
    g_xwake_target = 1;
    __asm__ volatile("dsb ish" ::: "memory");
    g_xwake_armed  = 1;
    __asm__ volatile("dsb ish" ::: "memory");

    secw_release_secondary(dtcb);

    smp_set_smpen_pub();
    g_smpcpu[0].cpu_id = 0;
    long on = smp_bringup_cpu(1);
    if (on != 0 && on != -4 /*ALREADY_ON*/) { rc = -3; goto done; }

    /* (3) Wait until Bdly has recorded it is ABOUT to sleep (g_sec_slept). */
    {
        const unsigned long MAX = 200000000UL;
        unsigned long tries = 0;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");
            if (g_sec_slept)
                break;
            if (++tries >= MAX) { rc = -4; goto done; }  /* Bdly never ran */
            __asm__ volatile("yield" ::: "memory");
        }
    }

    /* (4) NON-VACUITY (§1.3): poll for Bdly's wake with CPU 0's IRQ MASKED, so
     * CPU 0's knl_timer_handler can NEVER walk the shared timer queue during the
     * window — the ONLY core that can expire Bdly's wtmeb is CPU 1.  We mask
     * with disint() (IRQ+FIQ) and poll the volatile flag (no kernel call), then
     * restore.  knl_current_time advancing is the positive witness that CPU 1's
     * OWN tick fired (it advances only inside knl_timer_handler). */
    {
        UINT imask = disint();                 /* hold CPU 0's timer OUT */
        const unsigned long MAX = 600000000UL;
        unsigned long tries = 0;
        int timed_out = 0;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");
            if (g_sec_woke_i)
                break;
            if (++tries >= MAX) { timed_out = 1; break; }
            __asm__ volatile("yield" ::: "memory");
        }
        enaint(imask);                         /* restore CPU 0's timer */
        if (timed_out) { rc = -5; goto done; } /* Bdly never woke (no sec timer?) */
    }

    /* (5) Half (i) verdict gates. */
    if (g_sec_slept  != 1)            { rc = -6; goto done; }
    if (g_sec_woke_i != 1)            { rc = -7; goto done; }
    if (g_sec_dly_ercd != 0 /*E_OK*/) { rc = -8; goto done; } /* dly error */
    {
        /* knl_current_time advanced by at least the requested delay — driven
         * SOLELY by CPU 1's ticks (CPU 0's timer was masked out of the window). */
        long delta = g_sec_woke_time - g_sec_slept_time;
        if (delta < (long)SEC_DLY_MS) { rc = -9; goto done; } /* tick witness */
    }

    /* ───────────────────────── HALF (ii): cross-CPU wake ──────────────── */
    /* (6) Create a semaphore (count 0) + Bsem (low prio).  ARM the cross-CPU
     * wake for Bsem → CPU 1, then run Bsem on CPU 1 (re-using the dispatcher:
     * Bdly has parked on wfe, so CPU 1 is free to switch to Bsem). */
    {
        T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
        ID sid = tk_cre_sem(&cs);
        if (sid < E_OK) { rc = -10; goto done; }
        g_sec_sem_id = (int)sid;

        T_CTSK cts = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                       .task = (FP)smp_secwait_sem_task, .itskpri = 8, .stksz = 8192 };
        ID sid_t = tk_cre_tsk(&cts);
        if (sid_t < E_OK)               { rc = -11; goto done; }
        if (tk_sta_tsk(sid_t, 0) < E_OK){ rc = -12; goto done; }
        TCB *stcb = get_tcb(sid_t);
        g_sec_bsem_tcb = stcb;
        secw_claim_for_secondary(stcb);

        /* Arm the directed cross-CPU wake: a knl_make_ready(stcb) on ANY CPU now
         * publishes g_smpcpu[1].schedtsk = stcb + IPIs CPU 1. */
        g_xwake_tcb    = stcb;
        g_xwake_target = 1;
        __asm__ volatile("dsb ish" ::: "memory");
        g_xwake_armed  = 1;
        __asm__ volatile("dsb ish" ::: "memory");

        /* Hand Bsem to CPU 1 + re-release it (Bdly has parked, so schedtsk=Bsem). */
        bkl_acquire();
        g_smpcpu[1].schedtsk = stcb;
        bkl_release();
        __asm__ volatile("dsb ish" ::: "memory");
        smp_send_reschedule(1);                /* nudge CPU 1 to pick up Bsem */

        /* (7) Wait until Bsem has BLOCKED on the semaphore. */
        {
            const unsigned long MAX = 200000000UL;
            unsigned long tries = 0;
            for (;;) {
                __asm__ volatile("dmb ld" ::: "memory");
                if (g_sem_blocked)
                    break;
                if (++tries >= MAX) { rc = -13; goto done; } /* never blocked */
                __asm__ volatile("yield" ::: "memory");
            }
        }
        /* Give it a moment to fully enter TS_WAIT before signalling. */
        {
            unsigned long spin = 0;
            for (; spin < 2000000UL; spin++)
                __asm__ volatile("yield" ::: "memory");
        }

        /* (8) THE CROSS-CPU WAKE: CPU 0 signals the semaphore.  tk_sig_sem →
         * knl_wait_release_ok → knl_make_ready → knl_smp_wake(Bsem) →
         * g_smpcpu[1].schedtsk=Bsem + smp_send_reschedule(1) (unless NO_XWAKE).
         * Bsem re-dispatches on CPU 1 via the ②.2b-i async hook. */
        secw_dbg("[SMP] cpu0: tk_sig_sem (cross-CPU wake of Bsem on cpu1)...\r\n");
        ER ser = tk_sig_sem(sid, 1);
        if (ser != E_OK) { rc = -14; goto done; }  /* signal itself failed */

        /* (9) Bounded-wait for Bsem to wake (the cross-CPU path). */
        {
            const unsigned long MAX = 600000000UL;
            unsigned long tries = 0;
            for (;;) {
                __asm__ volatile("dmb ld" ::: "memory");
                if (g_sem_woke)
                    break;
                if (++tries >= MAX) { rc = -15; goto done; } /* never woke */
                __asm__ volatile("yield" ::: "memory");
            }
        }
    }

    /* (10) Half (ii) verdict gates. */
    if (g_sem_blocked  != 1)            { rc = -16; goto done; }
    if (g_sem_woke     != 1)            { rc = -17; goto done; }
    if (g_sem_wai_ercd != 0 /*E_OK*/)   { rc = -18; goto done; } /* wai error */
    if (smp_sgi_taken(1) < 1)           { rc = -19; goto done; } /* no SGI */

done:
    /* Disarm the cross-CPU wake so no later knl_make_ready (the normal boot that
     * follows) fires a stray directed IPI. */
    g_xwake_armed = 0;
    __asm__ volatile("dsb ish" ::: "memory");
    return rc;                                  /* 0 = PASS (both halves) */
}

/* ── observability accessors (usermain.c reads these for the verdict print) ── */
unsigned long smp_secwait_slept(void)      { __asm__ volatile("dmb ld":::"memory"); return g_sec_slept; }
unsigned long smp_secwait_woke_i(void)     { __asm__ volatile("dmb ld":::"memory"); return g_sec_woke_i; }
long          smp_secwait_dly_delta(void)  { __asm__ volatile("dmb ld":::"memory"); return g_sec_woke_time - g_sec_slept_time; }
unsigned long smp_secwait_sem_blocked(void){ __asm__ volatile("dmb ld":::"memory"); return g_sem_blocked; }
unsigned long smp_secwait_sem_woke(void)   { __asm__ volatile("dmb ld":::"memory"); return g_sem_woke; }
unsigned long smp_secwait_dly_ms(void)     { return (unsigned long)SEC_DLY_MS; }

#endif /* SMP_SECONDARY_WAIT */
