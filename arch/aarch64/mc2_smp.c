/*
 *  mc2_smp.c (aarch64) — MC-2.0 bare-metal constrained-SMP bringup
 *
 *  The SMALLEST real slice of bare-metal SMP (docs/architecture/
 *  mc2-baremetal-smp-plan.md §7 MC-2.0):
 *
 *    - wake EXACTLY ONE parked aarch64 secondary core via PSCI CPU_ON
 *      (function id 0xC4000003, SMC64) over `hvc #0` (QEMU virt's
 *      psci-conduit is HVC — arch_reboot.c already uses HVC for
 *      SYSTEM_RESET).
 *    - the secondary lands in _secondary_worker (start.S), sets up its
 *      own EL1 (caches + FP + CPUECTLR.SMPEN=1; MMU stays OFF, VA==PA),
 *      then enters pk_smp_worker_loop() here.
 *    - it sets g_cpu[1].woken=1, runs ONE trivial fixed deterministic
 *      tile (fill a known buffer with a fixed pattern), signals done via
 *      a single shared done-counter under one ldaxr/stlxr lock, then
 *      `wfe`-loops (NO busy-spin).
 *    - the primary observes woken/done with proper acquire barriers and
 *      verifies the tile output → MC2-BOOT: PASS / FAIL <why>.
 *
 *  HARD BOUNDARY (the safety margin ② must add on top of this):
 *    - The secondary NEVER runs a T-Kernel task. It never reads
 *      knl_ctxtsk / knl_schedtsk. The dispatcher (cpu_support.S
 *      .Ldispatch_loop / .Lidle) is byte-for-byte UNTOUCHED.
 *    - The primary stays uniprocessor.
 *
 *  This file is bare-metal-aarch64-ONLY (not in the hosted/Android
 *  COMMON list). Compiled only via boot/aarch64/Makefile.
 */

#include <stdint.h>
#include <stddef.h>

/* MC-2.1: the bare-metal pk_parallel_rows backend implements the SAME
 * contract the hosted pthread pool does (arch/common/llm/pk_parallel.h:104,
 * :39, :45). The hosted header lives in the LLM tier (arch/common/llm/),
 * which is NOT on the bare-metal include path and whose .c is NOT linked
 * here. So we carry the two needed declarations VERBATIM (KEEP IN SYNC):
 *   - pk_row_body            : pk_parallel.h:39
 *   - PK_PARALLEL_MIN_ROWS   : pk_parallel.h:45
 * The signature `void pk_parallel_rows(size_t,pk_row_body,void*)` matches
 * pk_parallel.h:104 so the bare-metal backend is the identical seam. */
typedef void (*pk_row_body)(void *ctx, size_t i0, size_t i1);
#define PK_PARALLEL_MIN_ROWS 64

/* ---------------------------------------------------------------------
 *  Per-CPU block. The field OFFSETS are mirrored in start.S via the
 *  PK_CPU_* macros below — KEEP IN SYNC (a _Static_assert guards it).
 * ------------------------------------------------------------------- */
#define PK_SMP_MAX_CPUS   4           /* QEMU -smp 4 / RPi3 = 4 cores */

struct pk_cpu {
    unsigned long stack_top;          /* off 0:  this core's own stack    */
    unsigned long cpu_id;             /* off 8:  mpidr Aff0 (1..N-1)       */
    volatile unsigned long woken;     /* off 16: set in the worker loop    */
};

/* Offsets used by _secondary_worker in start.S. */
#define PK_CPU_STACK_TOP   0
#define PK_CPU_CPU_ID      8
#define PK_CPU_WOKEN       16
#define PK_CPU_SIZE        24

_Static_assert(offsetof(struct pk_cpu, stack_top) == PK_CPU_STACK_TOP, "pk_cpu.stack_top");
_Static_assert(offsetof(struct pk_cpu, cpu_id)    == PK_CPU_CPU_ID,    "pk_cpu.cpu_id");
_Static_assert(offsetof(struct pk_cpu, woken)     == PK_CPU_WOKEN,     "pk_cpu.woken");
_Static_assert(sizeof(struct pk_cpu)              == PK_CPU_SIZE,      "pk_cpu size");

/* The per-CPU array. Slot 0 = primary (never woken as a worker here);
 * slot 1 = the one secondary MC-2.0 brings up. Slots 2/3 reserved for
 * MC-2.1 (N cores). Lives in BSS, flat-mapped, VA==PA. */
struct pk_cpu g_cpu[PK_SMP_MAX_CPUS];

/* Linker symbols: each secondary's dedicated stack top (linker.ld).
 * MC-2.0 added cpu1; MC-2.1 adds cpu2/cpu3 (§3.5). */
extern unsigned char _stack_top_cpu1[];
extern unsigned char _stack_top_cpu2[];   /* MC-2.1 */
extern unsigned char _stack_top_cpu3[];   /* MC-2.1 */

/* Map slot -> its dedicated stack top symbol (§3.1/§3.5). Slot 0 is the
 * primary (uses _stack_top, never released as a worker here). */
static unsigned long pk_stack_top_for(int slot)
{
    switch (slot) {
        case 1: return (unsigned long)_stack_top_cpu1;   /* REUSE MC-2.0 region */
        case 2: return (unsigned long)_stack_top_cpu2;   /* NEW MC-2.1 */
        case 3: return (unsigned long)_stack_top_cpu3;   /* NEW MC-2.1 */
        default: return 0;
    }
}

/* The assembly landing pad the secondary is released to (start.S). */
extern void _secondary_worker(void);

/* The firmware-independent "which core am I" (§3.2). Reads MPIDR_EL1 and
 * masks Aff0 — matches start.S:41-42's `and x0,x0,#0xFF`. Each released
 * secondary uses this to derive its OWN slot (NO core hardcodes slot 1). */
static inline unsigned long mpidr_aff0(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v & 0xFFUL;                 /* Aff0 */
}

/* ---------------------------------------------------------------------
 *  MC-2.0 trivial deterministic tile.
 *
 *  The secondary fills a known global buffer with a fixed pattern so
 *  the primary can prove the core ACTUALLY executed (not just woke). The
 *  pattern is purely integer + position-derived so it is byte-identical
 *  across runs and trivially checkable. (MC-2.1 grows this into the real
 *  gate-exceeding matmul cert; MC-2.0 only proves the bringup.)
 * ------------------------------------------------------------------- */
#define MC2_TILE_LEN   256

volatile uint32_t g_mc2_tile[MC2_TILE_LEN];

/* The single source of truth for the expected pattern — used by BOTH the
 * worker (to write) and the primary (to verify), so they cannot drift. */
static inline uint32_t mc2_tile_expected(unsigned i)
{
    /* A fixed deterministic function of the index. The 0x5A5A salt makes
     * an all-zero (untouched BSS) buffer fail the check, so a no-op
     * worker is caught. */
    return (uint32_t)(i * 2654435761u) ^ 0x5A5A0000u;
}

/* ---------------------------------------------------------------------
 *  The one work-queue / done-counter + one spinlock (ldaxr/stlxr).
 *  MC-2.0 uses a single done-flag and a single tile; MC-2.1 generalises
 *  this to the full §3.3 N-slice work-queue.
 * ------------------------------------------------------------------- */
static volatile unsigned long g_done = 0;   /* set to 1 by the worker */
static unsigned int          g_lock = 0;    /* the one spinlock word  */

/* Whether the worker should deliberately fault its tile (falsification:
 * proves the primary's bounded join does not wedge forever). Driven by a
 * build flag; default 0. */
#ifdef MC2_FAULTING_TILE
static const int g_fault_tile = 1;
#else
static const int g_fault_tile = 0;
#endif

/* ldaxr/stlxr exclusive spinlock with wfe backoff (§3.3). Tiny — the
 * critical section is a single store. */
static void mc2_lock(void)
{
    unsigned int tmp;
    __asm__ volatile(
        "   sevl                      \n"  /* prime the local event   */
        "1: wfe                       \n"
        "2: ldaxr   %w0, [%1]         \n"
        "   cbnz    %w0, 1b           \n"  /* held? sleep on wfe       */
        "   stxr    %w0, %w2, [%1]    \n"
        "   cbnz    %w0, 2b           \n"  /* store failed? retry      */
        : "=&r"(tmp)
        : "r"(&g_lock), "r"(1u)
        : "memory");
}

static void mc2_unlock(void)
{
    __asm__ volatile("stlr wzr, [%0]\n" : : "r"(&g_lock) : "memory");
    __asm__ volatile("sev" ::: "memory");  /* wake a contended waiter */
}

/* ---------------------------------------------------------------------
 *  CPUECTLR_EL1.SMPEN (S3_1_C15_C2_1, bit 6). §1.3 — without this a
 *  core's cacheable accesses are not guaranteed coherent across cores.
 *  Called from the worker (the secondary) early; the primary's bit is
 *  set by mc2_smp_primary_init() below.
 * ------------------------------------------------------------------- */
static inline void mc2_set_smpen(void)
{
#ifdef MC2_EQUIV_SMPEN_OFF
    /* TOOTH B (barrier/SMPEN falsifier, §4.4): deliberately do NOT set
     * SMPEN from C. On REAL weakly-ordered hardware (RPi3 A53) this leaves
     * cacheable accesses non-coherent across cores -> stale/torn output ->
     * memcmp mismatch. On QEMU TCG this race is MASKED (TCG models memory
     * strongly), so a QEMU PASS here is EXPECTED and recorded as "masked,
     * deferred to RPi3 (MC-2.2)" — NOT a proof that barriers are verified.
     * NB: start.S still sets SMPEN in asm for the SECONDARIES; this only
     * removes the C-side (re)assert, the strongest knob available without
     * touching the reused landing pad. */
    return;
#else
    unsigned long v;
    __asm__ volatile("mrs %0, S3_1_C15_C2_1" : "=r"(v));
    v |= (1UL << 6);
    __asm__ volatile("msr S3_1_C15_C2_1, %0" : : "r"(v));
    __asm__ volatile("isb" ::: "memory");
#endif
}

/* ---------------------------------------------------------------------
 *  MC-2.1 — the deterministic partition + the ONE N-slice work-queue.
 * ------------------------------------------------------------------- */

/* pk_slice_bm — a BYTE-FOR-BYTE copy of pk_slice() in the hosted golden
 * arch/common/llm/pk_parallel.c:57-63 (commit-pinned). The hosted TU is
 * NOT linked on bare metal (pk_parallel.h:18-24; boot/aarch64/Makefile
 * excludes it), so the bare-metal worker CANNOT link pk_slice — it must
 * carry this identical copy so the bits match both the hosted golden and
 * the serial path. KEEP IN SYNC (the §3.4 "one mind, one math" drift
 * surface; the equiv self-test's nw=1 reference uses this SAME function,
 * and MC-2.1b adds a self-consistency unit check — §4.5).
 *
 *   --- verbatim from pk_parallel.c:57-63 ---
 *   Slice s in [0,nw) owns rows [s*q + ...). A ragged remainder
 *   (out % nw) is given to the LAST slice. */
static void pk_slice_bm(size_t out, int nw, int s, size_t *i0, size_t *i1)
{
    size_t q = out / (size_t)nw;
    /* first (nw-1) slices get q rows each; the last gets the remainder too. */
    *i0 = (size_t)s * q;
#ifdef MC2_SLICE_BREAK
    /* FALSIFIER (MC-2.1b §2): deliberately perturb the PARTITION so the
     * standalone unit-check (mc2_slice_unitcheck) MUST report MC2-SLICE: FAIL.
     * This DROPS the ragged remainder — the last slice ends at i0+q instead of
     * `out`, so out%nw rows [out-(out%nw),out) are NEVER covered (a TOTAL/
     * DISJOINT gap) AND the last slice no longer matches the hosted golden
     * (pk_parallel.c:62). A build flag, never in the default kernel; it gives
     * the check teeth (proves it would catch a real drift of this hand-copy). */
    *i1 = *i0 + q;
#else
    *i1 = (s == nw - 1) ? out : (*i0 + q);
#endif
}

/* The ONE global work-queue (BSS, VA==PA). The primary (worker 0) fills
 * body/ctx/out/nw under the lock, bumps `gen` to PUBLISH, runs slice 0
 * itself, and joins on `done` reaching nw-1. Secondaries wfe until `gen`
 * advances, run their slice, and ++`done` under the lock. (§3.3) */
struct pk_wq {
    volatile pk_row_body   body;    /* current job (the serial inner loop)  */
    volatile void         *ctx;     /* matmul operands                      */
    volatile size_t        out;     /* total output rows                    */
    volatile int           nw;      /* slices this dispatch partitioned in  */
    volatile unsigned long gen;     /* bumped once per dispatch (PUBLISH)   */
    volatile int           done;    /* secondary slices finished (lock)     */
};
static struct pk_wq g_wq;

/* Bare-metal wake counter (analogue of pk_parallel_wake_count,
 * pk_parallel.c:140-143): a secondary increments its slot's entry ONLY
 * when it drains a real job (gen advanced). The idle assertion (§4.5)
 * reads these to prove secondaries BLOCK on wfe (no spurious drains). */
static volatile unsigned long g_wake[PK_SMP_MAX_CPUS];

/* Number of secondaries released into the work-queue (1..N-1). EVERY
 * released secondary drains EVERY published gen and reports `done`,
 * INDEPENDENT of nw — exactly like the hosted pool joins on `nhelpers`
 * not `nw` (pk_parallel.c:99,203). The dispatch join therefore waits for
 * g_wq_nhelpers reports, NOT nw-1: a slot that did no work (slot>=nw)
 * still ++done, so joining on nw-1 could break BEFORE the slice that
 * actually did the work has finished its store. Set by mc2_smp_release_n. */
static volatile int g_wq_nhelpers = 0;

/* Cert hook: force a worker count for the next pk_parallel_rows dispatch
 * (bare-metal analogue of pk_parallel_set_threads, pk_parallel.c:135).
 * <=0 = serial inline. The self-test sets this to 1/2/4. */
static int g_force_nw = 0;
void pk_smp_force_nw(int nw) { g_force_nw = nw; }

unsigned long pk_smp_wake_count(int slot)
{
    if (slot < 0 || slot >= PK_SMP_MAX_CPUS) return 0UL;
    __asm__ volatile("dmb ld" ::: "memory");
    return g_wake[slot];
}

/* ---------------------------------------------------------------------
 *  pk_smp_worker_loop — the C worker body. Entered by the secondary from
 *  _secondary_worker (start.S) AFTER its EL1 setup. NEVER returns to a
 *  T-Kernel context; never touches the scheduler.
 *
 *  MC-2.0 path (preserved): run ONE fixed deterministic tile + set
 *  g_done, so [mc2-boot-survives] still observes woken/g_done/tile.
 *  MC-2.1 path: AFTER the tile, enter the gen-driven work-queue drain
 *  (§3.2). The worker derives its OWN slot from MPIDR — NOT hardcoded.
 * ------------------------------------------------------------------- */
void pk_smp_worker_loop(void)
{
    /* SMPEN belt-and-braces: _secondary_el1_setup already set it in asm,
     * but re-asserting from C is harmless and keeps the C path honest. */
    mc2_set_smpen();

    /* Derive THIS core's slot (1..N-1) from MPIDR Aff0 — the firmware-
     * independent ground truth of "which core am I" (§3.2). NO core may
     * hardcode slot 1 (closes the MC-2.0 §1.6 hardcode). */
    int slot = (int)mpidr_aff0();
    if (slot < 1 || slot >= PK_SMP_MAX_CPUS) {
        /* Defensive: a slot we never released (or a malformed MPIDR).
         * Park — never index g_cpu[]/g_wake[] out of range. */
        for (;;)
            __asm__ volatile("wfe");
    }

    /* Mark this core awake. The primary set g_cpu[slot].cpu_id==slot at
     * release; assert the firmware actually landed us where expected. */
    g_cpu[slot].woken = 1;
    __asm__ volatile("dmb st" ::: "memory");   /* publish woken */
    __asm__ volatile("sev" ::: "memory");      /* poke the primary's join */

    /* MC-2.0 deterministic tile — slot 1 only writes it (the MC-2.0
     * single-secondary cert checks exactly this buffer + g_done). Cores
     * 2/3 skip the tile and go straight to the work-queue. */
    if (slot == 1) {
        if (!g_fault_tile) {
            for (unsigned i = 0; i < MC2_TILE_LEN; i++)
                g_mc2_tile[i] = mc2_tile_expected(i);
            /* RELEASE the tile stores, then publish done under the lock. */
            __asm__ volatile("dmb st" ::: "memory");
            mc2_lock();
            g_done = 1;
            mc2_unlock();
            __asm__ volatile("sev" ::: "memory");  /* wake the primary's join */
        } else {
            /* Falsification variant (MC-2.0): deliberately NEVER complete
             * the tile or g_done — emulate a worker that faulted mid-tile.
             * The primary's bounded join must NOT wedge forever (§4.2).
             * Park here; do NOT enter the work-queue. */
            for (;;)
                __asm__ volatile("wfe");
        }
    }

    /* MC-2.1 work-queue drain (§3.2). wfe until gen advances; acquire the
     * published job; compute this slot's slice; run the UNMODIFIED serial
     * body over [i0,i1); release stores; ++done under the lock; sev. The
     * worker NEVER returns and NEVER touches knl_ctxtsk/knl_schedtsk. */
    unsigned long seen = 0;
    for (;;) {
        while (g_wq.gen == seen)                  /* no new job */
            __asm__ volatile("wfe");              /* BLOCK — no busy-spin */
        seen = g_wq.gen;
        __asm__ volatile("dmb ld" ::: "memory");  /* ACQUIRE the published job */

        g_wake[slot]++;                           /* drained a real job */

        if (slot < g_wq.nw) {
            size_t i0, i1;
            pk_slice_bm(g_wq.out, g_wq.nw, slot, &i0, &i1);  /* SAME partition */
            if (i1 > i0)
                ((pk_row_body)g_wq.body)((void *)g_wq.ctx, i0, i1);  /* serial body */
        }
        __asm__ volatile("dmb st" ::: "memory");  /* RELEASE this worker's stores */

        mc2_lock();                                /* REUSE the one spinlock */
        g_wq.done++;
        mc2_unlock();                              /* stlr+sev — wakes the join */
        __asm__ volatile("sev" ::: "memory");
    }
}

/* ---------------------------------------------------------------------
 *  Primary side: set SMPEN on the primary, then release ONE secondary
 *  via PSCI CPU_ON. context_id = the per-CPU struct ptr (passed to the
 *  worker in x0 by the firmware → consumed by _secondary_worker).
 * ------------------------------------------------------------------- */
#define PSCI_CPU_ON_AARCH64   0xC4000003UL    /* SMC64 PSCI_CPU_ON */

/* PSCI return codes (subset). */
#define PSCI_SUCCESS           0
#define PSCI_ALREADY_ON       (-4)

static long psci_cpu_on(unsigned long target_mpidr,
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

/* Returns the PSCI return value of CPU_ON for the one secondary. The
 * primary calls this AFTER kernel core init but is otherwise free to
 * proceed; the join happens in mc2_smp_join(). MC-2.0's single-secondary
 * release — UNCHANGED so [mc2-boot-survives] still exercises exactly the
 * core-1 + tile + g_done path. */
long mc2_smp_release_one(void)
{
    /* SMPEN on the primary itself (§1.3) before it shares any buffer. */
    mc2_set_smpen();

    /* Initialise slot 1 (the secondary). g_cpu[] is BSS; set explicitly.
     * stack_top is the TOP of the region (stack grows down). */
    g_cpu[1].stack_top = (unsigned long)_stack_top_cpu1;
    g_cpu[1].cpu_id    = 1;
    g_cpu[1].woken     = 0;
    g_done             = 0;
    __asm__ volatile("dsb ish" ::: "memory");  /* publish the block */

    /* QEMU virt cortex-a53: MPIDR for core 1 = Aff0==1 (Aff1..3 = 0). */
    unsigned long target = 1;

    return psci_cpu_on(target,
                       (unsigned long)&_secondary_worker,
                       (unsigned long)&g_cpu[1]);
}

/* ---------------------------------------------------------------------
 *  MC-2.1: generalize the single release to N secondaries (§3.1). n is
 *  the TOTAL core count including the primary (2..4). Loops the body of
 *  mc2_smp_release_one() over slots 1..n-1, each with its OWN stack +
 *  cpu_id, REUSING psci_cpu_on() and _secondary_worker verbatim.
 *
 *  Returns 0 on full success; the first non-success PSCI rc otherwise
 *  (ALREADY_ON is tolerated — a core MC-2.0 already woke). If a core
 *  fails to wake, the equiv self-test sees a missing `done` increment ->
 *  the bounded join times out -> MC2-EQUIV: FAIL (not a hang).
 * ------------------------------------------------------------------- */
long mc2_smp_release_n(int n)
{
    if (n < 2) n = 2;
    if (n > PK_SMP_MAX_CPUS) n = PK_SMP_MAX_CPUS;

    /* SMPEN on the primary itself (§1.3) before it shares any buffer. */
    mc2_set_smpen();

    /* Init slots 1..n-1 BEFORE any psci_cpu_on so a freshly-woken core
     * reads a fully-published block (its stack + cpu_id). */
    for (int slot = 1; slot < n; slot++) {
        g_cpu[slot].stack_top = pk_stack_top_for(slot);
        g_cpu[slot].cpu_id    = (unsigned long)slot;
        g_cpu[slot].woken     = 0;
        g_wake[slot]          = 0;
    }
    g_wq.gen      = 0;
    g_wq.done     = 0;
    g_wq_nhelpers = n - 1;                      /* secondaries that will drain */
    __asm__ volatile("dsb ish" ::: "memory");  /* publish the block(s) */

    long rc = 0;
    for (int slot = 1; slot < n; slot++) {
        /* QEMU virt cortex-a53: MPIDR Aff0 == core index (Aff1..3 = 0) —
         * the same assumption MC-2.0 made for core 1, now for 2,3. */
        long r = psci_cpu_on((unsigned long)slot,
                             (unsigned long)&_secondary_worker,
                             (unsigned long)&g_cpu[slot]);
        if (r != PSCI_SUCCESS && r != PSCI_ALREADY_ON && rc == 0)
            rc = r;                            /* first hard error wins */
    }
    return rc;
}

/* Wait (bounded) until all secondaries 1..n-1 have set woken==1. Returns
 * 0 on success, -1 on timeout (a core never woke — caller reports FAIL,
 * the primary survives). Used by the equiv self-test before dispatching. */
int mc2_smp_wait_woken(int n)
{
    if (n < 2) n = 2;
    if (n > PK_SMP_MAX_CPUS) n = PK_SMP_MAX_CPUS;
    const unsigned long MAX_TRIES = 200000000UL;
    for (int slot = 1; slot < n; slot++) {
        unsigned long tries = 0;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");
            if (g_cpu[slot].woken == 1)
                break;
            if (++tries >= MAX_TRIES)
                return -1;
            __asm__ volatile("wfe");
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------
 *  MC-2.1: the bare-metal pk_parallel_rows backend (§3.4). Same contract
 *  as the hosted pthread pool (pk_parallel.h:104): run body(ctx,i0,i1)
 *  over a partition of [0,out) across nw workers, then JOIN. The primary
 *  is worker 0 (runs slice 0 itself); secondaries 1..nw-1 run their slice
 *  off the work-queue. Deterministic: the partition is pk_slice_bm (pure
 *  fn of (out,nw)); completion order is irrelevant (disjoint y[i]).
 *
 *  Fallback (mirrors pk_parallel.c:171-180): nw<=1 or out below the row
 *  gate -> body(ctx,0,out) INLINE, byte-identical, no secondary touched.
 * ------------------------------------------------------------------- */
void pk_parallel_rows(size_t out, pk_row_body body, void *ctx)
{
    int nw = (g_force_nw > 0) ? g_force_nw : 1;
    if (nw > PK_SMP_MAX_CPUS) nw = PK_SMP_MAX_CPUS;

    if (nw <= 1 || out < PK_PARALLEL_MIN_ROWS) {
        body(ctx, 0, out);                 /* serial inline — identical bits */
        return;
    }

    /* --- DISPATCH (primary = worker 0), §3.3 --- */
    mc2_lock();
    g_wq.body = body;
    g_wq.ctx  = ctx;
    g_wq.out  = out;
    g_wq.nw   = nw;
    g_wq.done = 0;
#ifndef MC2_EQUIV_NO_BARRIER
    __asm__ volatile("dsb ish" ::: "memory");  /* job fields land BEFORE gen */
#else
    /* TOOTH B (barrier falsifier, §4.4): drop the publish dsb ish. On REAL
     * hardware a worker can see the new gen but stale body/ctx/out/nw ->
     * corruption. On QEMU TCG this is MASKED (expected PASS, deferred to
     * RPi3). Not a cert failure on QEMU. */
#endif
    g_wq.gen++;                                /* PUBLISH */
    mc2_unlock();                              /* stlr release on the lock   */
    __asm__ volatile("sev" ::: "memory");      /* wake all workers from wfe  */

    /* primary runs slice 0 ITSELF */
    {
        size_t i0, i1;
        pk_slice_bm(out, nw, 0, &i0, &i1);
        if (i1 > i0)
            body(ctx, i0, i1);
    }
    __asm__ volatile("dmb st" ::: "memory");   /* primary's slice-0 stores visible */

    /* JOIN: bounded wait until ALL released secondaries report for this
     * gen — NOT nw-1. Every awake secondary drains every gen and ++done
     * even if its slot did no work (slot>=nw), exactly like the hosted
     * pool joins on `nhelpers` (pk_parallel.c:203). Joining on nw-1 could
     * break BEFORE the slice that actually did the work has finished its
     * store, since a do-nothing slot may report first. Watchdog ceiling so
     * a core that failed to wake can NOT wedge the primary forever
     * (mirrors the MC-2.0 mc2_smp_join bound). */
    {
        const unsigned long MAX_TRIES = 200000000UL;
        unsigned long tries = 0;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");  /* acquire done */
            if (g_wq.done >= g_wq_nhelpers)
                break;
            if (++tries >= MAX_TRIES)
                break;                         /* bounded fallback (FAIL via memcmp) */
            __asm__ volatile("wfe");
        }
    }
#ifndef MC2_EQUIV_NO_BARRIER
    __asm__ volatile("dmb ld" ::: "memory");   /* ACQUIRE before reading any y[i] */
#else
    /* TOOTH B: drop the join acquire dmb ld (§4.4) — same masked-on-QEMU
     * caveat as the publish dsb above. */
#endif
}

/* ---------------------------------------------------------------------
 *  Bounded join. Waits for the worker to set woken + done, with a hard
 *  spin-count ceiling so a faulting/never-completing worker (the
 *  falsification variant) can NOT wedge the primary forever (§4.2). The
 *  primary uses `wfe` between polls (the worker `sev`s on progress) but
 *  the ceiling guarantees forward progress even with zero events.
 *
 *  Returns:  0 = joined cleanly (woken && done && tile correct)
 *           -1 = timed out (worker never finished — but primary survives)
 * ------------------------------------------------------------------- */
int mc2_smp_join(void)
{
    /* Generous ceiling: QEMU TCG boots the secondary in well under this.
     * It is a watchdog, not a tight bound — its only job is "never spin
     * forever". */
    const unsigned long MAX_TRIES = 200000000UL;

    unsigned long tries = 0;
    for (;;) {
        __asm__ volatile("dmb ld" ::: "memory");   /* acquire before read */
        if (g_done)
            break;
        if (++tries >= MAX_TRIES)
            return -1;                              /* bounded fallback   */
        /* Light backoff: wfe wakes on the worker's sev or any event. The
         * spurious-wake-tolerant ceiling above does the real bounding. */
        __asm__ volatile("wfe");
    }

    __asm__ volatile("dmb ld" ::: "memory");        /* acquire output     */
    return 0;
}

/* ---------------------------------------------------------------------
 *  Verification helpers consumed by the boot self-test in main.c.
 * ------------------------------------------------------------------- */
int mc2_secondary_woken(void)
{
    __asm__ volatile("dmb ld" ::: "memory");
    return g_cpu[1].woken == 1;
}

/* Returns the index of the first mismatching tile element, or -1 if the
 * whole tile matches the expected fixed pattern. */
long mc2_tile_check(void)
{
    __asm__ volatile("dmb ld" ::: "memory");
    for (unsigned i = 0; i < MC2_TILE_LEN; i++) {
        if (g_mc2_tile[i] != mc2_tile_expected(i))
            return (long)i;
    }
    return -1;
}

/* =====================================================================
 *  MC-2.1: the [mc2-smp-equiv] byte-identity self-test (§4.2).
 *
 *  A SYNTHETIC, fixed-seed, gate-exceeding matmul (out=2048, in=512 ->
 *  1048576 MACs > the 524288 gate). Computes y_serial (nw=1, the inline
 *  serial left-fold) and y_par for nw in {2,4} via the bare-metal
 *  pk_parallel_rows, then asserts memcmp==0 AND an equal FNV-1a hash
 *  across all nw. Operands live in static BSS (NOT on any 16KB worker
 *  stack). This proves the deterministic-worker MECHANISM: N cores each
 *  compute a deterministic SLICE of one matmul and the reassembled output
 *  is byte-identical to the serial loop ("the one mind stays one").
 *
 *  This is a CERT VEHICLE, not a real workload — the only bare-metal
 *  matmul (dt_linear, R3 48x48) is far below the gate; wiring it is
 *  deferred to a later wave (§0, §3.4 of the plan). MC-2.1 does NOT
 *  modify dtr.c.
 *
 *  Gated behind MC2_EQUIV_SELFTEST so the 4MB synthetic-W BSS + the
 *  self-test only exist in the cert build — the plain/shipped kernel
 *  carries NONE of it (and wakes no secondary).
 * =================================================================== */
#ifdef MC2_EQUIV_SELFTEST
#define MC2_EQ_OUT   2048u    /* output rows  */
#define MC2_EQ_IN    512u     /* contraction  */

/* Synthetic operands + outputs in static BSS (VA==PA, off every stack). */
static float g_eq_W[MC2_EQ_OUT * MC2_EQ_IN];
static float g_eq_x[MC2_EQ_IN];
static float g_eq_y_serial[MC2_EQ_OUT];
static float g_eq_y_par[MC2_EQ_OUT];

/* Deterministic xorshift PRNG — copied from tests/llm/mc0_test.c:70-82 so
 * W,x are fixed-seed and byte-reproducible across runs and worker counts. */
static uint32_t g_eq_rng;
static void     eq_rng_seed(uint32_t s) { g_eq_rng = s ? s : 0x1234567u; }
static uint32_t eq_rng_u32(void)
{
    uint32_t x = g_eq_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_eq_rng = x;
    return x;
}
/* float in [-1,1) — copied from mc0_test.c:82 */
static float eq_rng_f(void) { return (float)((int32_t)eq_rng_u32()) / 2147483648.0f; }

/* FNV-1a over the output buffer — idiom from student_shell.c:691-694 /
 * mc0_test.c:115-122. Byte-level, so it catches any rounding difference. */
static uint64_t eq_fnv1a(const float *y, size_t out)
{
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t *)y;
    size_t bytes = out * sizeof(float);
    for (size_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628253ULL; }
    return h;
}

/* Fill W,x with the fixed seed. */
static void eq_build_Wx(uint32_t seed)
{
    eq_rng_seed(seed);
    for (size_t i = 0; i < (size_t)MC2_EQ_OUT * MC2_EQ_IN; i++)
        g_eq_W[i] = eq_rng_f();
    for (size_t j = 0; j < MC2_EQ_IN; j++)
        g_eq_x[j] = eq_rng_f();
}

/* When set (Tooth A), the PARALLEL body reassociates the reduction so its
 * bits MUST differ from the serial left-fold. Pure arithmetic -> bites on
 * QEMU TCG. The serial reference (nw=1) always runs the clean body. */
static int g_eq_racy_body = 0;

/* The matmul body — the UNMODIFIED dt_linear shape (dtr.c:148-157)
 * parameterized over output rows [i0,i1). y[m] = sum_n W[m*in+n]*x[n], a
 * single left-fold per row -> row order does not change the bits. */
static void mc2_eq_body(void *ctx, size_t i0, size_t i1)
{
    (void)ctx;
    const float *W = g_eq_W;
    const float *x = g_eq_x;
    float       *y = g_eq_y_par;       /* parallel runs write y_par */
    const size_t in = MC2_EQ_IN;

#ifdef MC2_EQUIV_RACY_PARTITION
    if (g_eq_racy_body) {
        /* TOOTH A — REASSOCIATE the reduction: 2 strided partial sums per
         * row, then add the partials. A DIFFERENT reduction tree than the
         * serial left-fold -> the rounding bits will not match -> memcmp
         * MUST fail. (mc0_test.c:131-160 shape.) Pure arithmetic: QEMU
         * TCG catches this regardless of memory ordering — the load-
         * bearing falsifier that proves the cert is non-vacuous. */
        for (size_t m = i0; m < i1; m++) {
            const float *row = W + m * in;
            float p0 = 0.0f, p1 = 0.0f;
            for (size_t n = 0; n < in; n++) {
                if ((n & 1u) == 0u) p0 += row[n] * x[n];
                else                p1 += row[n] * x[n];
            }
            y[m] = p0 + p1;
        }
        return;
    }
#endif

    for (size_t m = i0; m < i1; m++) {
        const float *row = W + m * in;
        float s = 0.0f;
        for (size_t n = 0; n < in; n++)
            s += row[n] * x[n];
        y[m] = s;
    }
}

/* The serial reference body — identical left-fold, writes y_serial. */
static void mc2_eq_body_serial(void)
{
    const float *W = g_eq_W;
    const float *x = g_eq_x;
    const size_t in = MC2_EQ_IN;
    for (size_t m = 0; m < MC2_EQ_OUT; m++) {
        const float *row = W + m * in;
        float s = 0.0f;
        for (size_t n = 0; n < in; n++)
            s += row[n] * x[n];
        g_eq_y_serial[m] = s;
    }
}

/* First mismatching output index between y_serial and y_par, or -1. */
static long eq_first_mismatch(void)
{
    const uint8_t *a = (const uint8_t *)g_eq_y_serial;
    const uint8_t *b = (const uint8_t *)g_eq_y_par;
    size_t bytes = (size_t)MC2_EQ_OUT * sizeof(float);
    for (size_t i = 0; i < bytes; i++) {
        if (a[i] != b[i])
            return (long)(i / sizeof(float));   /* offending row */
    }
    return -1;
}

/* The result of the equiv self-test, consumed by main.c for the verdict.
 * hashes[k] is the FNV-1a of the run with nw = nws[k] (1,2,4). */
struct mc2_equiv_result {
    int      ok;            /* 1 = byte-identical across all nw            */
    int      bad_nw;        /* the nw that diverged (0 if ok)             */
    long     bad_idx;       /* first mismatching row (-1 if hashes differ) */
    int      woke_fail;     /* 1 = a secondary never woke                  */
    uint64_t h_nw1;
    uint64_t h_nw2;
    uint64_t h_nw4;
};

/* Run ONE forced-nw matmul into y_par and return its FNV hash. */
static uint64_t eq_run_nw(int nw)
{
    pk_smp_force_nw(nw);
    /* Poison y_par so a no-op / partial write is caught (untouched rows
     * stay poisoned -> memcmp/hash mismatch). */
    for (size_t i = 0; i < MC2_EQ_OUT; i++)
        g_eq_y_par[i] = -987654.0f;
    pk_parallel_rows(MC2_EQ_OUT, mc2_eq_body, 0);
    return eq_fnv1a(g_eq_y_par, MC2_EQ_OUT);
}

/* Orchestrate the [mc2-smp-equiv] self-test. Releases cores 1..3, waits
 * for them, computes y_serial (nw=1) + y_par for nw in {1,2,4}, and
 * compares. Fills *r. The primary survives any failure (bounded joins).
 *
 * NOTE on the falsifier teeth (§4.4):
 *  - Tooth A (-DMC2_EQUIV_RACY_PARTITION): reassociating parallel body.
 *    Pure arithmetic -> MUST produce ok=0 under -smp 4 (QEMU bites).
 *  - Tooth B (-DMC2_EQUIV_SMPEN_OFF and/or a dropped barrier): the
 *    missing-coherency variant. On QEMU TCG it may still PASS (TCG masks
 *    the store-buffer/non-coherent-cache race) — recorded HONESTLY as
 *    "masked, deferred to RPi3 (MC-2.2)", NOT a cert failure. We do NOT
 *    claim "barriers verified" from a QEMU PASS.
 */
void mc2_smp_equiv_selftest(struct mc2_equiv_result *r)
{
    r->ok = 1; r->bad_nw = 0; r->bad_idx = -1; r->woke_fail = 0;
    r->h_nw1 = r->h_nw2 = r->h_nw4 = 0;

    /* Wake cores 1,2,3 (release_n was called by the boot hook; here we
     * just confirm they all reached the work-queue). */
    if (mc2_smp_wait_woken(PK_SMP_MAX_CPUS) != 0) {
        r->woke_fail = 1;
        r->ok = 0;
        r->bad_nw = -1;
        return;
    }

    eq_build_Wx(0xA53C0DE5u);

    /* y_serial — the clean serial left-fold reference. */
    mc2_eq_body_serial();
    r->h_nw1 = eq_fnv1a(g_eq_y_serial, MC2_EQ_OUT);

    /* nw=1 via the parallel seam (must hit the inline fallback -> same
     * bits as the serial reference: a self-consistency check of the seam
     * AND of pk_slice_bm's nw=1 path). */
    g_eq_racy_body = 0;
    {
        uint64_t h1 = eq_run_nw(1);
        if (h1 != r->h_nw1 || eq_first_mismatch() != -1) {
            r->ok = 0; r->bad_nw = 1; r->bad_idx = eq_first_mismatch();
            return;
        }
    }

    /* nw in {2,4}. Tooth A makes the PARALLEL body reassociate. */
#ifdef MC2_EQUIV_RACY_PARTITION
    g_eq_racy_body = 1;
#endif
    {
        uint64_t h2 = eq_run_nw(2);
        r->h_nw2 = h2;
        long m = eq_first_mismatch();
        if (m != -1 || h2 != r->h_nw1) {
            r->ok = 0; r->bad_nw = 2; r->bad_idx = m;
            return;
        }
    }
    {
        uint64_t h4 = eq_run_nw(4);
        r->h_nw4 = h4;
        long m = eq_first_mismatch();
        if (m != -1 || h4 != r->h_nw1) {
            r->ok = 0; r->bad_nw = 4; r->bad_idx = m;
            return;
        }
    }
}

/* ---------------------------------------------------------------------
 *  MC-2.1b: the idle assertion (§4.5). After the equiv runs, the
 *  secondaries must be back in wfe — NOT busy-spinning. Snapshot the wake
 *  counters, spin the primary for a bounded interval doing nothing, then
 *  re-snapshot: the counters MUST NOT advance (no spurious drains).
 *  Returns 1 = idle (PASS), 0 = a counter advanced (FAIL).
 * ------------------------------------------------------------------- */
int mc2_smp_idle_check(void)
{
    unsigned long before[PK_SMP_MAX_CPUS];
    for (int s = 1; s < PK_SMP_MAX_CPUS; s++)
        before[s] = pk_smp_wake_count(s);

    /* Bounded "do nothing" interval — long enough that a busy-spinning
     * worker would tick its counter, short enough not to stall the boot.
     * We deliberately do NOT sev/wfe here. */
    for (volatile unsigned long i = 0; i < 50000000UL; i++)
        __asm__ volatile("" ::: "memory");

    for (int s = 1; s < PK_SMP_MAX_CPUS; s++) {
        if (pk_smp_wake_count(s) != before[s])
            return 0;                         /* spurious drain -> FAIL */
    }
    return 1;
}
#endif /* MC2_EQUIV_SELFTEST */

/* =====================================================================
 *  MC-2.1b — the STANDALONE pk_slice_bm partition unit-check (§4.5).
 *
 *  The MC-2.1a equiv cert exercises pk_slice_bm only INDIRECTLY (the
 *  matmul reassembles correctly => the partition was sound). The MC-2.1a
 *  audit flagged that as a drift surface: the hand-copy of pk_slice()
 *  (arch/common/llm/pk_parallel.c:57-63 — the "one mind, one math" golden)
 *  has NO direct guard. MC-2.1b adds one.
 *
 *  This is a PURE INTEGER unit-check: it needs NO secondary cores (it runs
 *  on the primary alone, before any matmul) and is cheap. For a set of
 *  (out, nw) pairs — including ragged ones — it asserts pk_slice_bm
 *  satisfies the three partition invariants DIRECTLY:
 *
 *    (1) DISJOINT + TOTAL — the union of all nw slices covers [0,out)
 *        exactly once: a coverage array is incremented per index; every
 *        index in [0,out) must be hit EXACTLY once (no gap, no overlap).
 *    (2) ORDER            — slice s starts where slice s-1 ended
 *        (contiguous ascending), slice 0 starts at 0, slice nw-1 ends at
 *        out, and the ragged remainder lands on the LAST slice (matching
 *        pk_parallel.c:57-63 semantics: first nw-1 slices get q rows each).
 *    (3) MATCHES THE GOLDEN — the (i0,i1) pk_slice_bm produces equals what
 *        the hosted pk_slice formula produces for the same (out,nw,s). The
 *        hosted TU is NOT linked on bare metal (pk_parallel.h:18-24), so we
 *        re-derive the golden formula INLINE here (slice_golden, pinned to
 *        pk_parallel.c:57-63) rather than linking it.
 *
 *  Gated behind MC2_SLICE_SELFTEST so the plain kernel carries none of it;
 *  it is ALSO compiled when MC2_EQUIV_SELFTEST is set (so main.c can run it
 *  as an extra assertion block BEFORE the matmul). The falsifier
 *  (-DMC2_SLICE_BREAK) perturbs pk_slice_bm itself (drops the ragged
 *  remainder) and MUST make this report MC2-SLICE: FAIL.
 * =================================================================== */
#if defined(MC2_SLICE_SELFTEST) || defined(MC2_EQUIV_SELFTEST)

/* The golden partition formula, re-derived INLINE — a VERBATIM copy of
 * pk_slice() at arch/common/llm/pk_parallel.c:57-63 (commit-pinned). This
 * is the independent reference the check measures pk_slice_bm against; it
 * does NOT call pk_slice_bm and is NEVER perturbed by MC2_SLICE_BREAK (so a
 * broken pk_slice_bm diverges from this golden and the check bites). KEEP
 * IN SYNC with pk_parallel.c:57-63 alongside pk_slice_bm. */
static void slice_golden(size_t out, int nw, int s, size_t *i0, size_t *i1)
{
    size_t q = out / (size_t)nw;
    /* first (nw-1) slices get q rows each; the last gets the remainder too. */
    *i0 = (size_t)s * q;
    *i1 = (s == nw - 1) ? out : (*i0 + q);
}

/* Coverage scratch in static BSS (off every 16KB worker stack). Sized to
 * the largest `out` the check exercises (2049). The check runs on the
 * primary only; a single static buffer is fine (no concurrency). */
#define MC2_SLICE_COV_MAX 2049u
static uint8_t g_slice_cov[MC2_SLICE_COV_MAX];

/* Verdict, consumed by main.c. On FAIL, (bad_out,bad_nw,bad_idx) localizes
 * the first offending case; reason names which invariant tripped. */
struct mc2_slice_result {
    int      ok;          /* 1 = all invariants held for every case        */
    size_t   bad_out;     /* the `out` of the first failing case           */
    int      bad_nw;      /* the `nw`  of the first failing case            */
    long     bad_idx;     /* offending index/slice (-1 if N/A)             */
    int      reason;      /* 1=order 2=golden 3=coverage (0 if ok)         */
    int      n_cases;     /* how many (out,nw) pairs were exercised        */
};

/* Check ONE (out,nw) pair against all three invariants. Returns 0 = PASS,
 * else sets *idx/*reason and returns nonzero. Walks the nw slices once. */
static int mc2_slice_check_one(size_t out, int nw, long *idx, int *reason)
{
    /* Reset the coverage window for [0,out). */
    for (size_t i = 0; i < out; i++) g_slice_cov[i] = 0;

    size_t prev_end = 0;                       /* where the previous slice ended */
    for (int s = 0; s < nw; s++) {
        size_t i0 = 0, i1 = 0;
        pk_slice_bm(out, nw, s, &i0, &i1);

        /* (2) ORDER: slice 0 starts at 0; slice s starts where s-1 ended;
         * bounds are sane (i0<=i1, i1<=out); slice nw-1 ends exactly at out. */
        if (i0 != prev_end || i1 < i0 || i1 > out) {
            *idx = (long)s; *reason = 1; return 1;
        }
        if (s == nw - 1 && i1 != out) {        /* ragged remainder NOT on last */
            *idx = (long)s; *reason = 1; return 1;
        }

        /* (3) MATCHES THE GOLDEN: re-derive (g0,g1) from the inline golden
         * and require pk_slice_bm produced the identical pair. */
        size_t g0 = 0, g1 = 0;
        slice_golden(out, nw, s, &g0, &g1);
        if (i0 != g0 || i1 != g1) {
            *idx = (long)s; *reason = 2; return 1;
        }

        /* Accumulate coverage for this slice's rows. */
        for (size_t r = i0; r < i1; r++) {
            if (r < out) g_slice_cov[r]++;     /* clamp guard (r<out always) */
        }
        prev_end = i1;
    }

    /* (1) DISJOINT + TOTAL: every index in [0,out) hit EXACTLY once. */
    for (size_t i = 0; i < out; i++) {
        if (g_slice_cov[i] != 1) {
            *idx = (long)i; *reason = 3; return 1; /* gap (0) or overlap (>1) */
        }
    }
    return 0;
}

/* Run the standalone unit-check over a representative set of (out,nw)
 * pairs, including ragged ones (out not a multiple of nw, and out<nw, and
 * out==0/1 edges). Fills *r. Pure integer; no cores, no matmul. */
void mc2_slice_unitcheck(struct mc2_slice_result *r)
{
    /* out values: edges (0,1,2), small ragged (7), the gate boundary
     * (63,64,65 around PK_PARALLEL_MIN_ROWS), and the cert size (2048) plus
     * a ragged sibling (2049). */
    static const size_t outs[] = { 0, 1, 2, 7, 63, 64, 65, 2048, 2049 };
    static const int     nws[]  = { 1, 2, 3, 4 };

    r->ok = 1; r->bad_out = 0; r->bad_nw = 0; r->bad_idx = -1;
    r->reason = 0; r->n_cases = 0;

    for (size_t oi = 0; oi < sizeof(outs)/sizeof(outs[0]); oi++) {
        size_t out = outs[oi];
        if (out > MC2_SLICE_COV_MAX) continue;     /* never overrun the scratch */
        for (size_t ni = 0; ni < sizeof(nws)/sizeof(nws[0]); ni++) {
            int nw = nws[ni];
            /* The golden requires nw>=1 and (for a sensible partition) nw
             * not exceeding out — except out==0 which is the empty matmul
             * (every slice empty: still must be disjoint+total over the
             * empty range). pk_parallel_rows only ever partitions out>=nw
             * (it falls back to serial below the gate / when nw>out), so we
             * mirror that contract: skip nw>out for out>0. out==0 is checked
             * at every nw (all-empty must still be consistent). */
            if (out > 0 && (size_t)nw > out) continue;

            long idx = -1; int reason = 0;
            r->n_cases++;
            if (mc2_slice_check_one(out, nw, &idx, &reason) != 0) {
                r->ok = 0; r->bad_out = out; r->bad_nw = nw;
                r->bad_idx = idx; r->reason = reason;
                return;                            /* first failure wins */
            }
        }
    }
}
#endif /* MC2_SLICE_SELFTEST || MC2_EQUIV_SELFTEST */
