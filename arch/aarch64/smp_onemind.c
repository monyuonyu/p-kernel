/*
 *  smp_onemind.c (aarch64) — ②.2c CROWN cert [smp-one-mind].
 *
 *  THE PAYOFF of the entire ② full-SMP arc: prove a REAL mind forward
 *  (r_forward, the bare-metal R3 in-context Transformer, arch/common/
 *  r3_incontext.c) is BYTE-IDENTICAL whether it runs (a) under the shipped
 *  uniprocessor path or (b) as a real T-Kernel task ON A SECONDARY CPU under
 *  the live SMP scheduler. H_uni == H_smp ⇒ the SMP scheduler did not perturb
 *  a single bit of the mind's math. "The mind stays one across the SMP
 *  scheduler."
 *
 *  This REUSES the ②.2a smp_prod pattern verbatim (create a real tk_cre_tsk
 *  task, claim it for CPU 1 under the BKL, release the secondary into the
 *  production dispatcher, join). The ONLY new thing vs ②.2a: the task body
 *  computes r3_onemind_forward_hash() (a fixed-seed/fixed-input r_forward + an
 *  FNV-1a hash of its output) instead of a bare counter.
 *
 *  Flow (docs/architecture/smp-2c-one-mind-plan.md §2):
 *    Leg (a) H_uni: the driver (CPU 0) calls r3_onemind_forward_hash() DIRECTLY
 *      — the shipped uniprocessor reference.
 *    Leg (b) H_smp: the driver creates a REAL low-prio mind task M, claims it
 *      for CPU 1 under the BKL (knl_make_non_ready + g_smpcpu[1].schedtsk = M),
 *      releases CPU 1 into the production dispatcher (it register-context
 *      switches into M → r_forward runs on CPU 1), and joins on M's "ran" flag.
 *      The OTHER secondaries run a busy filler so the scheduler is provably
 *      driving multiple cores while CPU 1 computes the mind (§2.2 step 4).
 *    Verdict: ASSERT H_uni == H_smp (BYTE-IDENTICAL).
 *
 *  THE HONEST NARROWING (§3, load-bearing — stated in the cert + the verdict):
 *    The crown proves a SINGLE mind forward, scheduled on an SMP secondary while
 *    other cores run concurrently, is byte-identical to the same forward run
 *    uniprocessor. It does NOT prove CONCURRENT mind operations (two forwards at
 *    once, or forward-while-train) — those race the SHARED module-static rc/rw[]
 *    and need a mind-lock, DEFERRED. ②.2c runs the mind on EXACTLY ONE CPU at a
 *    time. The -DSMP_ONEMIND_RACE falsifier deliberately violates that (a 2nd CPU
 *    scribbles the shared rc/rw[] while M's forward is in-flight) → H_smp != H_uni
 *    → SMP-ONE-MIND: FAIL — proving the cert observes the REAL output AND that the
 *    single-forward discipline is load-bearing, not vacuously passing.
 *
 *  ②.2b-ii is NOT needed: M is a pure run-to-completion COMPUTE task (r_forward
 *  is a bounded loop nest over fixed dims). It computes the hash, records it,
 *  then parks on wfe — it never calls a blocking/timer syscall, so the secondary
 *  timer/WAIT path ([smp-secondary-sleep], deferred) is never exercised. ②.2c
 *  sits on the ②.2a pattern; it does not depend on ②.2b-i's async preempt either
 *  (M is never preempted — it runs straight through).
 *
 *  HONESTY (QEMU vs HW): a QEMU -smp 4 PASS proves the scheduler did not
 *  reorder/corrupt the mind's math under real concurrency (the determinism). It
 *  does NOT prove the BKL/SGI barrier/cache-coherency discipline on weakly-
 *  ordered silicon (QEMU TCG models memory strongly) — that is [live]-only on
 *  RPi3. The mind MATH (r_forward) is UNTOUCHED: ②.2c only SCHEDULES it + hashes
 *  its output.
 *
 *  GATING: the whole TU is empty unless -DSMP_ONE_MIND (which implies
 *  -DSMP_SELFTEST). The default build carries NONE of it → byte-identical (the
 *  .o is excluded from the LINK via SMP_CERT_EXCLUDE, like smp_prod.o).
 * ───────────────────────────────────────────────────────────────────────── */

#ifdef SMP_ONE_MIND
#ifndef SMP_SELFTEST
#error "SMP_ONE_MIND requires SMP_SELFTEST (per-CPU scheduler accessors)"
#endif

#include "kernel.h"
#include "task.h"
#include "ready_queue.h"
#include "smp_percpu.h"

extern void bkl_acquire(void);
extern void bkl_release(void);
extern long smp_bringup_secondary(void);

/* The crown forward+hash entry point (arch/common/r3_incontext.c, gated behind
 * -DSMP_ONE_MIND): runs ONE fixed-seed/fixed-input r_forward + FNV-1a hashes its
 * output. PURE — independent of any prior state; byte-identical on any CPU. */
extern unsigned long r3_onemind_forward_hash(void);

/* ── observability the boot CPU reads for the verdict ─────────────────── */
volatile unsigned long g_om_h_uni = 0;   /* uniprocessor reference hash (CPU 0) */
volatile unsigned long g_om_h_smp = 0;   /* SMP hash: forward ran on CPU 1       */
volatile void         *g_om_m_tcb = 0;   /* M's TCB recorded running on CPU 1    */
volatile unsigned long g_om_m_ran = 0;   /* M's body executed on CPU 1           */
volatile int           g_om_m_tskid = 0; /* M's tskid (a real T-Kernel id)       */

/* released by the driver to enter the prod dispatcher (smp.c reads these). */
volatile int           g_onemind_secondary_go = 0;
volatile int           g_onemind_racer_go     = 0;
/* FALSIFIER-ONLY (-DSMP_ONEMIND_RACE): "M's forward is mid-flight" flag. Set
 * to 1 by r3_onemind_forward_hash() immediately BEFORE r_forward (after the
 * deterministic re-seed) and back to 0 immediately AFTER, so it brackets EXACTLY
 * the read window the racer must corrupt. The racer (smp.c, CPU 2) scribbles the
 * shared rc/rw[] in small chunks GATED on this flag — continuously active across
 * the whole forward, every boot — instead of a fixed one-shot burst that QEMU's
 * nondeterministic secondary bring-up could land entirely before/after the read
 * window (the spurious-PASS the audit caught). Cross-CPU volatile is how this
 * cert already coordinates (g_onemind_secondary_go / g_onemind_racer_go); MMU is
 * OFF (VA==PA). In the non-race build this symbol is never read (the racer code
 * and the set/clear are both #ifdef SMP_ONEMIND_RACE), so the guard'd PASS build
 * is unaffected. */
volatile unsigned long g_om_forward_inflight = 0;
/* per-CPU busy filler counters (the §2.2 step-4 concurrency proof; smp.c
 * increments g_onemind_filler[me] on the non-mind secondaries). */
volatile unsigned long g_onemind_filler[SMP_MAX_CPUS] = { 0 };

/* M's body: a REAL T-Kernel task. Records it ran on CPU 1 with its own real
 * TCB, computes the crown forward hash into g_om_h_smp, then parks on a bare wfe
 * (NOT a kernel syscall — the ②.2a discipline; M must not enter the unwired
 * secondary timer/WAIT path). r_forward is run-to-completion: no blocking. */
EXPORT void smp_onemind_task_m(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    void *me = (void *)CUR_CTXTSK;
    g_om_m_tcb = me;
    __asm__ volatile("dmb ish" ::: "memory");

    /* THE CROWN: run the real mind forward ON CPU 1 + hash its output. */
    unsigned long h = r3_onemind_forward_hash();
    g_om_h_smp = h;
    __asm__ volatile("dmb ish" ::: "memory");
    g_om_m_ran = 1;                     /* publish "ran" AFTER the hash is stored */
    __asm__ volatile("dmb ish" ::: "memory");

    for (;;)
        __asm__ volatile("wfe");       /* park (②.2a terminal state) */
}

/* Helper: is `p` a pointer INTO the real TCB table? (the "real TCB, not a
 * stand-in" check, mirrors smp_prod.c). */
static int is_real_tcb(const void *p)
{
    const char *base = (const char *)&knl_tcb_table[0];
    const char *end  = (const char *)&knl_tcb_table[NUM_TSKID];
    const char *q    = (const char *)p;
    if (q < base || q >= end) return 0;
    return (((unsigned long)(q - base) % sizeof(TCB)) == 0);
}

/* The driver — runs on the boot CPU (CPU 0) inside the initial task. Returns
 * 0 = PASS (H_uni == H_smp, the mind survived SMP scheduling bit-for-bit),
 * <0 = FAIL. On a non-race build a NEGATIVE non-(-99) return is a plumbing
 * failure; rc==-99 specifically means H_uni != H_smp (the split-mind result the
 * falsifier expects, and a REAL BUG in a guard'd build). */
EXPORT int smp_onemind_test_run(void)
{
    /* ── Leg (a): the UNIPROCESSOR reference (H_uni), on CPU 0, directly. ── */
    unsigned long h_uni = r3_onemind_forward_hash();
    g_om_h_uni = h_uni;
    __asm__ volatile("dmb ish" ::: "memory");

    /* ── Leg (b): run the SAME forward as a real task M on a SECONDARY. ── */

    /* (1) confirm the initial task (CPU 0) is a real TCB. */
    void *a = (void *)CUR_CTXTSK;
    if (!is_real_tcb(a))
        return -1;

    /* (2) create + start a REAL low-prio mind task M (pri 8 < the initial
     * task's pri 1 → M sits READY in the shared queue, does NOT preempt CPU 0).
     * 8 KiB stack: r_forward uses static rc scratch, but dt_linear/LN have small
     * float[R_DM] stack locals; give headroom. */
    T_CTSK ct = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = (FP)smp_onemind_task_m, .itskpri = 8, .stksz = 8192 };
    ID mid = tk_cre_tsk(&ct);
    if (mid < E_OK)
        return -2;
    g_om_m_tskid = (int)mid;
    ER er = tk_sta_tsk(mid, 0);
    if (er < E_OK)
        return -3;

    TCB *mtcb = get_tcb(mid);

    /* (3) under the BKL, CLAIM M for CPU 1 (remove from the shared ready queue
     * so CPU 0 never also dispatches it, publish as CPU 1's schedtsk). */
    bkl_acquire();
    knl_make_non_ready(mtcb);
    mtcb->state = TS_READY;
    g_smpcpu[1].schedtsk = mtcb;
    g_smpcpu[1].ctxtsk   = NULL;
    g_smpcpu[1].dispatch_disabled = 0;
    bkl_release();
    __asm__ volatile("dsb ish" ::: "memory");

    /* (4) release the secondaries. The RACER (CPU 2) is armed FIRST so its
     * shared-rc scribble is already in flight when M's forward starts (falsifier
     * only; g_onemind_racer_go is inert in the normal build — no racer CPU reads
     * it there). Then release CPU 1 into the production dispatcher → it switches
     * into M → r_forward runs on CPU 1. */
    g_onemind_racer_go = 1;
    __asm__ volatile("dsb ish" ::: "memory");
    g_onemind_secondary_go = 1;
    __asm__ volatile("dsb ish" ::: "memory");
    long on = smp_bringup_secondary();
    if (on != 0 && on != -4 /*ALREADY_ON*/)
        return -4;

    /* (5) bounded-wait until M records the SMP hash. */
    const unsigned long MAX = 400000000UL;
    unsigned long tries = 0;
    for (;;) {
        __asm__ volatile("dmb ld" ::: "memory");
        if (g_om_m_ran == 1 && g_om_m_tcb != 0)
            break;
        if (++tries >= MAX)
            return -5;                   /* M never ran on CPU 1 (FAIL) */
        __asm__ volatile("yield" ::: "memory");
    }

    /* (6) M really ran on CPU 1 (a distinct real TCB from A). */
    void *m_now = (void *)g_smpcpu[1].ctxtsk;
    if (!is_real_tcb(m_now))   return -6;
    if (m_now == a)            return -7;
    if (m_now != (void *)mtcb) return -8;

    /* (7) THE CROWN ASSERTION: H_uni == H_smp (byte-identical mind output). */
    __asm__ volatile("dmb ld" ::: "memory");
    if (g_om_h_uni != g_om_h_smp)
        return -99;                      /* split mind — the falsifier's result */

    return 0;                            /* PASS: the mind stayed ONE under SMP */
}

/* ── evidence accessors (usermain.c reads these for the verdict print) ──── */
unsigned long smp_onemind_h_uni(void) { __asm__ volatile("dmb ld":::"memory"); return g_om_h_uni; }
unsigned long smp_onemind_h_smp(void) { __asm__ volatile("dmb ld":::"memory"); return g_om_h_smp; }
unsigned long smp_onemind_m_ran(void) { __asm__ volatile("dmb ld":::"memory"); return g_om_m_ran; }
void         *smp_onemind_m_tcb(void) { __asm__ volatile("dmb ld":::"memory"); return (void *)g_om_m_tcb; }
int           smp_onemind_m_tskid(void) { return g_om_m_tskid; }
unsigned long smp_onemind_filler(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) return 0UL;
    __asm__ volatile("dmb ld" ::: "memory");
    return g_onemind_filler[cpu];
}

#endif /* SMP_ONE_MIND */
