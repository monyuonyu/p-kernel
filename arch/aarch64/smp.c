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

/* Announce THIS secondary's dispatcher entry with its real CPU id, e.g.
 * "[SMP] cpu2 entered dispatcher\r\n" (②.1b: N=4 wakes cpus 1,2,3 — a single
 * hardcoded "cpu1" would lie about which core spoke). The blind writer above
 * has no formatter, so we splice the single id digit (0-9) inline. */
static void smp_dbg_cpu_entered(unsigned long cpu, const char *suffix)
{
    volatile unsigned int *dr = (volatile unsigned int *)SMP_UART_DR;
    const char *p = "[SMP] cpu";
    for (; *p; p++) *dr = (unsigned int)(unsigned char)*p;
    *dr = (unsigned int)('0' + (cpu % 10));
    for (; *suffix; suffix++) *dr = (unsigned int)(unsigned char)*suffix;
}

/* Number of CPUs in this slice. Defined HERE, before the ②.1a GIC block
 * that sizes its per-CPU arrays by it (and reused by the per-CPU SMP block
 * below). ②.1b generalized the run/mutex certs from 2 → 4 CPUs; ②.N8 now
 * scales them to 8 PHYSICAL cores (boot CPU + SEVEN secondaries, woken via
 * PSCI CPU_ON cores 1..7) — the real phone target — to prove the BKL +
 * per-CPU dispatch + SGI scale to the GICv2 ceiling. The shipped
 * uniprocessor kernel is untouched (all of this is SMP_SELFTEST-gated).
 *
 * MUST stay identical to SMP_MAX_CPUS in arch/aarch64/include/smp_percpu.h
 * (both define struct smp_cpu + the array size). The struct LAYOUT is
 * UNCHANGED (SMPCPU_SIZE stays 72; only the array COUNT grows 4 -> 8) so
 * cpu_support.S/start.S offsets are untouched.
 *
 * GICv2 CEILING: 8 is the maximum GICv2 supports — the GICD_SGIR target
 * list is 8 bits (1 per CPU), so SGI delivery (smp_send_reschedule) can
 * address at most CPUs 0..7. Going beyond 8 would need GICv3 (a separate
 * lift). */
#define SMP_MAX_CPUS   8          /* ②.N8 = boot CPU + SEVEN secondaries (GICv2 ceiling) */

/* ─── GICv2 SGI/IPI registers (②.1a) — header-light local #defines, mirror
 * of tkdev_conf.h:23-39. QEMU-virt GICv2 ONLY (RPi3 uses the BCM2837
 * mailbox, not GICD_SGIR — guarded below). ──────────────────────────── */
#ifdef BOARD_RPI3
/* RPi3 has NO GICD_SGIR; the cross-CPU IPI is the BCM2837 per-core mailbox
 * (deferred [live] follow-up, §4.4/§6 of the plan). Defining these here for
 * a BOARD_RPI3 build would be WRONG — so we do NOT, and smp_send_reschedule
 * compiles to a hard no-op + a clear marker. */
#else
#  define SMP_GICD_BASE   0x08000000UL    /* QEMU virt GICv2 distributor   */
#  define SMP_GICC_BASE   0x08010000UL    /* QEMU virt GICv2 CPU interface  */
#  define SMP_GICD_CTLR   0x000
#  define SMP_GICD_TYPER  0x004           /* GIC Distributor Type Register  */
#  define SMP_GICD_ISENABLER 0x100        /* GICD_ISENABLERn (PPI/SPI enable) */
#  define SMP_GICD_SGIR   0xF00
#  define SMP_GICC_CTLR   0x000
#  define SMP_GICC_PMR    0x004
#endif

/* ②.2b-ii — the EL1 physical-timer PPI (the per-CPU generic-timer interrupt).
 * INTID 30, identical to tkdev_conf.h:INTNO_TIMER_GIC / the boot CPU's timer.
 * The handler at knl_intvec[30] (timer_irq_handler) is GLOBAL — once the
 * secondary's PPI 30 is enabled + its CNTP armed, it runs UNCHANGED on the
 * secondary.  PPI bits in GICD_ISENABLER0 are PER-CPU-BANKED on GICv2 (§5.3). */
#define SMP_TIMER_PPI     30U

#define SMP_RESCHED_SGI   0U              /* SGI INTID 0 = "reschedule" */

/* ════════════════════════════════════════════════════════════════════
 *  RUNTIME CPU-COUNT AUTODETECT (slice 1 — device-autodetect-plan.md)
 *
 *  「デバイスのスペックを測って自動で合わしたい」 — measure the device, not
 *  hardcode it. SMP_MAX_CPUS (8) stays the COMPILE-TIME ARRAY CEILING (it
 *  sizes g_smpcpu[], the per-CPU flag arrays, the stacks). The RUNTIME active
 *  count g_smp_ncpu is decided ONCE at boot from GICD_TYPER, and EVERY loop
 *  over live cores (bringup / join / barrier / cert) reads g_smp_ncpu instead
 *  of the hardcoded ceiling. So the SAME binary wakes 1 secondary under
 *  -smp 2, 3 under -smp 4, 7 under -smp 8 — no recompile.
 *
 *  THE DECODE. GICD_TYPER bits[7:5] = CPUNumber = (#CPU interfaces)-1 for
 *  GICv2, so detected = ((TYPER>>5)&0x7)+1, range 1..8. On QEMU virt the GIC
 *  instantiates exactly as many CPU interfaces as -smp N requests (N<=8), so
 *  this returns N — the mechanism that makes the binary adapt. One
 *  non-destructive MMIO load, zero firmware dependency.
 *
 *  HONEST LIMITATION (RPi3). GICD_TYPER is GICv2/QEMU-virt-CLEAN. RPi3's
 *  interrupt block is the BCM2837 ARM Local Interrupt Controller — NOT a GIC
 *  (tkdev_init.c documents this: "There is no distributor or CPU interface").
 *  Under BOARD_RPI3 there is no GICD_TYPER to read, so smp_detect_ncpu()
 *  returns a documented build-constant (4 = the BCM2837's fixed core count).
 *  The DTB /cpus node is the canonical source there and is DEFERRED (it needs
 *  x0 saved at _start + an FDT parser; device-autodetect-plan.md §1.3).
 *
 *  FALSIFIER (-DSMP_FORCE_NCPU=N): ignore GICD_TYPER, hardcode N. Then a run
 *  on a SMALLER -smp count tries to wake cores that do not exist → it HANGS /
 *  FAILs (smp_wait_secondary_live / the join watchdog time out waiting for the
 *  absent cores) → proves the detection is load-bearing.
 * ════════════════════════════════════════════════════════════════════ */

/* The RUNTIME active CPU count, decided once at boot (default 1 until detect
 * runs). NOT the ceiling: it is always <= SMP_MAX_CPUS. */
static unsigned int g_smp_ncpu = 1;

/* Read GICD_TYPER bits[7:5] → (CPUNumber+1) = number of CPU interfaces, then
 * clamp to [1, SMP_MAX_CPUS] (the array ceiling). On BOARD_RPI3 (no GIC) and
 * under -DSMP_FORCE_NCPU the GICD_TYPER read is bypassed. */
unsigned int smp_detect_ncpu(void)
{
#if defined(SMP_FORCE_NCPU)
    /* FALSIFIER: ignore the hardware, force the count. A run under a SMALLER
     * -smp will then wait for cores that don't exist and the watchdog FAILs. */
    unsigned int n = (unsigned int)(SMP_FORCE_NCPU);
#elif defined(BOARD_RPI3)
    /* No GICD_TYPER on the BCM2837 local controller; the core count is the
     * fixed BCM2837 4 (DTB /cpus is the canonical source, DEFERRED). */
    unsigned int n = 4u;
#else
    unsigned int typer = *(volatile unsigned int *)(SMP_GICD_BASE + SMP_GICD_TYPER);
    unsigned int n = ((typer >> 5) & 0x7u) + 1u;   /* CPUNumber+1, range 1..8 */
#endif
    if (n < 1u) n = 1u;
    if (n > SMP_MAX_CPUS) n = SMP_MAX_CPUS;         /* clamp to the array ceiling */
    return n;
}

/* Decide the runtime active count ONCE, before bringup. Idempotent. */
static void smp_set_ncpu(void)
{
    g_smp_ncpu = smp_detect_ncpu();
}

/* The runtime active CPU count getter (the cert reads N from here, so the
 * driver and the kernel can never disagree on how many cores were woken). */
unsigned int smp_ncpu(void) { return g_smp_ncpu; }

/* The interrupt-handler table the production IRQ vector dispatches through
 * (cpu_support.S:264-269: knl_intvec[INTID] blr). FP == void(*)() (typedef.h).
 * We register knl_intvec[0] directly (header-light) for the self-test window;
 * knl_cpu_initialize zeroes this table only LATER, inside knl_t_kernel_main,
 * which the self-test runs BEFORE (so our slot survives the cert). */
extern void (*knl_intvec[])(void);
/* gicc_base_ptr (cpu_support.S:357) — the IRQ vector reads GICC_IAR/EOIR
 * through it. gic_init normally sets it (tkdev_init.c:96) DURING T-Kernel
 * boot — i.e. AFTER our self-test — so we must set it ourselves before any
 * SGI can be taken, else the vector reads IAR from address 0. */
extern unsigned long gicc_base_ptr;

/* ════════════════════════════════════════════════════════════════════
 *  THE GIC SGI / IPI PATH (②.1a) — the core new work.
 *
 *  SEND:    smp_send_reschedule(cpu) writes GICD_SGIR to deliver SGI 0 to
 *           ONE target CPU. dsb ish before (so the readied high-prio task
 *           is globally visible before the interrupt) + after (push the
 *           MMIO write). No-op under -DSMP_NO_IPI (the load-bearing
 *           falsifier) so B never preempts.
 *  RECEIVE: smp_gic_cpuif_init() enables THIS CPU's banked CPU interface
 *           (GICC_PMR/GICC_CTLR); smp_resched_sgi_handler (knl_intvec[0])
 *           runs on the target, sets g_resched_pending[me]; the existing
 *           _vec_el1_irq EOIRs. (Distributor enable + DAIF unmask: §2.5,
 *           done by the driver / the low-prio task.)
 * ════════════════════════════════════════════════════════════════════ */

/* Per-CPU "a reschedule was requested on me" flag (BSS, VA==PA). One writer
 * (the SGI handler on that CPU), one reader (the dispatcher on that CPU) →
 * a dmb suffices, no BKL (§5.4). */
volatile unsigned int g_resched_pending[SMP_MAX_CPUS];
/* Per-CPU count of SGIs taken (observability; proves an SGI was delivered). */
volatile unsigned int g_sgi_taken[SMP_MAX_CPUS];

/* Send the reschedule SGI to ONE target CPU (cpu = MPIDR Aff0 / CPU-interface
 * index). GICv2 GICD_SGIR: TargetListFilter=0 (use list) | (1<<cpu)<<16 |
 * sgi_id. */
void smp_send_reschedule(int cpu)
{
#if defined(SMP_NO_IPI) || defined(BOARD_RPI3)
    /* FALSIFIER (-DSMP_NO_IPI): the send is a no-op → B never gets the SGI →
     * no preempt → the watchdog reports SMP-PREEMPT: FAIL, proving the IPI is
     * load-bearing. (BOARD_RPI3 also no-ops: GICD_SGIR is QEMU-virt only.) */
    (void)cpu;
#else
    unsigned int val = ((1u << (unsigned)cpu) << 16) | (SMP_RESCHED_SGI & 0xF);
    /* The target's handler/dispatcher will read the shared ready list; the
     * readied high-prio task must be visible to it BEFORE the interrupt. */
    __asm__ volatile("dsb ish" ::: "memory");
    *(volatile unsigned int *)(SMP_GICD_BASE + SMP_GICD_SGIR) = val;
    __asm__ volatile("dsb ish" ::: "memory");   /* push the MMIO write out */
#endif
}

/* Per-CPU GIC CPU-interface enable. Writes ONLY this CPU's banked
 * GICC_PMR/GICC_CTLR (mirrors tkdev_init.c:105-106 for the local core) — it
 * does NOT touch the shared distributor (the boot CPU owns GICD_CTLR). On a
 * secondary this is the only way it can receive ANY interrupt. */
void smp_gic_cpuif_init(void)
{
#if !defined(BOARD_RPI3)
    *(volatile unsigned int *)(SMP_GICC_BASE + SMP_GICC_PMR)  = 0xFFu; /* allow all */
    *(volatile unsigned int *)(SMP_GICC_BASE + SMP_GICC_CTLR) = 1u;    /* enable    */
    __asm__ volatile("dsb ish; isb" ::: "memory");
#endif
}

/* SGI handler — runs on the TARGET CPU in IRQ context via knl_intvec[0]
 * (cpu_support.S:264-269 blr). ABI-minimal leaf void(void): the existing
 * vector save_caller_regs'd, stashed the IAR, and EOIRs after we return. We
 * ONLY set a per-CPU flag (no shared-state mutation, no nested call needing
 * the stashed IAR, no sp games below the vector's reserved slot) — the
 * safest possible shape past the recurring aarch64 IRQ-path C-ABI trap. The
 * actual re-dispatch happens on the dispatcher's next checkpoint (§2.4). */
void smp_resched_sgi_handler(void)
{
    unsigned long me = smp_mpidr_aff0();
    if (me < SMP_MAX_CPUS) {
        g_sgi_taken[me]++;
        g_resched_pending[me] = 1u;
    }
    __asm__ volatile("dmb ish" ::: "memory");
}

/* ②.2b-i: the IRQ-return-path async-preempt DECISION (smp_irq_need_resched)
 * is defined LOWER in this file — after struct smp_cpu / g_smpcpu[] and the
 * BKL globals (g_bkl_owner) it reads — see "②.2b-i — the IRQ-return-path
 * async-preempt DECISION" below the BKL block. */

/* Distributor + boot-CPU-interface enable + SGI handler registration, run
 * once by the driver BEFORE releasing the secondary (§2.5 steps 1-3). The
 * self-test runs before knl_t_kernel_main, so NONE of gic_init's state
 * exists yet — we stand it up ourselves, idempotently with the later
 * gic_init (which re-sets GICD_CTLR=1 / gicc_base_ptr harmlessly at boot). */
void smp_gic_selftest_setup(void)
{
#if !defined(BOARD_RPI3)
    /* (1) Distributor enable (shared block; idempotent with gic_init:99). */
    *(volatile unsigned int *)(SMP_GICD_BASE + SMP_GICD_CTLR) = 1u;
    /* (2) Register the SGI handler in knl_intvec[0] (header-light direct
     *     write; knl_define_inthdr does the same, cpu_insn.h:53-57). */
    knl_intvec[SMP_RESCHED_SGI] = smp_resched_sgi_handler;
    /* (2b) gicc_base_ptr — the IRQ vector reads IAR/EOIR through it; gic_init
     *      sets it only at T-Kernel boot (later). Set it now or the vector
     *      reads GICC_IAR from address 0 when an SGI fires. */
    gicc_base_ptr = SMP_GICC_BASE;
    /* SGI ids 0-15 are always-enabled at the GICv2 distributor (the
     * GICD_ISENABLER0 SGI bits are fixed-enabled) — no GICD_ISENABLER write
     * needed for id 0, unlike the timer PPI. */
    /* (3) Enable the boot CPU's OWN CPU interface. */
    smp_gic_cpuif_init();
    __asm__ volatile("dsb ish; isb" ::: "memory");
#endif
}

/* Unmask / re-mask IRQ (and FIQ) in DAIF on the calling CPU (§2.5 step 5) —
 * scoped to the cert window so the receiving CPU can actually TAKE the SGI.
 * Gated; never leaks into production (the whole TU is SMP_SELFTEST). */
static inline void smp_irq_unmask(void)
{
    __asm__ volatile("msr daifclr, #0x3; isb" ::: "memory"); /* clear I+F */
}
static inline void smp_irq_mask(void)
{
    __asm__ volatile("msr daifset, #0x3; isb" ::: "memory"); /* set   I+F */
}

/* ─── The per-CPU SMP block (this file's OWN; §DECOUPLING). The field
 * OFFSETS are mirrored in cpu_support.S via the SMPCPU_* macros below —
 * KEEP IN SYNC (a _Static_assert guards it). SMP_MAX_CPUS is #defined
 * above (moved up for the ②.1a GIC arrays). ─────────────────────────── */
struct smp_cpu {
    void          *ctxtsk;        /* off 0:  this CPU's running task (TCB*)  */
    void          *schedtsk;      /* off 8:  this CPU's next task    (TCB*)  */
    unsigned long  exec_count;    /* off 16: per-CPU dispatch/exec counter   */
    unsigned long  cpu_id;        /* off 24: MPIDR Aff0                       */
    volatile unsigned long live;  /* off 32: set 1 when CPU enters dispatcher*/
    /* ②.1a cross-CPU preempt observability (appended — KEEPS off 0/8/16
     * fixed so the asm SMPCPU_CTXTSK/SCHEDTSK mirror is unperturbed; only
     * SMPCPU_SIZE grows, mirrored in cpu_support.S). */
    volatile unsigned long preempted_at; /* off 40: iter at which the SGI was
                                          *         observed (0 = never)       */
    volatile unsigned long highprio_ran; /* off 48: 1 = the high-prio task ran */
    /* ②.1b: each secondary needs its OWN dispatcher stack (N=4 wakes THREE
     * secondaries — a single shared stack would corrupt). The boot CPU stores
     * &_stack_top_cpuN here before CPU_ON; start.S's secondary landing pad
     * loads SP from THIS field via the context_id (x19 = &g_smpcpu[cpu]).
     * Appended AFTER the ②.1a fields so off 0/8/16 stay fixed. */
    unsigned long stack_top;             /* off 56: this CPU's dispatcher SP   */
    /* ②.2a: per-CPU dispatch-disable flag (the production knl_dispatch_disabled
     * goes per-CPU; §2.2).  Appended AFTER ②.1b's stack_top.  This layout MUST
     * match struct smp_cpu in arch/aarch64/include/smp_percpu.h (the typed view
     * the kernel-common CUR_* macros index) — both index the SAME g_smpcpu[]
     * storage; a desync would read the wrong offset.  ②.1b/②.2a MERGE: both
     * appended a field; stack_top@56, dispatch_disabled@64, SMPCPU_SIZE 64->72. */
    int                    dispatch_disabled; /* off 64                        */
};

#define SMPCPU_CTXTSK     0
#define SMPCPU_SCHEDTSK   8
#define SMPCPU_EXEC       16
#define SMPCPU_STACK_TOP  56          /* ②.1b: per-CPU secondary stack top    */
#define SMPCPU_DISPDIS    64          /* ②.2a: per-CPU dispatch-disable flag  */
#define SMPCPU_SIZE       72          /* 56 base + 8 stack_top + 4 dispdis +pad */

_Static_assert(offsetof(struct smp_cpu, ctxtsk)   == SMPCPU_CTXTSK,   "smp_cpu.ctxtsk");
_Static_assert(offsetof(struct smp_cpu, schedtsk) == SMPCPU_SCHEDTSK, "smp_cpu.schedtsk");
_Static_assert(offsetof(struct smp_cpu, exec_count)==SMPCPU_EXEC,     "smp_cpu.exec");
_Static_assert(offsetof(struct smp_cpu, stack_top)== SMPCPU_STACK_TOP,"smp_cpu.stack_top");
_Static_assert(offsetof(struct smp_cpu, dispatch_disabled)==SMPCPU_DISPDIS, "smp_cpu.dispdis");
_Static_assert(sizeof(struct smp_cpu)             == SMPCPU_SIZE,     "smp_cpu.size");

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

/* ── TOCTOU fix (②.2b deadlock audit) ────────────────────────────────────
 *  Save+mask / restore IRQ+FIQ on THIS CPU, header-light (this TU avoids
 *  cpu_insn.h).  Used to close the two TOCTOU windows in bkl_acquire/release
 *  where the raw lock is held but g_bkl_owner != me (the inconsistent state
 *  the §5.4 guard would mis-read).  Mirrors the production disint()/enaint()
 *  discipline (cpu_insn.h:18-35): BEGIN_CRITICAL_SECTION masks IRQ *first*,
 *  then takes the lock — so the SGI handler can never observe the kernel
 *  mid-publish.  We replicate that ordering ONLY across the publish/clear
 *  windows, NOT the whole critical section (the deadlock cert deliberately
 *  holds the BKL with IRQs UNMASKED so the §5.4 guard is exercised). */
static inline unsigned long smp_bkl_di(void)
{
    unsigned long daif;
    __asm__ volatile("mrs %0, daif\n\t"
                     "msr daifset, #0x3"   /* mask I+F */
                     : "=r"(daif) :: "memory");
    return daif;
}
static inline void smp_bkl_ei(unsigned long daif)
{
    /* DAIF bit 7 = I (masked when 1); only re-unmask if the caller had it on. */
    if (!(daif & (1UL << 7)))
        __asm__ volatile("msr daifclr, #0x3" ::: "memory");
}

/* Acquire the BKL (recursive). Returns nothing; pairs with bkl_release.
 *
 *  TOCTOU WINDOW 1 (closed below): the raw spinlock is taken by raw_lock()
 *  BEFORE g_bkl_owner is published.  In that gap THIS CPU physically holds the
 *  raw lock but g_bkl_owner still reads the OLD value (-1 or a stale owner).
 *  An SGI landing here would run smp_irq_need_resched(), see g_bkl_owner != me,
 *  decide the §5.4 guard does NOT apply, and async-switch AWAY while we hold the
 *  raw lock → the switched-to context can never acquire the BKL → DEADLOCK.
 *  FIX: mask IRQ on this CPU from BEFORE raw_lock() until AFTER g_bkl_owner is
 *  published (the dsb makes it globally visible), then restore the caller's
 *  prior IRQ state.  The async-switch decision can now only be taken when
 *  g_bkl_owner is already consistent with raw-lock ownership. */
void bkl_acquire(void)
{
    long me = (long)smp_mpidr_aff0();

    /* Already mine? Just bump depth — no raw re-acquire (no self-deadlock). */
    if (g_bkl_owner == me) {
        g_bkl_depth++;
        return;
    }

    unsigned long di = smp_bkl_di();   /* mask IRQ across the publish window */
    raw_lock(&g_bkl_lock);             /* spin/wfe until we own the raw lock */
    g_bkl_owner = me;                  /* publish ownership UNDER the lock    */
    g_bkl_depth = 1;
    __asm__ volatile("dsb ish" ::: "memory");  /* publish before IRQ can fire */
    smp_bkl_ei(di);                    /* restore caller's IRQ state          */
}

/* Release the BKL.
 *
 *  TOCTOU WINDOW 2 (closed below): on the outermost release we clear
 *  g_bkl_owner = -1 BEFORE raw_unlock() drops the raw spinlock.  In that gap
 *  g_bkl_owner reads -1 (not me) but THIS CPU still physically holds the raw
 *  lock.  An SGI landing here would see g_bkl_owner != me, skip the §5.4 guard,
 *  and async-switch away while we still hold the raw lock → DEADLOCK, identical
 *  to window 1.  FIX: mask IRQ from BEFORE g_bkl_owner is cleared until AFTER
 *  raw_unlock() has actually released the raw lock, then restore. */
void bkl_release(void)
{
    /* Only the owner releases; depth gates the raw unlock. */
    if (g_bkl_depth == 1) {
        unsigned long di = smp_bkl_di();   /* mask IRQ across the clear window */
        __asm__ volatile("dsb ish" ::: "memory");
        g_bkl_owner = -1;
        raw_unlock(&g_bkl_lock);           /* raw lock actually freed here     */
        g_bkl_depth = 0;
        smp_bkl_ei(di);                    /* restore caller's IRQ state       */
    } else {
        g_bkl_depth--;
    }
}

/* ②.2b-ii SMP-idle BKL handoff (called from .Lsmp_idle, cpu_support.S).
 *
 *  A task that BLOCKS reaches END_CRITICAL_SECTION, which calls knl_dispatch
 *  (switching this CPU away) BEFORE BKL_RELEASE — so the blocking leaves THIS
 *  CPU owning the BKL at the depth it entered the syscall with (1 for a plain
 *  task syscall).  If the CPU then idles still holding it, another CPU's
 *  bkl_acquire spins forever.  smp_idle_bkl_drop FULLY releases the BKL iff this
 *  CPU owns it (recording the prior depth so it can be restored); on wake,
 *  smp_idle_bkl_reacquire re-takes it to that SAME depth, so the task we resume
 *  finds the BKL held exactly as its own pending BKL_RELEASE expects.  IRQ is
 *  masked by the caller across drop (we are about to wfe with IRQ unmasked, but
 *  the drop itself must complete first); the per-CPU saved depth is safe because
 *  only THIS CPU executes its own .Lsmp_idle. */
static unsigned int g_idle_saved_depth[SMP_MAX_CPUS];

void smp_idle_bkl_drop(void)
{
    long me = (long)smp_mpidr_aff0();
    if (me < 0 || me >= SMP_MAX_CPUS)
        return;
    if (g_bkl_owner != me) {
        g_idle_saved_depth[me] = 0;     /* we don't hold it — nothing to drop */
        return;
    }
    g_idle_saved_depth[me] = g_bkl_depth;   /* remember the entry depth */
    g_bkl_depth = 1;                        /* collapse to 1 so one release frees */
    bkl_release();                          /* fully drop: owner=-1, raw_unlock   */
}

void smp_idle_bkl_reacquire(void)
{
    long me = (long)smp_mpidr_aff0();
    if (me < 0 || me >= SMP_MAX_CPUS)
        return;
    /* Consume any reschedule pending on us: the SGI that woke us from idle has
     * done its job (the publisher set our schedtsk); .Ldispatch_loop will pick
     * it up.  Clearing here avoids a stale pending flag firing a spurious async
     * switch once we are running the resumed task. */
    g_resched_pending[me] = 0u;
    unsigned int d = g_idle_saved_depth[me];
    if (d == 0)
        return;                             /* we never held it — nothing to do */
    bkl_acquire();                          /* re-own at depth 1 (raw_lock)       */
    g_bkl_depth = d;                        /* restore the entry depth            */
    g_idle_saved_depth[me] = 0;
}

/* ════════════════════════════════════════════════════════════════════
 *  ②.2b-i — the IRQ-return-path async-preempt DECISION (Option A).
 *
 *  Called from _vec_el1_irq (cpu_support.S, SMP_SELFTEST) AFTER the SGI/timer
 *  is EOIR'd and BEFORE restore_caller_regs.  Returns 1 iff THIS CPU should
 *  perform a real register-context switch from interrupt context, mirroring
 *  the 4-clause END_CRITICAL_SECTION dispatch test (cpu_status.h) PLUS the
 *  §5.4 BKL-held guard:
 *
 *    (1) a reschedule is pending on me           (g_resched_pending[me])
 *    (2) my current task != my next task         (ctxtsk != schedtsk)
 *    (3) dispatch is not disabled on me           (!dispatch_disabled)
 *    (4) the interrupted ctx is NOT task-independent (!knl_taskindp)
 *    (5) §5.4 DEADLOCK GUARD: the interrupted ctx does NOT hold the BKL —
 *        i.e. it was not mid-critical-section.  Switching away from a task
 *        that holds the BKL would (a) strand the lock (the new task can never
 *        acquire it) and (b) is exactly the "preempt while BKL-held mid-
 *        syscall" self-deadlock the plan flags as the #1 hazard.  g_bkl_owner
 *        is published UNDER the raw lock and read here with a dmb; if it
 *        equals me, the interrupted context is inside the kernel → DEFER.
 *
 *  The pending flag is CONSUMED here (set to 0) only when we decide to switch,
 *  so a deferred reschedule (clause 5 false) stays pending and is retried on
 *  the next IRQ once the BKL is released (the task's own END_CRITICAL_SECTION
 *  will also dispatch it cooperatively — belt and braces).
 *
 *  knl_taskindp is a single GLOBAL W (signed int) at this base (§3.4) — NOT
 *  per-CPU.  For the ②.2b cert the secondary's async-preempt loop is plain
 *  task context (knl_taskindp == 0), so reading the global is correct here;
 *  per-CPU-izing it is ledgered as a ②.3 sharpening. */
extern int knl_taskindp;                 /* W == signed int (cpu_init.c) */

int smp_irq_need_resched(void)
{
#if defined(SMP_NO_ASYNC)
    /* FALSIFIER (-DSMP_NO_ASYNC): revert to the ②.1a flag-set-only behaviour —
     * the IRQ-return path performs NO context switch.  The SGI is still TAKEN
     * (smp_resched_sgi_handler bumped g_sgi_taken + set pending), but with no
     * switch and no flag-check in the tight loop the low-prio task is NEVER
     * preempted → the cert FAILs (g_async_highprio_ran stays 0), proving the
     * async switch is load-bearing.  We DO consume the pending flag so the
     * sgi_taken>=1 evidence cleanly distinguishes "SGI delivered, no switch"
     * from "no SGI". */
    unsigned long me0 = smp_mpidr_aff0();
    if (me0 < SMP_MAX_CPUS) {
        g_resched_pending[me0] = 0u;
        __asm__ volatile("dmb ish" ::: "memory");
    }
    return 0;
#else
    unsigned long me = smp_mpidr_aff0();
    if (me >= SMP_MAX_CPUS)
        return 0;

    __asm__ volatile("dmb ld" ::: "memory");

    /* (1) pending? */
    if (!g_resched_pending[me])
        return 0;

#if !defined(SMP_NO_BKL_GUARD)
    /* (5) §5.4 deadlock guard: never switch away from a BKL holder.
     *
     * FALSIFIER (-DSMP_NO_BKL_GUARD): REMOVE this clause.  The [smp-no-deadlock]
     * cert (smp_deadlock.c) then sends an SGI while a task holds the BKL; with
     * the guard gone the async switch FIRES mid-critical-section → the switched-
     * to task's bkl_acquire() spins FOREVER on the still-held raw lock → DEADLOCK
     * → the driver watchdog catches it → "SMP-NO-DEADLOCK: FAIL".  WITH the guard
     * (default) → the switch is DEFERRED → no deadlock → PASS.  This is the
     * §5.4-mandated certified falsifier that makes the guard LOAD-BEARING. */
    if (g_bkl_owner == (long)me)
        return 0;                         /* mid-critical-section → defer */
#endif

    /* (4) not task-independent (timer-startup / IRQ nesting marker). */
    if (knl_taskindp > 0)
        return 0;

    /* (3) dispatch not disabled on this CPU. */
    if (g_smpcpu[me].dispatch_disabled)
        return 0;

    /* (2) a different task is selected to run. */
    void *ctx   = g_smpcpu[me].ctxtsk;
    void *sched = g_smpcpu[me].schedtsk;
    if (sched == 0 || ctx == sched)
        return 0;

    /* (2b) ②.2b-ii: if THIS CPU is IDLE (ctxtsk == NULL — it parked in
     * .Lsmp_idle after its task blocked), do NOT async-switch from interrupt
     * context.  The idle loop owns the BKL handoff (smp_idle_bkl_drop /
     * _reacquire); an async knl_dispatch here would bypass the reacquire and
     * the resumed task would BKL_RELEASE a lock this CPU does not own.  Instead
     * return 0: the SGI merely wakes the wfe, and .Lsmp_idle re-acquires the BKL
     * and re-checks .Ldispatch_loop, which picks up the published schedtsk.  The
     * pending flag is KEPT so a later RUNNING-context preempt still fires (it is
     * harmlessly cleared when the idle loop dispatches, having no effect). */
    if (ctx == 0)
        return 0;

    /* All clauses hold → consume the pending flag and switch. */
    g_resched_pending[me] = 0u;
    __asm__ volatile("dmb ish" ::: "memory");
    return 1;
#endif /* SMP_NO_ASYNC */
}

/* Public wrapper so the cert TUs (smp_async.c) can set the primary's SMPEN
 * without re-declaring the static helper. */
void smp_set_smpen_pub(void) { smp_set_smpen(); }

/* Per-CPU "# of SGIs taken" — observability shared by the ②.1a [smp-cross-
 * preempt], ②.2b-i [smp-async-preempt], and any future SMP cert (reads the
 * always-SMP g_sgi_taken[] the SGI handler bumps).  Available whenever
 * SMP_SELFTEST is on, not just SMP_PREEMPT_TEST, so the async cert can prove
 * "the SGI was delivered" independently. */
unsigned long smp_sgi_taken(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) return 0UL;
    __asm__ volatile("dmb ld" ::: "memory");
    return g_sgi_taken[cpu];
}

/* ════════════════════════════════════════════════════════════════════
 *  ②.2b-ii — Half B: the cross-CPU WAIT-wake (knl_smp_wake)
 *
 *  THE GAP (design §4.2 Gap #2): when CPU 0 readies a task that belongs on
 *  CPU 1 (e.g. tk_sig_sem on CPU 0 waking a CPU-1 semaphore-waiter),
 *  knl_make_ready (kernel/common/task.c) sets only the CALLING CPU's
 *  CUR_SCHEDTSK — nothing tells CPU 1 to re-dispatch the now-READY task, so it
 *  sits in the shared ready queue while CPU 1 is parked.
 *
 *  THE FIX: knl_make_ready's tail calls knl_smp_wake_hook(tcb) → here.  We
 *  publish g_smpcpu[target].schedtsk = tcb and ring the reschedule SGI on the
 *  target; the SGI's IRQ-return path hits the already-certified ②.2b-i
 *  async-switch hook (smp_irq_need_resched), which re-dispatches tcb on the
 *  target.  We run with the BKL ALREADY HELD (every wake site is inside
 *  BEGIN/END_CRITICAL_SECTION, §4.5) → the schedtsk publish + SGI are
 *  BKL-serialised against the target's own scheduler mutations.
 *
 *  CRITICAL: knl_smp_wake is reached from knl_make_ready on EVERY SMP_SELFTEST
 *  build (the macro expands to a real call whenever SMP_SELFTEST is on), and
 *  CPU 0's normal T-Kernel boot calls knl_make_ready constantly (timer ticks,
 *  semaphores).  It MUST be an inert no-op unless the [smp-secondary-wait] cert
 *  has explicitly ARMED it (g_xwake_armed) for a specific target task — so the
 *  other certs (smp0..3 / onemind / mc2) and the production boot that follows
 *  are UNPERTURBED.  Directed single-target wake only (the general affinity /
 *  migration policy is ②.3 deferred, §7.3).
 * ════════════════════════════════════════════════════════════════════ */
/* Armed by the secwait driver: while non-zero, a wake of g_xwake_tcb is
 * directed to CPU g_xwake_target.  Zero = inert (every other build/path). */
volatile unsigned long  g_xwake_armed  = 0;
volatile void          *g_xwake_tcb    = 0;
volatile int            g_xwake_target = 1;   /* the only secondary in the cert */

/* TCB is treated OPAQUELY here (smp.c is header-light; struct smp_cpu uses
 * void* for the task pointers).  The prototype must match the macro's
 * `extern void knl_smp_wake(TCB *tcb)` in cpu_status.h, so we forward-declare
 * the same tag/typedef T-Kernel uses (kernel/tkernel/typedef etc.). */
#ifndef __tcb__
#define __tcb__
typedef struct task_control_block TCB;
#endif

void knl_smp_wake(TCB *tcb)
{
    /* Inert unless the cert armed a directed wake for THIS exact task.  This is
     * what keeps every other SMP_SELFTEST build byte-for-byte behaviourally
     * unchanged (the call is present but does nothing). */
    if (!g_xwake_armed)
        return;
    if ((void *)tcb != (void *)g_xwake_tcb)
        return;

    int target = g_xwake_target;
    if (target <= 0 || target >= SMP_MAX_CPUS)
        return;

    /* Why an EXPLICIT publish?  The cert's secondary task is LOWER priority than
     * every other ready task (cpu0_tsk), so knl_make_ready's own
     * `CUR_SCHEDTSK = tcb` branch is NEVER taken (tcb does not outrank the
     * shared-queue top).  Pointing the target CPU at the woken task therefore
     * requires this directed publish.  Caller holds the BKL → serialised vs the
     * target's scheduler reads. */
    unsigned long me = smp_mpidr_aff0();
    if ((unsigned long)target == me) {
        /* SELF-WAKE (half i): the woken task belongs to the CPU we are ALREADY
         * running on (the secondary's OWN timer tick readied its OWN dly task).
         * Publish schedtsk; NO IPI needed — the secondary's next dispatch
         * checkpoint (.Lsmp_idle → .Ldispatch_loop re-read after this IRQ
         * returns) picks it up.  -DSMP_NO_XWAKE does NOT affect this self path,
         * so half (i) still PASSes under that falsifier (its scope is the
         * cross-CPU path only). */
        g_smpcpu[target].schedtsk = tcb;
        __asm__ volatile("dsb ish" ::: "memory");
        return;
    }

    /* CROSS-CPU WAKE (half ii): target != me. */
#if defined(SMP_NO_XWAKE)
    /* FALSIFIER (-DSMP_NO_XWAKE): suppress the ENTIRE cross-CPU wake — NEITHER
     * publish the target's schedtsk NOR ring its doorbell.  CPU 0 still marks tcb
     * READY in the shared queue, but the target CPU is never pointed at it and
     * never told → the sem-waiter never wakes → half (ii) hangs → watchdog →
     * SMP-SECONDARY-WAIT: FAIL.  (Suppressing ONLY the SGI is insufficient: the
     * target's idle wfe also wakes on the incidental `sev` from the caller's
     * bkl_release and would then SEE a published schedtsk — so the publish MUST
     * be suppressed too for the falsifier to truly bite.)  Proves the directed
     * cross-CPU wake — publish + IPI — is LOAD-BEARING. */
    (void)tcb;
#else
    g_smpcpu[target].schedtsk = tcb;
    __asm__ volatile("dsb ish" ::: "memory");
    smp_send_reschedule(target);
#endif
}

#ifdef SMP_SECONDARY_WAIT
/* ②.2b-ii — Half A: program THIS (secondary) CPU's OWN EL1 physical timer.
 *
 *  Replicates the per-CPU subset of tkdev_init.c's gic_init()+timer_init() for
 *  the local core, header-light (this TU avoids tkdev_init.c's headers):
 *    (1) enable the secondary's CPU interface (GICC_PMR/CTLR) — reuse
 *        smp_gic_cpuif_init();
 *    (2) enable PPI 30 in the secondary's BANKED GICD_ISENABLER0 (per-CPU on
 *        GICv2 — writing it on CPU 1 enables CPU 1's PPI 30 ONLY, §5.3);
 *    (3) program CNTP_TVAL_EL0 = CNTFRQ_EL0/TIMER_HZ + CNTP_CTL_EL0 = 1 (the 4
 *        per-CPU-banked-register instructions of timer_init).
 *  tkdev_init.c / timer.c are NOT edited; the global knl_intvec[30] handler
 *  (timer_irq_handler) runs unchanged on the secondary.  TIMER_HZ = 100 (=
 *  tkdev_conf.h); the cadence is computed from the LOCAL CNTFRQ_EL0 so it is
 *  correct on both QEMU virt (62.5 MHz) and RPi3 (19.2 MHz). */
#define SMP_TIMER_HZ   100UL   /* == tkdev_conf.h TIMER_HZ (10 ms period) */

void smp_secondary_timer_init(void)
{
#if !defined(BOARD_RPI3)
    /* (1) this CPU's GIC CPU interface (idempotent with the SGI path's call). */
    smp_gic_cpuif_init();

#if !defined(SMP_NO_SEC_TIMER)
    /* (2) enable PPI 30 in THIS CPU's banked GICD_ISENABLER0 (word 0, bit 30).
     *     On GICv2 the PPI/SGI bits of ISENABLER0 are per-CPU-banked, so this
     *     enables ONLY the calling core's timer PPI (mirrors gic_enable_irq). */
    *(volatile unsigned int *)(SMP_GICD_BASE + SMP_GICD_ISENABLER)
        = (1U << (SMP_TIMER_PPI & 31U));
    __asm__ volatile("dsb ish; isb" ::: "memory");

    /* (3) program this CPU's banked CNTP (enable + first interval). */
    unsigned long freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long interval = freq / SMP_TIMER_HZ;
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(interval));
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((unsigned long)1)); /* enable, unmasked */
    __asm__ volatile("dsb ish; isb" ::: "memory");
#else
    /* FALSIFIER (-DSMP_NO_SEC_TIMER): the secondary's CNTP is NOT programmed and
     * its PPI 30 is NOT enabled → CPU 1 never takes its own tick → the cert's
     * tk_dly_tsk task on CPU 1 hangs (the driver holds CPU 0's timer service
     * out of the window, §1.3, so NO other CPU can wake it) → watchdog →
     * SMP-SECONDARY-WAIT: FAIL.  Proves the secondary timer is load-bearing. */
    (void)0;
#endif /* !SMP_NO_SEC_TIMER */
#endif /* !BOARD_RPI3 */
}
#endif /* SMP_SECONDARY_WAIT */

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
extern unsigned char _stack_top_cpu2[];         /* linker.ld (②.1b)   */
extern unsigned char _stack_top_cpu3[];         /* linker.ld (②.1b)   */

/* ②.N8: cores 4..7 each need their OWN 16KB dispatcher stack. These are NOT
 * carved in linker.ld (cpu1/2/3 are) ON PURPOSE: the linker's cpu1/2/3
 * reservation sits BELOW _kernel_end, and _kernel_end's address is embedded
 * in the SHIPPED kernel's .text (cpu_init.c uses it as the heap base) — so
 * growing the linker reservation would move _kernel_end and break the
 * DEFAULT-build .text byte-identity. Instead we use static, 16-byte-aligned
 * BSS arrays HERE, which is fully SMP_SELFTEST-gated: in the default build
 * this whole file emits ZERO bytes (smp.o text/data/bss == 0), so _bss_end /
 * _kernel_end / .text are unchanged; only -DSMP_SELFTEST allocates them. The
 * stack TOP is &arr[sizeof(arr)] (16-aligned; AArch64 SP must be 16-aligned),
 * mirroring the linker's _stack_top_cpuN (the END of each region). The
 * synthetic task/counter state lives in OTHER static BSS, NOT on these. */
#define SMP_SEC_STACK_BYTES  0x4000               /* 16KB per secondary, == cpu1/2/3 */
static unsigned char smp_stack_cpu4[SMP_SEC_STACK_BYTES] __attribute__((aligned(16)));
static unsigned char smp_stack_cpu5[SMP_SEC_STACK_BYTES] __attribute__((aligned(16)));
static unsigned char smp_stack_cpu6[SMP_SEC_STACK_BYTES] __attribute__((aligned(16)));
static unsigned char smp_stack_cpu7[SMP_SEC_STACK_BYTES] __attribute__((aligned(16)));

/* DEPRECATED single shared secondary stack (②.0). KEPT only so any stale
 * reference still links; the N=4 path stores each secondary's stack in its
 * OWN g_smpcpu[cpu].stack_top (start.S loads SP from there). NOT used by the
 * current bringup. */
unsigned long g_smp_sec_stack_top = 0;

/* Per-secondary stack tops, indexed by CPU id (slot 0 = boot CPU, unused). */
static unsigned long smp_sec_stack_for(unsigned long cpu)
{
    switch (cpu) {
    case 1: return (unsigned long)_stack_top_cpu1;          /* linker.ld (below _kernel_end) */
    case 2: return (unsigned long)_stack_top_cpu2;
    case 3: return (unsigned long)_stack_top_cpu3;
    /* ②.N8: TOP of each static BSS stack (SP grows DOWN from here; the array
     * is 16-aligned and SMP_SEC_STACK_BYTES is a multiple of 16, so the top
     * is 16-aligned as AArch64 SP requires). */
    case 4: return (unsigned long)(smp_stack_cpu4 + SMP_SEC_STACK_BYTES);
    case 5: return (unsigned long)(smp_stack_cpu5 + SMP_SEC_STACK_BYTES);
    case 6: return (unsigned long)(smp_stack_cpu6 + SMP_SEC_STACK_BYTES);
    case 7: return (unsigned long)(smp_stack_cpu7 + SMP_SEC_STACK_BYTES);
    default: return 0;
    }
}

/* Release ONE secondary (cpu id = MPIDR Aff0) into the per-CPU dispatcher.
 * Each secondary gets its OWN stack (g_smpcpu[cpu].stack_top) — start.S loads
 * SP from the context_id block, so the seven secondaries never share a stack. */
long smp_bringup_cpu(unsigned long cpu)
{
    if (cpu == 0 || cpu >= SMP_MAX_CPUS)
        return PSCI_SUCCESS;                    /* nothing to do for boot CPU */
    g_smpcpu[cpu].cpu_id    = cpu;
    g_smpcpu[cpu].stack_top = smp_sec_stack_for(cpu);
    __asm__ volatile("dsb ish" ::: "memory");   /* publish before CPU_ON */
    /* QEMU virt cortex-a53: core N has MPIDR Aff0 == N. */
    return smp_psci_cpu_on(cpu,
                           (unsigned long)&_secondary_dispatch_entry,
                           (unsigned long)&g_smpcpu[cpu]);
}

/* Release ALL secondaries (cores 1 .. SMP_MAX_CPUS-1; ②.N8 = cores 1..7)
 * into the per-CPU dispatcher. Returns the first non-SUCCESS PSCI result
 * (or SUCCESS). The loop is parameterized on SMP_MAX_CPUS — no per-core
 * unrolling — so 4->8 needed no change here, only the stack map + count. */
long smp_bringup_secondary(void)
{
    /* AUTODETECT: decide how many cores this device actually has BEFORE the
     * bringup loop, so we wake EXACTLY that many (1..g_smp_ncpu-1 secondaries)
     * — not the hardcoded ceiling. Same binary adapts to -smp 2/4/8. */
    smp_set_ncpu();

    smp_set_smpen();                            /* primary's SMPEN */
    g_smpcpu[0].cpu_id = 0;

    long firstbad = PSCI_SUCCESS;
    for (unsigned long c = 1; c < g_smp_ncpu; c++) {
        long on = smp_bringup_cpu(c);
        if (on != PSCI_SUCCESS && on != PSCI_ALREADY_ON && firstbad == PSCI_SUCCESS)
            firstbad = on;
    }
    return firstbad;
}

/* Bounded wait until ALL secondaries (cores 1 .. SMP_MAX_CPUS-1) mark
 * themselves live in the dispatcher. Returns 0 = all up, -1 = timeout (a
 * wedged/missing CPU is detectable, NOT an infinite hang — the
 * [smp-boot-survives] watchdog). */
int smp_wait_secondary_live(void)
{
    const unsigned long MAX_TRIES = 200000000UL;
    unsigned long tries = 0;
    for (;;) {
        __asm__ volatile("dmb ld" ::: "memory");
        int allup = 1;
        /* Wait only for the cores we actually woke (1..g_smp_ncpu-1), NOT the
         * ceiling — else a -smp 2 run would hang waiting for absent cores. */
        for (unsigned long c = 1; c < g_smp_ncpu; c++)
            if (g_smpcpu[c].live != 1) { allup = 0; break; }
        if (allup)
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

/* (struct smp_task is defined above, near the ready list.) ②.1b/②.N8: ONE
 * task per CPU (N=SMP_MAX_CPUS, now 8) so every CPU pulls a DISTINCT task
 * from the ONE shared ready list and the mutex total is exactly N*K.
 * SMP_NTASKS==8 == SMP_MAX_READY, so the ready list holds all 8 tasks. */
#define SMP_NTASKS   SMP_MAX_CPUS
static struct smp_task g_tasks[SMP_NTASKS];
static unsigned long   g_task_budget = 0;   /* K, set by the driver */
/* ②.N8: with one task per CPU the shared ready list must hold all N. At
 * N=8 this is EXACTLY at the SMP_MAX_READY=8 capacity — guard it so a future
 * N>8 (would need GICv3 anyway) can't silently drop tasks in smp_ready_push. */
_Static_assert(SMP_NTASKS <= SMP_MAX_READY, "ready list too small for N tasks");

/* Concurrency barrier: each CPU bumps this when it is about to start its
 * increment loop; both spin until it reaches g_smp_ncpu (the RUNTIME active
 * count, NOT the ceiling) so the loops run TRULY CONCURRENTLY (so the NOLOCK
 * falsifier reliably loses updates). With autodetect, exactly g_smp_ncpu CPUs
 * each run one task and arrive here; waiting for SMP_MAX_CPUS would wedge a
 * sub-ceiling -smp run. */
static volatile unsigned long g_barrier = 0;

static void smp_barrier_wait(void)
{
    bkl_acquire();
    g_barrier++;
    bkl_release();
    /* Spin (NOT under the lock) until every WOKEN CPU has arrived. Bounded so a
     * missing CPU can't wedge forever. */
    unsigned long tries = 0;
    while (g_barrier < g_smp_ncpu) {
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

/* ════════════════════════════════════════════════════════════════════
 *  ②.1a — the CROSS-CPU PREEMPT cert workload (gated SMP_PREEMPT_TEST).
 *
 *  CPU B (the secondary) runs a LOW-prio spin task that, each iteration,
 *  checks g_resched_pending[B]. CPU A readies a HIGH-prio task targeted at
 *  B and smp_send_reschedule(B). B takes the SGI (handler sets the flag),
 *  observes it at its next checkpoint, RE-SELECTS under the BKL, finds the
 *  high-prio task A pushed, and runs it. The cert proves: the SGI is
 *  delivered → the handler runs on B → B re-selects to the high-prio task
 *  within a watchdog bound. The -DSMP_NO_IPI falsifier (no send) → B never
 *  preempts → SMP-PREEMPT: FAIL, proving the IPI is load-bearing.
 *
 *  This preemption is "cooperative-at-a-checkpoint": B's low-prio task
 *  checks the resched flag at loop boundaries. A TRUE asynchronous
 *  register-context preempt inside the SGI handler is the production
 *  context-switch work, DEFERRED to ②.2 (§2.4/§6). ②.1a proves the IPI
 *  MECHANISM, not the production scheduler conversion.
 * ════════════════════════════════════════════════════════════════════ */
#ifdef SMP_PREEMPT_TEST

/* Set by the driver before releasing the secondary → the secondary runs the
 * preempt loop instead of the ②.0 pull loop. */
static volatile int g_preempt_mode = 0;

/* The high-prio task A readies for B. A short task with a distinct id; its
 * "run" records that the high-prio task executed on B (sets highprio_ran on
 * B's per-CPU block). For the cert to PASS this must run on B AFTER the SGI. */
static struct smp_task g_highprio_task;   /* id 999 */

/* B's low-prio spin task pointer (so its ctxtsk is observable). */
static struct smp_task g_lowprio_task;    /* id 1 */

/* B sets this when it is provably spinning on the low-prio task (so A only
 * sends the SGI once B is actually in the interruptible loop). */
static volatile int g_b_spinning = 0;

/* The LOW-prio spin loop running on B with IRQs unmasked. Bounded (capped
 * iterations) so a missed preempt can NEVER wedge — the watchdog discipline.
 * Each iteration checks g_resched_pending[B]; on the flag, it records
 * preempted_at, re-selects under the BKL (smp_ready_pull now returns the
 * high-prio task A pushed), runs it, and returns. */
static void smp_secondary_preempt_loop(unsigned long me)
{
    /* Enable THIS CPU's GIC CPU interface so it can receive the SGI, then
     * unmask IRQ/FIQ for the cert window (§2.3, §2.5 step 5). */
    smp_gic_cpuif_init();
    smp_irq_unmask();

    g_smpcpu[me].ctxtsk = &g_lowprio_task;   /* B's current = low-prio */
    g_lowprio_task.claimed = 1;
    __asm__ volatile("dmb st" ::: "memory");
    g_b_spinning = 1;                        /* tell A we're interruptible */
    __asm__ volatile("dmb st; sev" ::: "memory");

    /* Bounded spin. The cap is large enough that, in a PASS run, the SGI
     * arrives well before it; in a NO_IPI run, it elapses and B never
     * preempts (preempted_at stays 0) → the driver watchdog FAILs. */
    const unsigned long CAP = 400000000UL;
    unsigned long k = 0;
    for (;;) {
        __asm__ volatile("dmb ld" ::: "memory");
        if (g_resched_pending[me]) {
            g_resched_pending[me] = 0;           /* consume */
            g_smpcpu[me].preempted_at = k ? k : 1; /* observed at iter k (>0) */
            __asm__ volatile("dmb ish" ::: "memory");

            /* RE-SELECT under the BKL exactly as ②.0 does — the only new
             * thing is the SGI-set trigger that made us re-enter the pull. */
            bkl_acquire();
            struct smp_task *t = smp_ready_pull();   /* → the high-prio task */
            if (t) {
                g_smpcpu[me].schedtsk = t;
                g_smpcpu[me].ctxtsk   = t;           /* B switched to it */
                g_smpcpu[me].exec_count++;
            }
            bkl_release();

            if (t) {
                /* Prove the asm per-CPU current-task load returns the
                 * high-prio task (the per-CPU switch happened in asm too). */
                if (smp_cur_tcb_load() == (void *)t && t->id == 999UL) {
                    g_smpcpu[me].highprio_ran = 1;   /* the high-prio ran on B */
                    t->done = 1;
                }
            }
            __asm__ volatile("dmb st; sev" ::: "memory");
            break;                                   /* preempt observed */
        }
        if (++k >= CAP)
            break;                                   /* watchdog cap (NO_IPI) */
        __asm__ volatile("yield" ::: "memory");
    }

    smp_irq_mask();                                  /* scope the IRQ-enable */
    g_smpcpu[me].live = 1;
    __asm__ volatile("dmb st; sev" ::: "memory");
    for (;;)
        __asm__ volatile("wfe");                     /* park; driver reaps */
}

#endif /* SMP_PREEMPT_TEST */

#ifdef SMP_2TASKS_PROD
/* ②.2a [smp-2tasks-prod]: the secondary, instead of the ②.0 stand-in pull
 * loop, waits for the driver (smp_prod.c, on CPU 0) to publish its schedtsk =
 * a REAL TCB, then enters the PRODUCTION dispatcher loop (.Ldispatch_loop via
 * smp_prod_enter_dispatch, cpu_support.S) — a genuine register-context switch
 * into the real task.  Symbols from smp_prod.c. */
extern volatile int g_prod_secondary_go;
extern void smp_prod_enter_dispatch(void);   /* cpu_support.S; never returns */
#endif

#ifdef SMP_ASYNC_PREEMPT
/* ②.2b-i [smp-async-preempt]: the secondary waits for the driver (smp_async.c,
 * on CPU 0) to publish its schedtsk = the REAL low-prio loop task L, then
 * enters the PRODUCTION dispatcher (smp_prod_enter_dispatch → .Ldispatch_loop)
 * — switching into L on CPU 1.  Symbols from smp_async.c. */
extern volatile int g_async_secondary_go;
extern void smp_prod_enter_dispatch(void);   /* cpu_support.S; never returns */
#endif

#ifdef SMP_DEADLOCK_TEST
/* ②.2b [smp-no-deadlock]: the secondary waits for the driver (smp_deadlock.c,
 * on CPU 0) to publish its schedtsk = the REAL BKL-holding loop task L, then
 * enters the PRODUCTION dispatcher (smp_prod_enter_dispatch → .Ldispatch_loop)
 * — switching into L on CPU 1.  Symbols from smp_deadlock.c. */
extern volatile int g_dl_secondary_go;
extern void smp_prod_enter_dispatch(void);   /* cpu_support.S; never returns */
#endif

#ifdef SMP_ONE_MIND
/* ②.2c [smp-one-mind]: CPU 1 waits for the driver (smp_onemind.c, on CPU 0) to
 * publish its schedtsk = the REAL mind task M (whose body computes the forward
 * hash), then enters the PRODUCTION dispatcher (smp_prod_enter_dispatch →
 * .Ldispatch_loop) — switching into M on CPU 1.  The OTHER secondaries run a
 * per-CPU busy/filler (or, under -DSMP_ONEMIND_RACE, the shared-rc scribble that
 * races M's in-flight forward).  Symbols from smp_onemind.c. */
extern volatile int g_onemind_secondary_go;          /* release CPU 1 into M    */
extern volatile unsigned long g_onemind_filler[SMP_MAX_CPUS]; /* per-CPU concurrency proof */
extern volatile int g_onemind_racer_go;              /* release the racer CPU   */
extern void smp_prod_enter_dispatch(void);   /* cpu_support.S; never returns    */
#ifdef SMP_ONEMIND_RACE
extern void r3_onemind_race_scribble(unsigned long iters);
extern volatile unsigned long g_om_forward_inflight; /* M's forward read window */
#endif
#endif

#ifdef SMP_SECONDARY_WAIT
/* ②.2b-ii [smp-secondary-wait]: CPU 1 waits for the driver (smp_secwait.c, on
 * CPU 0) to publish its schedtsk = the REAL waiter task, programs its OWN EL1
 * timer (smp_secondary_timer_init), then enters the PRODUCTION dispatcher
 * (smp_prod_enter_dispatch → .Ldispatch_loop) — switching into the waiter on
 * CPU 1.  The waiter blocks (tk_dly_tsk / tk_wai_sem) and is later WOKEN either
 * by CPU 1's OWN tick (half i) or by CPU 0's cross-CPU wake (half ii, via
 * knl_smp_wake → SGI → the ②.2b-i async hook).  Symbols from smp_secwait.c. */
extern volatile int g_secwait_secondary_go;
extern void smp_secondary_timer_init(void);  /* program this CPU's CNTP (above) */
extern void smp_prod_enter_dispatch(void);   /* cpu_support.S; never returns    */
#endif

/* The per-CPU dispatcher loop body (entered from smp_dispatch_loop). */
void smp_dispatch_run(void)
{
    unsigned long me = smp_mpidr_aff0();
    if (me >= SMP_MAX_CPUS) {
        for (;;) __asm__ volatile("wfe");     /* never index OOB */
    }

#ifdef SMP_ASYNC_PREEMPT
    /* ②.2b-i: run the REAL low-prio loop task L via the production dispatcher on
     * this secondary, then let the async IRQ-return hook preempt it mid-loop. */
    {
        g_smpcpu[me].live = 1;
        __asm__ volatile("dmb st; sev" ::: "memory");
        smp_dbg("[SMP] cpu1 entered PRODUCTION dispatcher (async-preempt)\r\n");
        unsigned long tries = 0;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");
            if (g_async_secondary_go && g_smpcpu[me].schedtsk != 0)
                break;
            if (++tries >= 200000000UL) {
                for (;;) __asm__ volatile("wfe");   /* driver watchdog FAILs */
            }
            __asm__ volatile("yield" ::: "memory");
        }
        smp_prod_enter_dispatch();             /* → .Ldispatch_loop; runs L */
        for (;;) __asm__ volatile("wfe");
    }
#endif

#ifdef SMP_DEADLOCK_TEST
    /* ②.2b [smp-no-deadlock]: run the REAL BKL-holding loop task L via the
     * production dispatcher on this secondary, then let the §5.4 guard DEFER the
     * async switch while L holds the BKL. */
    {
        g_smpcpu[me].live = 1;
        __asm__ volatile("dmb st; sev" ::: "memory");
        smp_dbg("[SMP] cpu1 entered PRODUCTION dispatcher (no-deadlock)\r\n");
        unsigned long tries = 0;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");
            if (g_dl_secondary_go && g_smpcpu[me].schedtsk != 0)
                break;
            if (++tries >= 200000000UL) {
                for (;;) __asm__ volatile("wfe");   /* driver watchdog FAILs */
            }
            __asm__ volatile("yield" ::: "memory");
        }
        smp_prod_enter_dispatch();             /* → .Ldispatch_loop; runs L */
        for (;;) __asm__ volatile("wfe");
    }
#endif

#ifdef SMP_2TASKS_PROD
    /* ②.2a: run a REAL TCB via the production dispatcher on this secondary. */
    {
        g_smpcpu[me].live = 1;
        __asm__ volatile("dmb st; sev" ::: "memory");
        smp_dbg("[SMP] cpu1 entered PRODUCTION dispatcher (2tasks-prod)\r\n");
        /* Wait until the driver has set g_smpcpu[me].schedtsk to the real B. */
        unsigned long tries = 0;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");
            if (g_prod_secondary_go && g_smpcpu[me].schedtsk != 0)
                break;
            if (++tries >= 200000000UL) {
                for (;;) __asm__ volatile("wfe");   /* driver watchdog FAILs */
            }
            __asm__ volatile("yield" ::: "memory");
        }
        smp_prod_enter_dispatch();             /* → .Ldispatch_loop; runs B */
        /* never returns (switches into B's stack) */
        for (;;) __asm__ volatile("wfe");
    }
#endif

#ifdef SMP_ONE_MIND
    /* ②.2c [smp-one-mind]: CPU 1 runs the REAL mind task M (its body computes
     * r3_onemind_forward_hash) via the production dispatcher; the OTHER
     * secondaries run a per-CPU busy/filler so the scheduler is provably driving
     * MULTIPLE cores while CPU 1 computes the mind (the §2.2 step-4 genuine
     * concurrency). Under -DSMP_ONEMIND_RACE the racer CPU instead scribbles the
     * SHARED rc/rw[] that M's in-flight forward reads → corruption → FAIL. */
    {
        g_smpcpu[me].live = 1;
        __asm__ volatile("dmb st; sev" ::: "memory");
        if (me == 1) {
            /* The MIND CPU: wait for the driver to publish M, then switch in. */
            smp_dbg("[SMP] cpu1 entered PRODUCTION dispatcher (one-mind)\r\n");
            unsigned long tries = 0;
            for (;;) {
                __asm__ volatile("dmb ld" ::: "memory");
                if (g_onemind_secondary_go && g_smpcpu[me].schedtsk != 0)
                    break;
                if (++tries >= 200000000UL) {
                    for (;;) __asm__ volatile("wfe");   /* driver watchdog FAILs */
                }
                __asm__ volatile("yield" ::: "memory");
            }
            smp_prod_enter_dispatch();             /* → .Ldispatch_loop; runs M */
            for (;;) __asm__ volatile("wfe");
        } else {
            /* The OTHER secondaries: genuine concurrency while M computes.  CPU 2
             * is the racer slot (only the falsifier arms g_onemind_racer_go). */
#ifdef SMP_ONEMIND_RACE
            if (me == 2) {
                unsigned long tries = 0;
                for (;;) {
                    __asm__ volatile("dmb ld" ::: "memory");
                    if (g_onemind_racer_go) break;
                    if (++tries >= 200000000UL) break;
                    __asm__ volatile("yield" ::: "memory");
                }
                /* DETERMINISTIC race (audit fix): a fixed one-shot burst could
                 * land ENTIRELY before M re-seeds rw[] (corruption overwritten) or
                 * ENTIRELY after the forward's reads (clean read) → spurious PASS
                 * ~13%/boot.  Instead: scribble the SHARED rc/rw[] in SMALL CHUNKS,
                 * GATED on M's "forward in flight" flag (g_om_forward_inflight,
                 * which r3_onemind_forward_hash sets=1 right before r_forward and
                 * =0 right after).  We start scribbling as soon as released and
                 * keep scribbling until we have OBSERVED the forward complete
                 * (flag seen 1 then back to 0) — so the corruption is live DURING
                 * the reads on EVERY boot.  A BOUNDED total-iteration cap is the
                 * fallback so the racer can NEVER hang the boot if the flag is
                 * missed; it then falls through to the busy filler / wfe. */
                const unsigned long RACE_CHUNK = 100000UL;   /* re-check flag often */
                const unsigned long RACE_CAP   = 200000000UL;/* hard fallback bound */
                unsigned long done = 0;
                int seen_inflight = 0;
                for (;;) {
                    __asm__ volatile("dmb ld" ::: "memory");
                    unsigned long inflight = g_om_forward_inflight;
                    if (inflight) seen_inflight = 1;
                    /* Exit once the forward we corrupted has finished (1→0)... */
                    if (seen_inflight && !inflight) break;
                    /* ...or if the bounded fallback cap is hit (never hang). */
                    if (done >= RACE_CAP) break;
                    r3_onemind_race_scribble(RACE_CHUNK);    /* small chunk, re-check */
                    done += RACE_CHUNK;
                }
            }
#endif
            /* All non-mind secondaries: a bounded busy counter (concurrency proof
             * the driver reads). Bounded so the run terminates. */
            for (unsigned long k = 0; k < 50000000UL; k++) {
                g_onemind_filler[me] = k;
                __asm__ volatile("" ::: "memory");
            }
            __asm__ volatile("dmb st" ::: "memory");
            for (;;) __asm__ volatile("wfe");
        }
    }
#endif

#ifdef SMP_SECONDARY_WAIT
    /* ②.2b-ii [smp-secondary-wait]: CPU 1 programs its OWN EL1 timer, waits for
     * the driver (smp_secwait.c, on CPU 0) to publish its schedtsk = the REAL
     * waiter task, then enters the PRODUCTION dispatcher — switching into the
     * waiter on CPU 1.  The waiter blocks (tk_dly_tsk / tk_wai_sem) and is later
     * woken by CPU 1's OWN tick (half i) or CPU 0's cross-CPU wake (half ii). */
    {
        g_smpcpu[me].live = 1;
        __asm__ volatile("dmb st; sev" ::: "memory");
        if (me == 1) {
            /* Program CPU 1's own banked CNTP + enable its PPI 30 BEFORE the
             * production dispatcher runs the waiter (so a tk_dly_tsk taken on
             * CPU 1 can be woken by CPU 1's own tick).  The PRODUCTION dispatcher
             * (.Ldispatch_loop, restoring the waiter's frame) unmasks IRQ as part
             * of restoring the task's SPSR. */
            smp_secondary_timer_init();
            smp_dbg("[SMP] cpu1 entered PRODUCTION dispatcher (secondary-wait)\r\n");
            unsigned long tries = 0;
            for (;;) {
                __asm__ volatile("dmb ld" ::: "memory");
                if (g_secwait_secondary_go && g_smpcpu[me].schedtsk != 0)
                    break;
                if (++tries >= 200000000UL) {
                    for (;;) __asm__ volatile("wfe");   /* driver watchdog FAILs */
                }
                __asm__ volatile("yield" ::: "memory");
            }
            smp_prod_enter_dispatch();             /* → .Ldispatch_loop; runs waiter */
            for (;;) __asm__ volatile("wfe");
        } else {
            /* Other secondaries idle (the cert uses only CPUs 0,1). */
            for (;;) __asm__ volatile("wfe");
        }
    }
#endif

#ifdef SMP_PREEMPT_TEST
    /* ②.1a: if the driver armed the preempt cert, the secondary runs the
     * interruptible low-prio loop instead of the ②.0 pull loop. */
    if (g_preempt_mode) {
        g_smpcpu[me].live = 1;
        __asm__ volatile("dmb st; sev" ::: "memory");
        smp_dbg("[SMP] cpu1 entered dispatcher (preempt cert)\r\n");
        smp_secondary_preempt_loop(me);       /* never returns */
    }
#endif

    g_smpcpu[me].live = 1;
    __asm__ volatile("dmb st; sev" ::: "memory");
    /* Emit the per-CPU entry marker UNDER the BKL so the blind UART writes
     * from the SEVEN secondaries (②.N8) do NOT interleave into a garbled line
     * (the harness greps for an intact "cpuN entered dispatcher"). The lock is
     * held only for the short marker; it does not serialize the workload. */
    bkl_acquire();
    smp_dbg_cpu_entered(me, " entered dispatcher\r\n");
    bkl_release();

    /* ②.1b/②.N8: each secondary pulls EXACTLY ONE task (the boot CPU also pulls
     * exactly one inline, smp_selftest_run). With ONE task per CPU
     * (SMP_NTASKS == SMP_MAX_CPUS == 8) every CPU gets a DISTINCT task, every CPU
     * arrives at the concurrency barrier (smp_barrier_wait waits for exactly
     * SMP_MAX_CPUS arrivals), and the mutex total is exactly N*K. A single
     * secondary draining MULTIPLE tasks would (a) starve another secondary of
     * a task → its exec_count stays 0 (SMP-RUN regresses) and (b) wedge the
     * barrier (it expects all SMP_MAX_CPUS to arrive). So: pull one, run one.
     *
     * PULL+CLAIM under the BKL (§3.3 shared queue); smp_ready_pull claims it
     * atomically (BKL held) so the OTHER CPUs get DISTINCT tasks. */
    bkl_acquire();
    struct smp_task *t = smp_ready_pull();
    if (t) {
        g_smpcpu[me].schedtsk = t;        /* per-CPU next task */
        g_smpcpu[me].ctxtsk   = t;        /* per-CPU current task */
        g_smpcpu[me].exec_count++;        /* this CPU advanced ITS task */
    }
    bkl_release();

    if (t) {
        /* Prove the asm per-CPU current-task load returns OUR task. */
        if (smp_cur_tcb_load() != (void *)t) {
            /* Per-CPU state mismatch — should never happen under the BKL.
             * Mark a sentinel so the driver can detect it. */
            g_smpcpu[me].exec_count |= (1UL << 60);
        }
        smp_run_task(t);                  /* RUN it on this CPU */
    }

    /* This CPU's task is done — idle (the driver reaps via counters). */
    for (;;)
        __asm__ volatile("wfe");
}

/* ── Driver entry (called by the boot self-test in main.c) ──────────────
 *  ②.N8 (was ②.1b N=4): seeds SMP_NTASKS (== SMP_MAX_CPUS == 8) tasks + the
 *  ONE shared ready list, releases ALL SEVEN secondaries (cores 1..7) into
 *  their own per-CPU dispatchers, runs the BOOT CPU's share inline (so it
 *  also runs a task), then JOINs on every task. Each of the 8 CPUs pulls
 *  EXACTLY ONE distinct task; the mutex total is exactly N*K under the BKL.
 *
 *  NOTE: a secondary does NOT return from smp_dispatch_run (it idles after
 *  its one task). So the driver runs the boot CPU's dispatcher inline: it
 *  seeds the work, releases the secondaries, runs ITS one task, then joins.
 *  We keep the boot CPU bounded to its single task so it returns to print the
 *  verdict. */
int smp_selftest_run(unsigned long K)
{
    /* AUTODETECT first: decide the runtime active count from GICD_TYPER so we
     * seed/push/join EXACTLY g_smp_ncpu tasks (one per woken CPU). With the
     * hardcoded ceiling we would seed 8 tasks but only g_smp_ncpu CPUs pull
     * them → the join would wait forever for the unpulled tasks. (Idempotent
     * with the smp_set_ncpu() inside smp_bringup_secondary() below.) */
    smp_set_ncpu();
    const unsigned int ncpu = g_smp_ncpu;

    g_task_budget = K;
    for (unsigned int i = 0; i < ncpu; i++) {
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
    for (unsigned int i = 0; i < ncpu; i++)   /* one task per WOKEN CPU */
        smp_ready_push(&g_tasks[i]);
    bkl_release();          /* depth 2 -> 1, raw lock still held */
    bkl_release();          /* depth 1 -> 0, raw unlock */
    __asm__ volatile("dsb ish" ::: "memory");

    /* Release the secondaries we actually have (cores 1..g_smp_ncpu-1) into
     * their per-CPU dispatchers (each pulls one task and runs it concurrently
     * with us). smp_bringup_secondary() re-runs detect (idempotent). */
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

    /* JOIN: bounded wait until ALL g_smp_ncpu tasks report done (every WOKEN
     * CPU finished its task). Only ncpu tasks were seeded/pushed, so we join
     * exactly those. Watchdog so a wedged CPU is a FAIL, not a hang. */
    const unsigned long MAX = 200000000UL;
    unsigned long tries = 0;
    for (;;) {
        __asm__ volatile("dmb ld" ::: "memory");
        int alldone = 1;
        for (unsigned int i = 0; i < ncpu; i++)
            if (!g_tasks[i].done) { alldone = 0; break; }
        if (alldone) break;
        if (++tries >= MAX) return -100;      /* join timeout (FAIL) */
        __asm__ volatile("wfe");
    }
    return 0;
}

#ifdef SMP_PREEMPT_TEST
/* ── ②.1a observability helpers (read per-CPU evidence for the cert) ──── */
unsigned long smp_preempted_at(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) return 0UL;
    __asm__ volatile("dmb ld" ::: "memory");
    return g_smpcpu[cpu].preempted_at;
}
unsigned long smp_highprio_ran(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) return 0UL;
    __asm__ volatile("dmb ld" ::: "memory");
    return g_smpcpu[cpu].highprio_ran;
}
/* The &g_highprio_task pointer (so the driver can assert B's ctxtsk == it). */
void *smp_highprio_taskptr(void) { return (void *)&g_highprio_task; }

/* ── ②.1a CROSS-CPU PREEMPT driver (boot CPU A) ───────────────────────
 *  Returns 0 = PASS (B provably preempted to the high-prio task after the
 *  SGI), <0 = FAIL (a watchdog stage timed out / no preempt). main.c reads
 *  the per-CPU evidence and prints SMP-PREEMPT: PASS/FAIL. */
int smp_preempt_test_run(void)
{
    /* Reset per-CPU evidence + tasks. */
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        g_smpcpu[i].preempted_at = 0;
        g_smpcpu[i].highprio_ran = 0;
        g_resched_pending[i]     = 0;
        g_sgi_taken[i]           = 0;
    }
    g_lowprio_task.id = 1;     g_lowprio_task.budget = 0;
    g_lowprio_task.claimed = 0; g_lowprio_task.done = 0;
    g_highprio_task.id = 999;  g_highprio_task.budget = 0;
    g_highprio_task.claimed = 0; g_highprio_task.done = 0;
    g_ready_n = 0; g_ready_hd = 0;
    g_b_spinning = 0;
    g_preempt_mode = 1;        /* arm the secondary's preempt loop */
    __asm__ volatile("dsb ish" ::: "memory");

    /* §2.5 steps 1-3: distributor enable + SGI handler in knl_intvec[0] +
     * boot-CPU interface + gicc_base_ptr — BEFORE the secondary can fire. */
    smp_gic_selftest_setup();

    /* ②.1b HONEST NOTE: the cross-CPU PREEMPT cert stays at N=2 (boot CPU A
     * + ONE secondary B = cpu1). The run/mutex certs scale to N=4, but the
     * preempt cert deliberately does NOT: waking cpus 2,3 would have them
     * spin the (single-target) preempt loop to their watchdog CAP waiting for
     * an SGI only sent to cpu1, inflating latency with no added proof — the
     * SGI MECHANISM is fully exercised by one target. A robust N=4 preempt
     * (per-target SGIs / staggered sends) is ②.2 production-scheduler work,
     * not this sandbox polish. So here we wake ONLY cpu1.
     *
     * Release JUST cpu1 (it enables ITS CPU interface, unmasks IRQ, and
     * begins spinning on the low-prio task, checking g_resched_pending[1]). */
    smp_set_smpen();                             /* primary's SMPEN */
    g_smpcpu[0].cpu_id = 0;
    long on = smp_bringup_cpu(1);
    if (on != PSCI_SUCCESS && on != PSCI_ALREADY_ON)
        return (int)on;                          /* CPU_ON failed */

    /* Wait until B is provably spinning (interruptible) before sending. */
    {
        const unsigned long MAX = 200000000UL;
        unsigned long tries = 0;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");
            if (g_b_spinning) break;
            if (++tries >= MAX) return -10;      /* B never started spinning */
            __asm__ volatile("wfe");
        }
    }
    smp_dbg("[SMP] cpu1 spinning on low-prio task (resched_pending=0)\r\n");

    /* A readies the HIGH-prio task for B (under the BKL), then sends the
     * reschedule SGI to CPU 1. (NO_IPI → the send is a no-op.) */
    bkl_acquire();
    smp_ready_push(&g_highprio_task);
    bkl_release();
    __asm__ volatile("dsb ish" ::: "memory");
    smp_send_reschedule(1);
    smp_dbg("[SMP] cpu0 readied high-prio task, sent reschedule SGI to cpu1\r\n");

    /* Bounded-wait for the preemption evidence: B observed the resched
     * (preempted_at != 0), the high-prio task ran (highprio_ran == 1), and
     * B's per-CPU current task is the high-prio one. Watchdog → FAIL. */
    {
        const unsigned long MAX = 300000000UL;
        unsigned long tries = 0;
        for (;;) {
            __asm__ volatile("dmb ld" ::: "memory");
            if (g_smpcpu[1].preempted_at != 0 &&
                g_smpcpu[1].highprio_ran == 1 &&
                g_smpcpu[1].ctxtsk == (void *)&g_highprio_task)
                return 0;                        /* PASS */
            if (++tries >= MAX)
                return -20;                      /* no preempt (NO_IPI / miss) */
            __asm__ volatile("wfe");
        }
    }
}
#endif /* SMP_PREEMPT_TEST */

#endif /* SMP_SELFTEST */
