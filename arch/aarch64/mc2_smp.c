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

/* Linker symbol: the secondary's dedicated stack top (linker.ld). */
extern unsigned char _stack_top_cpu1[];

/* The assembly landing pad the secondary is released to (start.S). */
extern void _secondary_worker(void);

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
    unsigned long v;
    __asm__ volatile("mrs %0, S3_1_C15_C2_1" : "=r"(v));
    v |= (1UL << 6);
    __asm__ volatile("msr S3_1_C15_C2_1, %0" : : "r"(v));
    __asm__ volatile("isb" ::: "memory");
}

/* ---------------------------------------------------------------------
 *  pk_smp_worker_loop — the C worker body. Entered by the secondary from
 *  _secondary_worker (start.S) AFTER its EL1 setup. NEVER returns to a
 *  T-Kernel context; never touches the scheduler.
 * ------------------------------------------------------------------- */
void pk_smp_worker_loop(void)
{
    /* SMPEN belt-and-braces: _secondary_el1_setup already set it in asm,
     * but re-asserting from C is harmless and keeps the C path honest. */
    mc2_set_smpen();

    /* Mark this core awake (slot 1 for MC-2.0's single secondary). */
    g_cpu[1].woken = 1;
    __asm__ volatile("dmb st" ::: "memory");   /* publish woken */
    __asm__ volatile("sev" ::: "memory");      /* poke the primary's join */

    /* Run the ONE trivial deterministic tile. */
    if (!g_fault_tile) {
        for (unsigned i = 0; i < MC2_TILE_LEN; i++)
            g_mc2_tile[i] = mc2_tile_expected(i);
    } else {
        /* Falsification variant: deliberately NEVER complete the tile or
         * the done signal — emulate a worker that faulted mid-tile. The
         * primary's bounded join (§4.2) must NOT wedge forever. */
        for (;;)
            __asm__ volatile("wfe");
    }

    /* RELEASE the tile stores, then publish done under the lock. */
    __asm__ volatile("dmb st" ::: "memory");   /* worker's stores visible */
    mc2_lock();
    g_done = 1;
    mc2_unlock();
    __asm__ volatile("sev" ::: "memory");      /* wake the primary's join */

    /* Park on wfe — NO busy-spin (§5). MC-2.1 turns this into the
     * gen-driven work-queue drain; MC-2.0 stops after one tile. */
    for (;;)
        __asm__ volatile("wfe");
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
 * proceed; the join happens in mc2_smp_join(). */
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
