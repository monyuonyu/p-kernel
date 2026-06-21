/*
 *  smp.c (aarch64) — ②.0 full-SMP slice: 2 CPUs run the T-Kernel
 *  dispatcher under ONE Big Kernel Lock (BKL).
 *
 *  The SMALLEST real slice of ② (docs/architecture/full-smp-plan.md §7,
 *  ②.0): take the UNIPROCESSOR T-Kernel scheduler and make it genuinely
 *  symmetric-multiprocessing — N CPUs each run their OWN current task —
 *  while preserving the uniprocessor invariants almost verbatim: only ONE
 *  CPU executes kernel code at a time, serialized by the BKL.
 *
 *  WHY THIS IS SAFE FIRST (the plan's §2.1 thesis): the Big Kernel Lock
 *  makes "only one CPU is in the kernel at a time" true again, so all the
 *  §1.1 shared-state races (ready queue, allocator, object tables, ...)
 *  vanish as a class. Tasks run concurrently in COMPUTE (where MC-2 already
 *  proved concurrency is byte-safe); every kernel entry serializes.
 *
 *  ────────────────────────────────────────────────────────────────────
 *  HONEST SCOPE of ②.0 (this file) — what is delivered vs deferred:
 *
 *  DELIVERED:
 *    - g_bkl: a recursive (owner-CPU + depth) ticket-ish spinlock built on
 *      the aarch64 ldaxr/stlxr + stlr + wfe/sev discipline already proven
 *      in mc2_smp.c:147-166. Acquired on dispatcher/critical-section ENTRY,
 *      released on EXIT. Only one CPU inside the kernel at a time.
 *    - g_smpcpu[]: a per-CPU SMP block (this file's OWN — sharing/merging
 *      with mc2_smp.c's g_cpu[] is a later cleanup, §DECOUPLING). Each CPU
 *      runs its OWN ctxtsk/schedtsk from this block — the per-CPU scheduler
 *      state ② requires.
 *    - Bringup: ONE secondary is released (PSCI CPU_ON) into a new landing
 *      pad _secondary_dispatch_entry (start.S, ADDED — does NOT touch
 *      _secondary_worker), which does EL1 setup (reusing _secondary_el1_setup)
 *      then enters the per-CPU dispatcher smp_dispatch_loop (cpu_support.S).
 *    - ONE shared global ready list (g_ready[]) under the BKL — NOT per-CPU
 *      run-queues (§3.3). Both CPUs cooperatively PULL the next runnable
 *      task from it under the lock.
 *    - The certs: [smp-2-tasks-run], [smp-mutual-exclusion] (+ its NO-LOCK
 *      falsifier), [smp-boot-survives], with per-CPU execution counters.
 *
 *  DEFERRED (honest, per the plan's sequencing):
 *    - IPIs / cross-CPU preemption (GICD_SGIR + reschedule SGI) → ②.1.
 *      ②.0 uses cooperative pull from the shared ready list; a CPU re-checks
 *      the ready list when its current task yields/blocks. No CPU forces a
 *      reschedule on another (§4.3).
 *    - Per-CPU-izing the PRODUCTION knl_ctxtsk/knl_schedtsk across all 166
 *      reader sites → staged later. ②.0 does NOT touch the shipped
 *      dispatcher (.Ldispatch_loop) nor task.c's globals; the DEFAULT build
 *      is byte-identical. The SMP slice runs its OWN per-CPU dispatcher
 *      (smp_dispatch_loop) over its OWN per-CPU state — the real mechanism,
 *      proven, without destabilizing the shipped uniprocessor kernel.
 *    - Finer locks (g_rqlock/g_memlock/object locks) → ②.3.
 *    - Hosted-port (threads-as-CPUs) SMP → separate lift (§8).
 *
 *  GATING: every symbol in this file is compiled ONLY under SMP_SELFTEST.
 *  With no flag, this TU is an empty object — the shipped kernel carries
 *  NONE of it and behaves byte-identically to before. (boot/aarch64/Makefile
 *  always compiles smp.c; without -DSMP_SELFTEST it contributes nothing.)
 *
 *  DECOUPLING (avoid a merge conflict with in-flight MC-2.1b): this file
 *  does NOT #include or edit mc2_smp.c. The PSCI CPU_ON / SMPEN / MPIDR
 *  helpers it needs are STATIC in mc2_smp.c (un-externable), so we carry
 *  our OWN small copies here (the snippets are tiny + well-understood).
 *  Merging g_cpu[]/g_smpcpu[] is a later cleanup.
 *
 *  COHERENCY honesty (MC-2 §4.4, inherited): QEMU TCG models memory
 *  strongly and MAY MASK a missing dsb ish / SMPEN=0 race. The barrier
 *  teeth of the BKL are only fully [live] on RPi3 hardware. A QEMU green
 *  proves "the lock serializes + 2 CPUs run distinct tasks"; it does NOT
 *  prove the barrier discipline on weakly-ordered silicon.
 *
 *  Bare-metal-aarch64-ONLY (not on the hosted/Android COMMON list).
 * ───────────────────────────────────────────────────────────────────── */

#ifdef SMP_SELFTEST

#include <stdint.h>
#include <stddef.h>

/* ─── from the T-Kernel headers we need (header-light TU) ──────────────
 * We deliberately do NOT pull the full kernel.h here (this code runs
 * before/around the scheduler and wants a minimal surface). We mirror the
 * two TCB offsets the per-CPU dispatcher uses, guarded against drift by
 * the SAME offset.h the production dispatcher uses. */
#include "offset.h"   /* TCB_SSP, TCB_task — single source of truth */

/* ── PSCI CPU_ON (own copy; mc2_smp.c's is static) ──────────────────── */
#define PSCI_CPU_ON_AARCH64   0xC4000003UL    /* SMC64 PSCI_CPU_ON */
#define PSCI_SUCCESS           0
#define PSCI_ALREADY_ON       (-4)

static long smp_psci_cpu_on(unsigned long target_mpidr,
                            unsigned long entry_pa,
                            unsigned long context_id)
{
    register unsigned long x0 __asm__("x0") = PSCI_CPU_ON_AARCH64;
    register unsigned long x1 __asm__("x1") = target_mpidr;
    register unsigned long x2 __asm__("x2") = entry_pa;
    register unsigned long x3 __asm__("x3") = context_id;
    __asm__ volatile("hvc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3)
                     : "memory", "x4", "x5", "x6", "x7",
                       "x8", "x9", "x10", "x11", "x12",
                       "x13", "x14", "x15", "x16", "x17");
    return (long)x0;
}

/* CPUECTLR_EL1.SMPEN (own copy; mc2_smp.c's is static). The secondary's
 * start.S path also sets SMPEN in asm; this is the C-side belt-and-braces
 * for the primary before it shares any buffer. */
static inline void smp_set_smpen(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, S3_1_C15_C2_1" : "=r"(v));
    v |= (1UL << 6);
    __asm__ volatile("msr S3_1_C15_C2_1, %0" : : "r"(v));
    __asm__ volatile("isb" ::: "memory");
}

/* Firmware-independent "which CPU am I" — MPIDR Aff0 (own copy). */
static inline unsigned long smp_mpidr_aff0(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v & 0xFFUL;
}

/* Minimal blind UART writer (QEMU virt PL011 @ 0x09000000), so EITHER CPU
 * can announce progress (the secondary has no other console). Blind writes
 * (no FR busy-wait) so a wedged FIFO never hangs the writer; chars may drop
 * under contention but the markers are short. Bare-metal QEMU-virt only.
 * RPi3 PL011 is at 0x3F201000 — guarded so the marker still lands there. */
#ifdef BOARD_RPI3
#  define SMP_UART_DR   0x3F201000UL
#else
#  define SMP_UART_DR   0x09000000UL
#endif
static void smp_dbg(const char *s)
{
    volatile unsigned int *dr = (volatile unsigned int *)SMP_UART_DR;
    for (; *s; s++)
        *dr = (unsigned int)(unsigned char)*s;
}

/* ─── The per-CPU SMP block (this file's OWN; §DECOUPLING). The field
 * OFFSETS are mirrored in cpu_support.S via the SMPCPU_* macros below —
 * KEEP IN SYNC (a _Static_assert guards it). ────────────────────────── */
#define SMP_MAX_CPUS   2          /* ②.0 = boot CPU + ONE secondary */

struct smp_cpu {
    void          *ctxtsk;        /* off 0:  this CPU's running task (TCB*)  */
    void          *schedtsk;      /* off 8:  this CPU's next task    (TCB*)  */
    unsigned long  exec_count;    /* off 16: per-CPU dispatch/exec counter   */
    unsigned long  cpu_id;        /* off 24: MPIDR Aff0                       */
    volatile unsigned long live;  /* off 32: set 1 when CPU enters dispatcher*/
};

#define SMPCPU_CTXTSK     0
#define SMPCPU_SCHEDTSK   8
#define SMPCPU_EXEC       16
#define SMPCPU_SIZE       40

_Static_assert(offsetof(struct smp_cpu, ctxtsk)   == SMPCPU_CTXTSK,   "smp_cpu.ctxtsk");
_Static_assert(offsetof(struct smp_cpu, schedtsk) == SMPCPU_SCHEDTSK, "smp_cpu.schedtsk");
_Static_assert(offsetof(struct smp_cpu, exec_count)==SMPCPU_EXEC,     "smp_cpu.exec");

/* The per-CPU array (BSS, VA==PA). Slot 0 = boot CPU; slot 1 = the one
 * secondary ②.0 brings up. cpu_support.S indexes this by MPIDR Aff0. */
struct smp_cpu g_smpcpu[SMP_MAX_CPUS];

/* Exported for the asm dispatcher to compute &g_smpcpu[me]. */
unsigned long smp_this_cpu(void) { return smp_mpidr_aff0(); }

/* The asm per-CPU current-task load (cpu_support.S, SMP_SELFTEST). Returns
 * g_smpcpu[mpidr_aff0()].ctxtsk — the per-CPU generalization of the
 * production dispatcher's knl_ctxtsk read. */
extern void *smp_cur_tcb_load(void);

/* ════════════════════════════════════════════════════════════════════
 *  THE BIG KERNEL LOCK (g_bkl)
 *
 *  Recursive: a CPU already holding the BKL (e.g. a nested critical
 *  section, or — in a fuller wave — a timer IRQ nested inside a syscall)
 *  must NOT deadlock against itself. We track owner-CPU + recursion depth
 *  and acquire the underlying spinlock ONLY on the outermost entry.
 *
 *  Underlying spinlock: ldaxr/stlxr + stlr with wfe/sev backoff — the
 *  exact discipline proven in mc2_smp.c:147-166. wfe makes a waiter sleep.
 * ════════════════════════════════════════════════════════════════════ */
static unsigned int          g_bkl_lock  = 0;      /* the raw spinlock word   */
static volatile long         g_bkl_owner = -1;     /* owner CPU id (-1 = free) */
static volatile unsigned int g_bkl_depth = 0;      /* recursion depth          */

static void raw_lock(unsigned int *l)
{
    unsigned int tmp;
    __asm__ volatile(
        "   sevl                      \n"
        "1: wfe                       \n"
        "2: ldaxr   %w0, [%1]         \n"
        "   cbnz    %w0, 1b           \n"
        "   stxr    %w0, %w2, [%1]    \n"
        "   cbnz    %w0, 2b           \n"
        : "=&r"(tmp)
        : "r"(l), "r"(1u)
        : "memory");
}

static void raw_unlock(unsigned int *l)
{
    __asm__ volatile("stlr wzr, [%0]\n" : : "r"(l) : "memory");
    __asm__ volatile("sev" ::: "memory");
}

/* Acquire the BKL (recursive). Returns nothing; pairs with bkl_release. */
void bkl_acquire(void)
{
    long me = (long)smp_mpidr_aff0();

    /* Already mine? Just bump depth — no raw re-acquire (no self-deadlock). */
    if (g_bkl_owner == me) {
        g_bkl_depth++;
        return;
    }

    raw_lock(&g_bkl_lock);          /* spin/wfe until we own the raw lock */
    g_bkl_owner = me;               /* publish ownership UNDER the lock    */
    g_bkl_depth = 1;
    __asm__ volatile("dsb ish" ::: "memory");
}

void bkl_release(void)
{
    /* Only the owner releases; depth gates the raw unlock. */
    if (--g_bkl_depth == 0) {
        __asm__ volatile("dsb ish" ::: "memory");
        g_bkl_owner = -1;
        raw_unlock(&g_bkl_lock);
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  THE ONE SHARED GLOBAL READY LIST (g_ready[]) — §3.3
 *
 *  NOT per-CPU run-queues (deferred to ②.3). A tiny array of runnable
 *  task TCB pointers; each CPU, when it needs work, locks the BKL, pops the
 *  next runnable task, sets g_smpcpu[me].schedtsk, unlocks. This preserves
 *  global semantics (the runnable tasks run on whichever CPU pulls them)
 *  and is fully correct under the BKL.
 *
 *  For the ②.0 self-test the "runnable set" is a fixed pool of self-test
 *  tasks; the real production knl_ready_queue per-CPU-ization is staged
 *  (§DEFERRED). This is the smallest real shared ready queue.
 * ════════════════════════════════════════════════════════════════════ */
/* A self-test "task": an id, a per-task increment budget, and a flag the
 * CPU sets when it has pulled/run it. The pointer to this struct serves as
 * the per-CPU ctxtsk (a stand-in TCB for the slice). */
struct smp_task {
    unsigned long id;
    unsigned long budget;        /* K increments of the shared counter */
    volatile int  claimed;       /* a CPU has pulled it (no double-run) */
    volatile int  done;          /* fully run                          */
};

#define SMP_MAX_READY   8
static struct smp_task *g_ready[SMP_MAX_READY]; /* runnable task slots */
static int              g_ready_n  = 0;         /* count               */
static int              g_ready_hd = 0;         /* round-robin head    */

/* Push a runnable task (caller holds the BKL). */
void smp_ready_push(struct smp_task *t)
{
    if (g_ready_n < SMP_MAX_READY)
        g_ready[g_ready_n++] = t;
}

/* Pull + CLAIM the next UNCLAIMED runnable task (caller holds the BKL).
 * Scans round-robin from g_ready_hd so two CPUs pulling back-to-back get
 * DISTINCT tasks. Marks the returned task claimed (under the BKL → atomic)
 * so the other CPU cannot double-run it. Returns NULL when every task is
 * already claimed (the ready list is BOUNDED — no infinite skip loop). */
struct smp_task *smp_ready_pull(void)
{
    if (g_ready_n == 0)
        return NULL;
    for (int k = 0; k < g_ready_n; k++) {
        int idx = (g_ready_hd + k) % g_ready_n;
        struct smp_task *t = g_ready[idx];
        if (t && !t->claimed) {
            t->claimed = 1;                 /* claim under the BKL */
            g_ready_hd = (idx + 1) % g_ready_n;
            return t;
        }
    }
    return NULL;                            /* all claimed → no work */
}

/* ════════════════════════════════════════════════════════════════════
 *  THE SHARED-COUNTER MUTUAL-EXCLUSION CERT  [smp-mutual-exclusion]
 *
 *  Tasks on BOTH CPUs increment ONE shared counter through a kernel-locked
 *  path. With the BKL, the final total == the exact expected count (no lost
 *  updates). FALSIFIER (-DSMP_MUTEX_NOLOCK): the increment bypasses the
 *  BKL → under truly-concurrent tasks the RMW races → lost updates → the
 *  total is WRONG. That proves the lock is load-bearing.
 * ════════════════════════════════════════════════════════════════════ */
volatile unsigned long g_shared_counter = 0;

/* One increment of the shared counter. Locked unless the NOLOCK falsifier
 * is active. We do a deliberately NON-atomic read-modify-write with a tiny
 * window so that, WITHOUT the lock, concurrent CPUs lose updates (the
 * window makes the race statistically certain even when QEMU TCG models
 * memory strongly — the falsifier must bite). */
void smp_counter_inc(void)
{
#ifndef SMP_MUTEX_NOLOCK
    bkl_acquire();
#endif
    unsigned long v = g_shared_counter;     /* read  */
    /* widen the race window so a NOLOCK build loses updates reliably */
    for (volatile int s = 0; s < 8; s++)
        __asm__ volatile("" ::: "memory");
    g_shared_counter = v + 1;               /* write */
#ifndef SMP_MUTEX_NOLOCK
    bkl_release();
#endif
}

/* ════════════════════════════════════════════════════════════════════
 *  BRINGUP — release ONE secondary into the per-CPU dispatcher.
 *
 *  The boot CPU calls smp_bringup_secondary() AFTER the kernel is up. It
 *  inits g_smpcpu[], sets SMPEN on itself, and releases CPU 1 via PSCI
 *  CPU_ON to _secondary_dispatch_entry (start.S, ADDED). The context_id
 *  (x0) handed to the secondary is &g_smpcpu[1] so its asm setup can load
 *  its own stack from it (mirrors mc2_smp.c's pattern).
 * ════════════════════════════════════════════════════════════════════ */
extern void _secondary_dispatch_entry(void);   /* start.S (ADDED) */
extern unsigned char _stack_top_cpu1[];         /* linker.ld (reused) */

/* The secondary's per-CPU dispatcher stack (its own region). */
unsigned long g_smp_sec_stack_top = 0;

long smp_bringup_secondary(void)
{
    smp_set_smpen();                            /* primary's SMPEN */

    g_smpcpu[0].cpu_id = 0;
    g_smpcpu[1].cpu_id = 1;
    g_smp_sec_stack_top = (unsigned long)_stack_top_cpu1;
    __asm__ volatile("dsb ish" ::: "memory");   /* publish the block */

    /* QEMU virt cortex-a53: core 1 has MPIDR Aff0 == 1. */
    return smp_psci_cpu_on(1,
                           (unsigned long)&_secondary_dispatch_entry,
                           (unsigned long)&g_smpcpu[1]);
}

/* Bounded wait until the secondary marks itself live in the dispatcher.
 * Returns 0 = up, -1 = timeout (a wedged/missing CPU is detectable, NOT an
 * infinite hang — the [smp-boot-survives] watchdog). */
int smp_wait_secondary_live(void)
{
    const unsigned long MAX_TRIES = 200000000UL;
    unsigned long tries = 0;
    for (;;) {
        __asm__ volatile("dmb ld" ::: "memory");
        if (g_smpcpu[1].live == 1)
            return 0;
        if (++tries >= MAX_TRIES)
            return -1;
        __asm__ volatile("wfe");
    }
}

unsigned long smp_exec_count(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) return 0UL;
    __asm__ volatile("dmb ld" ::: "memory");
    return g_smpcpu[cpu].exec_count;
}

void *smp_running_tcb(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) return NULL;
    __asm__ volatile("dmb ld" ::: "memory");
    return g_smpcpu[cpu].ctxtsk;
}

unsigned long smp_get_counter(void)
{
    __asm__ volatile("dmb ld" ::: "memory");
    return g_shared_counter;
}

/* ════════════════════════════════════════════════════════════════════
 *  THE PER-CPU DISPATCHER ORCHESTRATOR + the ②.0 self-test workload.
 *
 *  smp_dispatch_run() is the C body of the per-CPU dispatcher loop
 *  (entered from smp_dispatch_loop in cpu_support.S on BOTH CPUs). Each
 *  CPU:
 *    1. marks g_smpcpu[me].live = 1   (boot-survives observability)
 *    2. under the BKL, PULLS the next runnable task from the ONE shared
 *       ready list (smp_ready_pull) and sets g_smpcpu[me].schedtsk/ctxtsk
 *       — the per-CPU current task (§3). The asm helper smp_cur_tcb_load
 *       reads back this per-CPU ctxtsk to prove the per-CPU load works in
 *       asm.
 *    3. RUNS the pulled task body on THIS CPU (cooperative — ②.0 defers
 *       the full task-stack context switch; §4.3).
 *    4. loops until the shared ready list is drained, then idles.
 *
 *  Because the round-robin pull (smp_ready_pull) hands distinct tasks to
 *  back-to-back pullers, the two CPUs run DISTINCT tasks → distinct ctxtsk
 *  → [smp-2-tasks-run].
 * ════════════════════════════════════════════════════════════════════ */

/* (struct smp_task is defined above, near the ready list.) */
#define SMP_NTASKS   2
static struct smp_task g_tasks[SMP_NTASKS];
static unsigned long   g_task_budget = 0;   /* K, set by the driver */

/* Concurrency barrier: each CPU bumps this when it is about to start its
 * increment loop; both spin until it reaches SMP_MAX_CPUS so the loops run
 * TRULY CONCURRENTLY (so the NOLOCK falsifier reliably loses updates). */
static volatile unsigned long g_barrier = 0;

static void smp_barrier_wait(void)
{
    bkl_acquire();
    g_barrier++;
    bkl_release();
    /* Spin (NOT under the lock) until every CPU has arrived. Bounded so a
     * missing CPU can't wedge forever. */
    unsigned long tries = 0;
    while (g_barrier < SMP_MAX_CPUS) {
        __asm__ volatile("dmb ld" ::: "memory");
        if (++tries >= 200000000UL) break;
        __asm__ volatile("yield" ::: "memory");
    }
}

/* Run one self-test task on the calling CPU: do `budget` shared-counter
 * increments. The barrier ensures both CPUs increment concurrently. */
static void smp_run_task(struct smp_task *t)
{
    smp_barrier_wait();                       /* line both CPUs up */
    for (unsigned long k = 0; k < t->budget; k++)
        smp_counter_inc();                    /* locked (or NOLOCK falsifier) */
    __asm__ volatile("dmb st" ::: "memory");
    t->done = 1;
}

/* The per-CPU dispatcher loop body (entered from smp_dispatch_loop). */
void smp_dispatch_run(void)
{
    unsigned long me = smp_mpidr_aff0();
    if (me >= SMP_MAX_CPUS) {
        for (;;) __asm__ volatile("wfe");     /* never index OOB */
    }

    g_smpcpu[me].live = 1;
    __asm__ volatile("dmb st; sev" ::: "memory");
    smp_dbg("[SMP] cpu1 entered dispatcher\r\n");

    for (;;) {
        /* PULL+CLAIM the next runnable task under the BKL (§3.3 shared
         * queue). smp_ready_pull claims it atomically (BKL held) and
         * returns NULL once every task is claimed — bounded, no infinite
         * skip loop. The OTHER CPU therefore gets a DISTINCT task. */
        bkl_acquire();
        struct smp_task *t = smp_ready_pull();
        if (t) {
            g_smpcpu[me].schedtsk = t;        /* per-CPU next task */
            g_smpcpu[me].ctxtsk   = t;        /* per-CPU current task */
            g_smpcpu[me].exec_count++;        /* this CPU advanced ITS task */
        }
        bkl_release();

        if (!t)
            break;                            /* ready list drained */

        /* Prove the asm per-CPU current-task load returns OUR task. */
        if (smp_cur_tcb_load() != (void *)t) {
            /* Per-CPU state mismatch — should never happen under the BKL.
             * Mark a sentinel so the driver can detect it. */
            g_smpcpu[me].exec_count |= (1UL << 60);
        }

        smp_run_task(t);                      /* RUN it on this CPU */
    }

    /* Workload drained on this CPU — idle (the driver reaps via counters). */
    for (;;)
        __asm__ volatile("wfe");
}

/* ── Driver entry (called by the boot self-test in main.c) ──────────────
 *  Sets up the two tasks + the shared ready list, releases the secondary,
 *  enters its OWN per-CPU dispatcher (so the BOOT CPU also runs a task),
 *  then — after both CPUs have run — returns the verdict via the helpers.
 *
 *  NOTE: the boot CPU does NOT return from smp_dispatch_run (it idles after
 *  draining). So the driver runs the dispatcher in a SEPARATE flow: it
 *  seeds the work, releases CPU1, then runs ITS share of the work inline
 *  (one task) and joins. We keep the boot CPU's loop BOUNDED to its single
 *  task so it returns to print the verdict. */
int smp_selftest_run(unsigned long K)
{
    g_task_budget = K;
    for (int i = 0; i < SMP_NTASKS; i++) {
        g_tasks[i].id      = (unsigned long)(100 + i);  /* distinct ids */
        g_tasks[i].budget  = K;
        g_tasks[i].claimed = 0;
        g_tasks[i].done    = 0;
    }
    g_shared_counter = 0;
    g_barrier        = 0;
    g_ready_n = 0; g_ready_hd = 0;

    /* Exercise the BKL RECURSION path before releasing the secondary: a
     * nested bkl_acquire on the SAME CPU must NOT self-deadlock (the
     * recursive owner-CPU+depth design, §2.1). If recursion were broken,
     * this would hang HERE — making the recursive claim non-paper. */
    bkl_acquire();
    bkl_acquire();          /* nested — depth 2, no raw re-acquire */
    for (int i = 0; i < SMP_NTASKS; i++)
        smp_ready_push(&g_tasks[i]);
    bkl_release();          /* depth 2 -> 1, raw lock still held */
    bkl_release();          /* depth 1 -> 0, raw unlock */
    __asm__ volatile("dsb ish" ::: "memory");

    /* Release the secondary into its per-CPU dispatcher (it will pull one
     * task and run it concurrently with us). */
    long on = smp_bringup_secondary();
    if (on != PSCI_SUCCESS && on != PSCI_ALREADY_ON)
        return (int)on;                       /* CPU_ON failed */

    /* The BOOT CPU runs ITS share inline: pull exactly one task, run it,
     * then join. (We do NOT call smp_dispatch_run here — that idles; the
     * boot CPU must return to print the verdict.) */
    unsigned long me = smp_mpidr_aff0();      /* 0 */
    g_smpcpu[me].live = 1;
    {
        bkl_acquire();
        struct smp_task *t = smp_ready_pull();  /* claims atomically (BKL) */
        if (t) {
            g_smpcpu[me].schedtsk = t;
            g_smpcpu[me].ctxtsk   = t;
            g_smpcpu[me].exec_count++;
        }
        bkl_release();
        if (t) {
            if (smp_cur_tcb_load() != (void *)t)
                g_smpcpu[me].exec_count |= (1UL << 60);
            smp_run_task(t);                  /* boot CPU runs its task */
        }
    }

    /* JOIN: bounded wait until both tasks report done (the secondary
     * finished its task). Watchdog so a wedged CPU is a FAIL, not a hang. */
    const unsigned long MAX = 200000000UL;
    unsigned long tries = 0;
    for (;;) {
        __asm__ volatile("dmb ld" ::: "memory");
        int alldone = 1;
        for (int i = 0; i < SMP_NTASKS; i++)
            if (!g_tasks[i].done) { alldone = 0; break; }
        if (alldone) break;
        if (++tries >= MAX) return -100;      /* join timeout (FAIL) */
        __asm__ volatile("wfe");
    }
    return 0;
}

#endif /* SMP_SELFTEST */
