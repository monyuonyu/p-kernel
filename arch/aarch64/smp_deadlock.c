/*
 *  smp_deadlock.c (aarch64) — ②.2b deadlock cert  [smp-no-deadlock].
 *
 *  THE CERTIFIED FALSIFIER the smp-2b-async-preempt-plan §5.4/§7 MANDATED but
 *  that ②.2b-i did NOT deliver.  The audit found the §5.4 BKL-held reschedule
 *  guard (smp.c smp_irq_need_resched: `if (g_bkl_owner==(long)me) return 0;`)
 *  is CORRECT by reasoning but UNCERTIFIED — the auditor DELETED it and the
 *  [smp-async-preempt] cert STILL PASSED, because that cert never constructs a
 *  BKL-HELD interrupted context.  This file constructs exactly that context and
 *  proves the guard is LOAD-BEARING.
 *
 *  ────────────────────────────────────────────────────────────────────────
 *  THE SCENARIO (the §5.4 "preempt while BKL-held mid-syscall" self-deadlock,
 *  the plan's "single most likely place ②.2b self-deadlocks"):
 *
 *    L (low-prio, on CPU 1): unmasks IRQ, then ACQUIRES the BKL — i.e. ENTERS a
 *      kernel critical section — and runs a TIGHT no-poll loop INSIDE it
 *      (bumping g_dl_crit_counter to a cap).  IRQ stays UNMASKED across the
 *      critical section, so an SGI CAN land mid-critical-section (this is the
 *      whole point — the guard, not IRQ-masking, is what defers the switch).
 *      When the loop finishes, L snapshots, RELEASES the BKL, and — exactly as
 *      a real END_CRITICAL_SECTION dispatches a deferred reschedule
 *      cooperatively — if a reschedule is STILL pending it re-sends the SGI to
 *      ITSELF so the now-BKL-FREE IRQ entry runs the deferred switch.
 *
 *    H (high-prio, the preemptor): records the crit counter value AT the time
 *      it runs (proves L finished its critical section ATOMICALLY before H ran)
 *      and g_dl_highprio_ran=1; then it ACQUIRES + releases the BKL itself (the
 *      deadlock victim in the falsifier — see below) and hands the CPU back to
 *      L (cooperative knl_dispatch) so L resumes its post-critical loop.
 *
 *    Driver (CPU 0): publishes L, releases the secondary; waits until L is
 *      provably INSIDE its critical section (g_dl_in_crit==1 AND the crit
 *      counter is advancing); THEN publishes H as schedtsk and sends the SGI to
 *      CPU 1 — so the SGI is GUARANTEED to land while L holds the BKL.
 *
 *  WITH the guard (default): the SGI handler sets pending; smp_irq_need_resched
 *  sees g_bkl_owner==1 → returns 0 → DEFERS.  L's critical section completes
 *  ATOMICALLY (H did NOT run in the middle: g_dl_observed_crit == DL_CRIT_CAP),
 *  L releases the BKL, the deferred reschedule (the pending flag was KEPT, not
 *  lost) re-fires AFTER release → H runs → H acquires the BKL CLEANLY (L already
 *  released) → no deadlock.  PASS:
 *    - g_dl_highprio_ran == 1                  (the deferred reschedule fired —
 *                                               not lost)
 *    - g_dl_observed_crit == DL_CRIT_CAP       (critical section was atomic —
 *                                               H ran only AFTER it completed)
 *    - g_dl_hi_got_bkl == 1                    (H acquired the BKL — no strand)
 *    - g_dl_resumed == 1                       (L resumed after the round-trip)
 *    - smp_sgi_taken(1) >= 1                   (an SGI was delivered)
 *  → "SMP-NO-DEADLOCK: PASS".
 *
 *  WITHOUT the guard (-DSMP_NO_BKL_GUARD removes the g_bkl_owner==me clause):
 *  the SGI lands mid-critical-section, smp_irq_need_resched returns 1, the
 *  async switch fires WHILE L holds the BKL → H runs → H's bkl_acquire() spins
 *  FOREVER on the raw lock (L is suspended mid-critical-section, still owning
 *  it, and can never release because it was switched away) → DEADLOCK.  The
 *  driver's bounded watchdog catches it → "SMP-NO-DEADLOCK: FAIL".  The guard
 *  is now CERTIFIED: WITH → PASS, WITHOUT → deadlock-detected FAIL.
 *
 *  GATING: the whole TU is empty unless -DSMP_DEADLOCK_TEST (which implies
 *  -DSMP_SELFTEST).  The default build carries NONE of it → byte-identical.
 *
 *  HONESTY (inherited): QEMU TCG models memory strongly and delivers SGIs with
 *  forgiving timing; a QEMU green proves the guard defers the switch and the
 *  deferred reschedule is not lost, NOT the barrier discipline on weak silicon.
 *  The deadlock the falsifier reproduces is a CONTROL-FLOW self-deadlock
 *  (spin-forever on the raw lock), which QEMU models faithfully (it is not a
 *  memory-ordering effect) — so the falsifier bites on QEMU.
 * ───────────────────────────────────────────────────────────────────────── */

#ifdef SMP_DEADLOCK_TEST
#ifndef SMP_SELFTEST
#error "SMP_DEADLOCK_TEST requires SMP_SELFTEST (per-CPU scheduler accessors)"
#endif

#include "kernel.h"
#include "task.h"
#include "ready_queue.h"
#include "smp_percpu.h"

extern void bkl_acquire(void);
extern void bkl_release(void);
extern long smp_bringup_cpu(unsigned long cpu);
extern void smp_set_smpen_pub(void);
extern void smp_send_reschedule(int cpu);
extern void smp_gic_selftest_setup(void);
extern unsigned long smp_sgi_taken(int cpu);
extern void knl_dispatch(void);
extern void smp_gic_cpuif_init(void);

/* ── the cert state (BSS, VA==PA; observed by the driver on CPU 0) ──────── */
/* L's in-critical-section counter — bumped with the BKL HELD, NO flag-check. */
volatile unsigned long g_dl_crit_counter   = 0;
/* L's post-critical counter — bumped AFTER release (the deferred switch lands
 * here in the WITH-guard run; in the falsifier L never reaches here). */
volatile unsigned long g_dl_post_counter   = 0;
/* L sets this 1 once it is INSIDE its critical section (after bkl_acquire). */
volatile unsigned long g_dl_in_crit        = 0;
/* L sets this 1 once it has RELEASED the BKL (critical section finished). */
volatile unsigned long g_dl_released       = 0;
/* Driver sets this 1 — AFTER it has confirmed the SGI was actually TAKEN by
 * CPU 1 (sgi_taken>=1) — to let L EXIT its critical section.  This makes the
 * SGI DETERMINISTICALLY land WHILE L holds the BKL: L cannot leave the critical
 * section until the SGI has provably been delivered into the BKL-held window. */
volatile unsigned long g_dl_crit_release_ok = 0;
/* H records g_dl_crit_counter AT the instant it runs (observability). */
volatile unsigned long g_dl_observed_crit  = 0;
/* H records g_dl_released AT the instant it runs.  WITH the guard this MUST be
 * 1 — H is only switched-to AFTER L finished its critical section + released the
 * BKL → the critical section was ATOMIC (no preemption mid-section).  WITHOUT
 * the guard H runs mid-section (released==0) → but then H deadlocks below, so a
 * "ran-but-mid-section" state can never reach a PASS. */
volatile unsigned long g_dl_obs_released   = 0;
/* H sets this 1 when it runs on CPU 1 (the deferred reschedule fired). */
volatile unsigned long g_dl_highprio_ran   = 0;
/* H sets this 1 AFTER it has itself acquired+released the BKL (proves the lock
 * was NOT stranded — in the falsifier H never gets here, it deadlocks here). */
volatile unsigned long g_dl_hi_got_bkl     = 0;
/* L sets this 1 once it has RESUMED past the preempt point (round-trip done). */
volatile unsigned long g_dl_resumed        = 0;
/* the TCBs. */
volatile void *g_dl_lo_tcb = 0;
volatile void *g_dl_hi_tcb = 0;
/* the secondary go-flag (smp.c's smp_dispatch_run waits on it). */
volatile int  g_dl_secondary_go = 0;

/* The in-critical-section loop cap.  Large enough that the driver reliably
 * observes L mid-critical-section and lands the SGI inside it, small enough to
 * finish within the watchdog after the round-trip. */
#define DL_CRIT_CAP   2000000UL
#define DL_POST_CAP   2000000UL

#ifdef BOARD_RPI3
#  define DL_UART_DR  0x3F201000UL
#else
#  define DL_UART_DR  0x09000000UL
#endif
static void dl_dbg(const char *s)
{
    volatile unsigned int *dr = (volatile unsigned int *)DL_UART_DR;
    for (; *s; s++)
        *dr = (unsigned int)(unsigned char)*s;
}

/* ── L: the LOW-prio task that ENTERS a kernel critical section (holds the
 *  BKL) and spins a tight no-poll loop INSIDE it.  A REAL T-Kernel task. */
EXPORT void smp_dl_lowprio_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    g_dl_lo_tcb = (void *)CUR_CTXTSK;
    __asm__ volatile("dmb ish" ::: "memory");

    dl_dbg("[SMP] L: low-prio task RUNNING on cpu1\r\n");

    smp_gic_cpuif_init();
    __asm__ volatile("msr daifclr, #0x3; isb" ::: "memory");  /* unmask I+F */
    dl_dbg("[SMP] L: IRQ unmasked; ACQUIRING the BKL (entering crit section)\r\n");

    /* ENTER the kernel critical section: hold the BKL across the tight loop.
     * IRQs stay UNMASKED (the deadlock cert deliberately exercises the §5.4
     * guard, NOT IRQ-masking — the TOCTOU fix only masks across the tiny
     * publish/clear windows INSIDE bkl_acquire/release, not the body). */
    bkl_acquire();
    g_dl_in_crit = 1;
    __asm__ volatile("dmb st; sev" ::: "memory");
    dl_dbg("[SMP] L: BKL acquired; spinning in critical section (BKL held)\r\n");

    /* THE CRITICAL-SECTION LOOP — held under the BKL with IRQs UNMASKED.  L
     * keeps the BKL until the DRIVER (which has confirmed the SGI was actually
     * TAKEN — sgi_taken>=1) signals g_dl_crit_release_ok.  This makes the SGI
     * DETERMINISTICALLY land while L holds the BKL (no timing race): L is
     * provably still inside the critical section when the reschedule arrives.
     * The loop bumps g_dl_crit_counter (capped) purely so the driver can see L
     * is alive + mid-critical; it does NOT poll the resched flag (the §5.4 guard,
     * not L, is what must defer the switch). */
    for (;;) {
        if (g_dl_crit_counter < DL_CRIT_CAP)
            g_dl_crit_counter++;
        __asm__ volatile("dmb ld" ::: "memory");
        if (g_dl_crit_release_ok)
            break;                       /* driver confirmed SGI taken → finish */
    }

    dl_dbg("[SMP] L: critical loop done; RELEASING the BKL\r\n");
    bkl_release();
    g_dl_released = 1;
    __asm__ volatile("dsb ish" ::: "memory");

    /* COOPERATIVE deferred dispatch — exactly what a real END_CRITICAL_SECTION
     * does (smp_irq_need_resched §5.4 note: "the task's own END_CRITICAL_SECTION
     * will also dispatch it cooperatively").  If a reschedule is STILL pending
     * (the guard deferred the first SGI, keeping the flag set), re-arm the IRQ
     * entry now that the BKL is free so the deferred switch fires.  Reads the
     * pending flag the SGI handler set (one writer/one reader → dmb). */
    extern volatile unsigned int g_resched_pending[];
    __asm__ volatile("dmb ld" ::: "memory");
    if (g_resched_pending[1])
        smp_send_reschedule(1);          /* re-fire the deferred reschedule */

    /* POST-CRITICAL loop (no BKL held now).  The deferred async switch to H
     * lands HERE; when H hands back, L RESUMES this loop and finishes. */
    while (g_dl_post_counter < DL_POST_CAP) {
        g_dl_post_counter++;
        if (g_dl_highprio_ran && !g_dl_resumed) {
            g_dl_resumed = 1;
            __asm__ volatile("dmb st" ::: "memory");
        }
    }

    __asm__ volatile("dmb st; sev" ::: "memory");
    __asm__ volatile("msr daifset, #0x3; isb" ::: "memory");
    for (;;)
        __asm__ volatile("wfe");
}

/* ── H: the HIGH-prio preemptor.  Switched-to by the deferred async switch.
 *  It records the crit counter (must == cap → the critical section was atomic),
 *  then completes a dependency that L can ONLY have satisfied if it ALREADY
 *  released the BKL — which is the exact thing the §5.4 guard guarantees.  This
 *  dependency is what turns the WITHOUT-guard case into a PERMANENT DEADLOCK. */
EXPORT void smp_dl_highprio_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    g_dl_hi_tcb = (void *)CUR_CTXTSK;

    /* Snapshot, AT the instant H runs, both the crit counter (observability) and
     * whether L had already RELEASED the BKL.
     *  WITH    the guard: the switch was DEFERRED until L finished its critical
     *          section + released → g_dl_obs_released == 1 (atomic section).
     *  WITHOUT the guard: H ran mid-critical-section → g_dl_obs_released == 0
     *          (and H deadlocks below, so this never reaches a PASS). */
    g_dl_observed_crit = g_dl_crit_counter;
    g_dl_obs_released  = g_dl_released;
    g_dl_highprio_ran  = 1;
    __asm__ volatile("dmb ish; sev" ::: "memory");

    dl_dbg("[SMP] H: high-prio ran; waiting for L to have RELEASED the BKL\r\n");

    /* THE DEADLOCK HINGE.  H must not proceed until L has provably RELEASED the
     * BKL (g_dl_released == 1).
     *
     *  WITH the guard (default): H is only ever switched-to AFTER L finished its
     *  critical section, released the BKL (g_dl_released=1), and re-fired the
     *  deferred SGI.  So g_dl_released is ALREADY 1 → this wait passes instantly
     *  → no deadlock.
     *
     *  WITHOUT the guard (-DSMP_NO_BKL_GUARD): the async switch fired WHILE L
     *  held the BKL — L is now SUSPENDED mid-critical-section (g_dl_released==0)
     *  and can ONLY resume if H hands the CPU back.  But H is waiting for L to
     *  set g_dl_released, which L can only do AFTER resuming → a CYCLE → H spins
     *  here FOREVER → PERMANENT DEADLOCK → the driver's bounded watchdog catches
     *  it → SMP-NO-DEADLOCK: FAIL.  This is the certified falsifier: the §5.4
     *  guard is the ONLY thing that breaks the cycle.
     *
     *  (NO watchdog cap here ON PURPOSE: a capped spin would silently "recover"
     *  and mask the deadlock; the real hazard is an unbounded wedge, so we let
     *  the DRIVER's watchdog be the single bound.) */
    for (;;) {
        __asm__ volatile("dmb ld" ::: "memory");
        if (g_dl_released == 1)
            break;
        __asm__ volatile("yield" ::: "memory");
    }

    /* L has released the BKL → acquiring it now is CLEAN (proves no strand). */
    bkl_acquire();
    g_dl_hi_got_bkl = 1;
    __asm__ volatile("dmb st" ::: "memory");
    bkl_release();

    dl_dbg("[SMP] H: L released cleanly (no strand); handing back to L\r\n");

    /* Hand the CPU BACK to L so it RESUMES its post-critical loop. */
    bkl_acquire();
    g_smpcpu[1].schedtsk = (TCB *)g_dl_lo_tcb;
    bkl_release();
    __asm__ volatile("dsb ish" ::: "memory");

    knl_dispatch();                      /* switch back to L (never returns) */

    for (;;)
        __asm__ volatile("wfe");
}

/* Claim a started task OUT of the shared ready queue (so CPU 0 never runs it). */
static void dl_claim_for_secondary(TCB *tcb)
{
    bkl_acquire();
    knl_make_non_ready(tcb);
    tcb->state = TS_READY;
    bkl_release();
}

/* ── The driver — runs on the boot CPU (CPU 0).  Returns 0 = PASS, <0 = FAIL
 *  (a <0 from the BKL-held-switch deadlock is caught by the bounded watchdog). */
EXPORT int smp_dl_test_run(void)
{
    g_dl_crit_counter    = 0;
    g_dl_post_counter    = 0;
    g_dl_in_crit         = 0;
    g_dl_released        = 0;
    g_dl_crit_release_ok = 0;
    g_dl_observed_crit   = 0;
    g_dl_obs_released    = 0;
    g_dl_highprio_ran    = 0;
    g_dl_hi_got_bkl      = 0;
    g_dl_resumed         = 0;
    g_dl_lo_tcb          = 0;
    g_dl_hi_tcb          = 0;
    g_dl_secondary_go    = 0;
    __asm__ volatile("dsb ish" ::: "memory");

    smp_gic_selftest_setup();

    /* Create + start L (low prio, itskpri 8) and H (high prio, itskpri 4). */
    T_CTSK ctl = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                   .task = (FP)smp_dl_lowprio_task,  .itskpri = 8, .stksz = 8192 };
    ID lid = tk_cre_tsk(&ctl);
    if (lid < E_OK) return -1;
    if (tk_sta_tsk(lid, 0) < E_OK) return -2;
    TCB *ltcb = get_tcb(lid);

    T_CTSK cth = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                   .task = (FP)smp_dl_highprio_task, .itskpri = 4, .stksz = 8192 };
    ID hid = tk_cre_tsk(&cth);
    if (hid < E_OK) return -3;
    if (tk_sta_tsk(hid, 0) < E_OK) return -4;
    TCB *htcb = get_tcb(hid);

    g_dl_lo_tcb = ltcb;
    g_dl_hi_tcb = htcb;

    dl_claim_for_secondary(ltcb);
    dl_claim_for_secondary(htcb);

    /* Publish L as CPU 1's first task + release the secondary into the
     * PRODUCTION dispatcher (it switches into L, which acquires the BKL). */
    bkl_acquire();
    g_smpcpu[1].schedtsk          = ltcb;
    g_smpcpu[1].ctxtsk            = NULL;
    g_smpcpu[1].dispatch_disabled = 0;
    bkl_release();
    g_dl_secondary_go = 1;
    __asm__ volatile("dsb ish" ::: "memory");

    smp_set_smpen_pub();
    g_smpcpu[0].cpu_id = 0;
    long on = smp_bringup_cpu(1);
    if (on != 0 && on != -4 /*ALREADY_ON*/)
        return -5;

    /* Wait until L is provably INSIDE its critical section (g_dl_in_crit) AND
     * the crit counter is advancing (genuinely mid-loop, BKL held). */
    {
        const unsigned long MAX = 200000000UL;
        unsigned long tries = 0;
        const unsigned long ADVANCE = 50000UL;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");
            if (g_dl_in_crit == 1 && g_dl_crit_counter >= ADVANCE)
                break;                     /* L holds the BKL, mid-critical */
            if (++tries >= MAX)
                return -6;                 /* L never entered the crit section */
            __asm__ volatile("yield" ::: "memory");
        }
    }

    /* Publish H as CPU 1's next task and send the SGI — GUARANTEED to land
     * while L holds the BKL (mid-critical-section).  ctxtsk(L) != schedtsk(H)
     * ⇒ smp_irq_need_resched would switch — EXCEPT the §5.4 guard sees
     * g_bkl_owner==1 and DEFERS (WITH guard); WITHOUT the guard it switches →
     * deadlock.
     *
     * CRITICAL: publish schedtsk LOCK-FREE (a single 8-byte aligned store +
     * dsb).  We must NOT bkl_acquire() here — L is holding the BKL across its
     * critical section, so a bkl_acquire() on CPU 0 would BLOCK on the raw lock
     * until L releases, but L only releases AFTER we set g_dl_crit_release_ok,
     * which is BELOW this point → a driver-side deadlock unrelated to the cert.
     * The schedtsk field's only consumer is smp_irq_need_resched (clause 2),
     * which reads it under a dmb; a plain store + dsb ish is the correct
     * publish discipline for a single per-CPU field (the SGI handler / guard do
     * NOT take the BKL). */
    g_smpcpu[1].schedtsk = htcb;
    __asm__ volatile("dsb ish" ::: "memory");
    smp_send_reschedule(1);
    dl_dbg("[SMP] cpu0 sent reschedule SGI to cpu1 (L is mid-critical-section)\r\n");

    /* DETERMINISTIC handshake: wait until the SGI was provably TAKEN by CPU 1
     * (sgi_taken>=1 — the handler ran inside the BKL-held window), THEN let L
     * leave its critical section.  This removes the timing race: the reschedule
     * is GUARANTEED to have landed while L held the BKL.
     *
     *  WITH the guard: the handler set pending + bumped sgi_taken; the §5.4 guard
     *  deferred the switch and returned L to its loop → L is still spinning,
     *  BKL-held.  We now release it → L finishes + re-fires the deferred SGI.
     *
     *  WITHOUT the guard: the switch already fired the instant the SGI was taken
     *  (mid-loop) → L is SUSPENDED holding the BKL → setting crit_release_ok has
     *  no effect (L will never read it) → the deadlock below stands. */
    {
        const unsigned long MAX = 200000000UL;
        unsigned long tries = 0;
        for (;;) {
            if (smp_sgi_taken(1) >= 1)
                break;                     /* SGI provably delivered into CPU 1 */
            if (++tries >= MAX)
                return -14;                /* SGI never taken (delivery failure) */
            __asm__ volatile("yield" ::: "memory");
        }
    }
    g_dl_crit_release_ok = 1;              /* now L may leave the critical section */
    __asm__ volatile("dsb ish" ::: "memory");

    /* Bounded-wait for the full PASS evidence.  In the FALSIFIER this loop
     * TIMES OUT because H DEADLOCKED waiting for L (which is suspended
     * mid-critical-section holding the BKL) → g_dl_hi_got_bkl/resumed never set
     * → the watchdog returns -7 → SMP-NO-DEADLOCK: FAIL. */
    {
        const unsigned long MAX = 600000000UL;
        unsigned long tries = 0;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");
            if (g_dl_highprio_ran == 1 &&
                g_dl_hi_got_bkl   == 1 &&
                g_dl_resumed      == 1 &&
                g_dl_post_counter == DL_POST_CAP)
                break;                     /* full round-trip, no deadlock */
            if (++tries >= MAX)
                return -7;                 /* DEADLOCK / no deferred fire */
            __asm__ volatile("yield" ::: "memory");
        }
    }

    /* The verdict gates (main.c re-asserts these for the print). */
    if (g_dl_highprio_ran != 1)               return -8;  /* deferred reschedule lost */
    if (g_dl_obs_released != 1)               return -9;  /* crit section NOT atomic  */
    if (g_dl_hi_got_bkl != 1)                 return -10; /* BKL stranded / deadlock  */
    if (g_dl_resumed != 1)                    return -11; /* L never resumed          */
    if (g_dl_post_counter != DL_POST_CAP)     return -12; /* L never finished         */
    if (smp_sgi_taken(1) < 1)                 return -13; /* no SGI delivered         */

    return 0;                              /* PASS */
}

/* ── observability accessors (main.c reads these for the verdict print) ──── */
unsigned long smp_dl_crit(void)        { __asm__ volatile("dmb ld":::"memory"); return g_dl_crit_counter; }
unsigned long smp_dl_observed_crit(void){ __asm__ volatile("dmb ld":::"memory"); return g_dl_observed_crit; }
unsigned long smp_dl_obs_released(void) { __asm__ volatile("dmb ld":::"memory"); return g_dl_obs_released; }
unsigned long smp_dl_post(void)        { __asm__ volatile("dmb ld":::"memory"); return g_dl_post_counter; }
unsigned long smp_dl_highprio_ran(void){ __asm__ volatile("dmb ld":::"memory"); return g_dl_highprio_ran; }
unsigned long smp_dl_hi_got_bkl(void)  { __asm__ volatile("dmb ld":::"memory"); return g_dl_hi_got_bkl; }
unsigned long smp_dl_resumed(void)     { __asm__ volatile("dmb ld":::"memory"); return g_dl_resumed; }
unsigned long smp_dl_crit_cap(void)    { return DL_CRIT_CAP; }

#endif /* SMP_DEADLOCK_TEST */
