/*
 *  main.c (aarch64 / QEMU virt)
 *  Hardware init → T-Kernel startup
 *
 *  Called from arch/aarch64/start.S after stack setup and BSS clear.
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel.h"
#include "task.h"
#include "offset.h"

/* T-Kernel entry point declared in kernel.h-via-includes; no local proto */

/* PL011 UART init (arch/aarch64/sio.c) */
extern void sio_init(void);

#ifdef ARK_BAREMETAL_SMOKE
/* Bare-metal ARK smoke test (arch/aarch64/ark_bdev.c). Local prototype so this
 * TU need not pull extra headers; INT == int on LP64. Runs before T-Kernel,
 * driving virtio-blk in polled mode, then halts with a PASS/FAIL verdict. */
extern int ark_baremetal_smoke(void (*emit)(const char *));
#endif

#ifdef MC2_SMP_SELFTEST
/* MC-2.0 bare-metal constrained-SMP bringup (arch/aarch64/mc2_smp.c).
 * Local prototypes — keep this TU header-light. See the plan
 * docs/architecture/mc2-baremetal-smp-plan.md §4.2 [mc2-boot-survives]. */
extern long mc2_smp_release_one(void);   /* PSCI CPU_ON one secondary */
extern int  mc2_smp_join(void);          /* bounded join; -1 = timed out */
extern int  mc2_secondary_woken(void);   /* 1 = g_cpu[1].woken observed */
extern long mc2_tile_check(void);        /* -1 = tile matches pattern   */
#endif

#ifdef MC2_EQUIV_SELFTEST
/* MC-2.1 [mc2-smp-equiv] byte-identity cert (arch/aarch64/mc2_smp.c).
 * See docs/architecture/mc2-1-ncore-equiv-plan.md §4. The result struct
 * is mirrored here (header-light TU); KEEP IN SYNC with mc2_smp.c. */
extern long mc2_smp_release_n(int n);    /* PSCI CPU_ON cores 1..n-1 */
extern int  mc2_smp_idle_check(void);    /* 1 = secondaries idle in wfe */
struct mc2_equiv_result {
    int      ok;
    int      bad_nw;
    long     bad_idx;
    int      woke_fail;
    unsigned long long h_nw1;
    unsigned long long h_nw2;
    unsigned long long h_nw4;
};
extern void mc2_smp_equiv_selftest(struct mc2_equiv_result *r);
#endif

#if defined(MC2_SLICE_SELFTEST) || defined(MC2_EQUIV_SELFTEST)
/* MC-2.1b STANDALONE pk_slice_bm partition unit-check (arch/aarch64/
 * mc2_smp.c). A PURE INTEGER check of the hand-copied partition function —
 * needs NO secondary cores, runs on the primary before any matmul. The
 * result struct is mirrored here (header-light TU); KEEP IN SYNC with
 * mc2_smp.c's struct mc2_slice_result. */
struct mc2_slice_result {
    int           ok;
    unsigned long bad_out;
    int           bad_nw;
    long          bad_idx;
    int           reason;     /* 1=order 2=golden 3=coverage */
    int           n_cases;
};
extern void mc2_slice_unitcheck(struct mc2_slice_result *r);
#endif
#ifdef SMP_SELFTEST
/* ②.0 full-SMP slice: 2 CPUs run the T-Kernel dispatcher under one Big
 * Kernel Lock (arch/aarch64/smp.c). docs/architecture/20-architecture/full-smp-plan.md §7.
 * Header-light externs (this TU runs before T-Kernel is up). */
extern int           smp_selftest_run(unsigned long K); /* 0 = joined OK */
extern int           smp_wait_secondary_live(void);     /* 0 = up        */
extern unsigned long smp_exec_count(int cpu);           /* per-CPU counter */
extern void         *smp_running_tcb(int cpu);          /* per-CPU ctxtsk */
extern unsigned long smp_get_counter(void);             /* shared total   */
extern unsigned int  smp_ncpu(void);                    /* runtime active count */
#endif
#ifdef SMP_PREEMPT_TEST
/* ②.1a cross-CPU preemption via GIC SGI IPI (arch/aarch64/smp.c).
 * docs/architecture/smp-1-ipi-preempt-plan.md §1/§4. */
extern int           smp_preempt_test_run(void);   /* 0 = B preempted (PASS) */
extern unsigned long smp_preempted_at(int cpu);    /* iter B observed resched */
extern unsigned long smp_highprio_ran(int cpu);    /* 1 = high-prio ran on B  */
extern unsigned long smp_sgi_taken(int cpu);       /* # SGIs B took           */
extern void         *smp_highprio_taskptr(void);   /* &g_highprio_task        */
#endif

/* PL011 base differs between QEMU virt and BCM2837 — keep this print
 * helper minimal so it works before sio_init() runs. */
#ifdef BOARD_RPI3
#  define PRINT_UART_BASE   0x3F201000UL    /* BCM2837 PL011 (UART0) */
#else
#  define PRINT_UART_BASE   0x09000000UL    /* QEMU virt PL011       */
#endif
static void print(const char *s)
{
    volatile unsigned int *uart = (volatile unsigned int *)(PRINT_UART_BASE + 0x00); /* UARTDR */
    volatile unsigned int *fr   = (volatile unsigned int *)(PRINT_UART_BASE + 0x18); /* UARTFR */
    for (; *s; s++) {
        while (*fr & (1 << 5)) {}   /* wait TX FIFO not full */
        *uart = (unsigned int)(unsigned char)*s;
    }
}

#if defined(MC2_SMP_SELFTEST) || defined(MC2_EQUIV_SELFTEST) || defined(MC2_SLICE_SELFTEST) || defined(SMP_SELFTEST)
/* Minimal signed-long printer for FAIL diagnostics (no libc tm_printf here —
 * this runs before T-Kernel). Prints decimal. */
static void print_long(long v)
{
    char buf[24];
    int  i = 0;
    unsigned long u;
    int neg = 0;
    if (v < 0) { neg = 1; u = (unsigned long)(-v); } else { u = (unsigned long)v; }
    if (u == 0) buf[i++] = '0';
    while (u) { buf[i++] = (char)('0' + (u % 10)); u /= 10; }
    char out[26];
    int  o = 0;
    if (neg) out[o++] = '-';
    while (i > 0) out[o++] = buf[--i];
    out[o] = '\0';
    print(out);
}
#endif

#ifdef MC2_EQUIV_SELFTEST
/* Unsigned-hex printer for the FNV hashes (the cert prints all three and
 * the harness asserts they are identical). 16 hex digits. */
static void print_hex64(unsigned long long v)
{
    char out[19];
    int o = 0;
    out[o++] = '0'; out[o++] = 'x';
    for (int sh = 60; sh >= 0; sh -= 4) {
        int nib = (int)((v >> sh) & 0xFULL);
        out[o++] = (char)(nib < 10 ? ('0' + nib) : ('a' + nib - 10));
    }
    out[o] = '\0';
    print(out);
}
#endif

#if defined(MC2_SLICE_SELFTEST) || defined(MC2_EQUIV_SELFTEST)
/* Print the MC2-SLICE verdict a harness greps. PASS prints the case count;
 * FAIL localizes (out,nw,@idx) and names the invariant that tripped. The
 * falsifier build (-DMC2_SLICE_BREAK) must drive this to MC2-SLICE: FAIL. */
static void mc2_slice_print_verdict(void)
{
    struct mc2_slice_result sr;
    mc2_slice_unitcheck(&sr);
    if (sr.ok) {
        print("[MC2] pk_slice_bm partition unit-check: ");
        print_long((long)sr.n_cases);
        print(" (out,nw) cases — DISJOINT+TOTAL, ORDER, MATCHES-GOLDEN\r\n");
        print("MC2-SLICE: PASS\r\n");
    } else {
        const char *why = sr.reason == 1 ? "order"
                        : sr.reason == 2 ? "golden-mismatch"
                        : sr.reason == 3 ? "coverage" : "?";
        print("MC2-SLICE: FAIL out=");
        print_long((long)sr.bad_out);
        print(" nw=");
        print_long((long)sr.bad_nw);
        print(" @");
        print_long(sr.bad_idx);
        print(" reason=");
        print(why);
        print("\r\n");
    }
}
#endif

int main(void)
{
    /* UART init — must be first for debug output */
    sio_init();

    print("=== p-kernel aarch64 boot ===\r\n");
    print("[INIT] UART\r\n");

    /* Compile-time guard: keep TCB_SSP/TCB_tskctxb in offset.h in sync
     * with the actual TCB layout. If a TCB field's type ever changes
     * width (e.g. another typedef shift), the kernel boots into a
     * silent sync abort via knl_dispatch — much friendlier to fail at
     * link time. */
    _Static_assert(offsetof(TCB, tskctxb) == TCB_tskctxb,
                   "offset.h TCB_tskctxb out of sync with task.h");
    _Static_assert(sizeof(TCB) <= 256,
                   "TCB grew past tk_cre_tsk's assumed ceiling");

#ifdef ARK_BAREMETAL_SMOKE
    /* ARK on REAL hardware: format/write/sync/remount/read round-trip on the
     * physical virtio-blk disk, then halt. No T-Kernel needed — virtio-blk is
     * polled. Mirrors boot/x86/main.c's smoke hook. */
    {
        print("[ARK] running bare-metal smoke (virtio-blk)\r\n");
        int rc = ark_baremetal_smoke(print);
        if (rc == 0)
            print("[ARK] SMOKE RESULT: PASS\r\n");
        else
            print("[ARK] SMOKE RESULT: FAIL\r\n");
        print("=== ark-smoke done — halting ===\r\n");
        for (;;) __asm__ volatile ("wfe");
    }
#endif

#ifdef MC2_SLICE_SELFTEST
    /* MC-2.1b STANDALONE pk_slice_bm partition unit-check. A PURE INTEGER
     * check (no secondary cores, no matmul) of the hand-copied partition
     * function against its three invariants + the hosted golden. Runs on
     * the primary alone, then falls through to the normal T-Kernel boot
     * (so a plain-flag run still reaches the banner). The falsifier build
     * (-DMC2_SLICE_BREAK) must drive this to MC2-SLICE: FAIL. */
    {
        print("[MC2] pk_slice_bm STANDALONE partition unit-check...\r\n");
        mc2_slice_print_verdict();
        /* Fall through to the normal T-Kernel boot. */
    }
#endif

#ifdef MC2_SMP_SELFTEST
    /* MC-2.0 [mc2-boot-survives]: wake ONE parked secondary via PSCI
     * CPU_ON, run it through a trivial deterministic tile, prove the
     * primary survives and still goes on to boot the T-Kernel scheduler.
     *
     * Verdict breakdown (the harness greps "MC2-BOOT: PASS"):
     *   (a) we reached this point  -> banner about to print (kernel boots)
     *   (b) secondary woken        -> g_cpu[1].woken == 1
     *   (c) tile correct           -> mc2_tile_check() == -1
     *   (d) scheduler still ticks  -> the [BOOT]/init-task banner below
     *                                 prints after we return from here.
     * The faulting-tile variant (-DMC2_FAULTING_TILE) makes the join time
     * out; we report MC2-BOOT: FAIL join-timeout but DO NOT hang — the
     * primary continues to boot, proving a bad worker can't wedge it. */
    {
        print("[MC2] releasing one secondary via PSCI CPU_ON...\r\n");
        long on = mc2_smp_release_one();
        if (on != 0) {
            print("MC2-BOOT: FAIL cpu_on rc=");
            print_long(on);
            print("\r\n");
        } else {
            int  jr   = mc2_smp_join();
            int  woke = mc2_secondary_woken();
            long bad  = mc2_tile_check();
            if (jr != 0) {
                print("MC2-BOOT: FAIL join-timeout (primary survives, continuing)\r\n");
            } else if (!woke) {
                print("MC2-BOOT: FAIL secondary-not-woken\r\n");
            } else if (bad != -1) {
                print("MC2-BOOT: FAIL tile-mismatch@");
                print_long(bad);
                print("\r\n");
            } else {
                print("[MC2] secondary woken=1, tile pattern OK\r\n");
                print("MC2-BOOT: PASS\r\n");
            }
        }
        /* Fall through to the normal T-Kernel boot — proves clause (d):
         * the primary scheduler still ticks AFTER the secondary is up. */
    }
#endif

#ifdef MC2_EQUIV_SELFTEST
    /* MC-2.1 [mc2-smp-equiv]: wake cores 1,2,3 and prove a synthetic
     * gate-exceeding matmul computed via the bare-metal pk_parallel_rows
     * is BYTE-IDENTICAL to the serial loop across nw in {1,2,4}. The
     * primary survives any failure (bounded joins) and STILL boots the
     * T-Kernel (so [mc2-boot-survives] holds). See the plan §4. */
    {
        /* MC-2.1b: run the STANDALONE pk_slice_bm partition unit-check FIRST
         * (pure integer, no cores) — the equiv matmul below exercises the
         * partition only indirectly, so this directly guards the hand-copy
         * drift surface before we depend on it. */
        print("[MC2] pk_slice_bm partition unit-check (pre-matmul)...\r\n");
        mc2_slice_print_verdict();

        print("[MC2] releasing cores 1,2,3 via PSCI CPU_ON (N-core)...\r\n");
        long on = mc2_smp_release_n(4);
        if (on != 0) {
            print("MC2-EQUIV: FAIL cpu_on rc=");
            print_long(on);
            print("\r\n");
        } else {
            struct mc2_equiv_result r;
            mc2_smp_equiv_selftest(&r);
            print("[MC2] FNV nw=1 "); print_hex64(r.h_nw1); print("\r\n");
            print("[MC2] FNV nw=2 "); print_hex64(r.h_nw2); print("\r\n");
            print("[MC2] FNV nw=4 "); print_hex64(r.h_nw4); print("\r\n");
            if (r.woke_fail) {
                print("MC2-EQUIV: FAIL secondary-not-woken\r\n");
            } else if (r.ok) {
                int idle = mc2_smp_idle_check();
                print("MC2-EQUIV: PASS\r\n");
                if (idle) print("MC2-IDLE: PASS\r\n");
                else      print("MC2-IDLE: FAIL (worker busy-spun)\r\n");
            } else {
                print("MC2-EQUIV: FAIL nw=");
                print_long(r.bad_nw);
                print(" @");
                print_long(r.bad_idx);
                print("\r\n");
            }
        }
        /* Fall through to the normal T-Kernel boot. */
    }
#endif

#if defined(SMP_SELFTEST) && !defined(SMP_PREEMPT_TEST) && !defined(SMP_2TASKS_PROD) && !defined(SMP_ASYNC_PREEMPT) && !defined(SMP_DEADLOCK_TEST) && !defined(SMP_ONE_MIND) && !defined(SMP_SECONDARY_WAIT)
    /* ②.0/②.1b/②.N8 full-SMP slice: bring up SEVEN secondaries (cores 1..7)
     * into the per-CPU T-Kernel dispatcher under one Big Kernel Lock; have ALL
     * EIGHT CPUs run a DISTINCT task that increments a SHARED counter K times
     * under the BKL. ②.N8 = the real phone target (8 physical cores = the
     * GICv2 ceiling). Prove (now at N=8):
     *   [smp-2-tasks-run]    every CPU advanced its OWN per-CPU task (distinct)
     *   [smp-mutual-exclusion] shared total == exact N*K (BKL holds, N=8)
     *   [smp-boot-survives]  all CPUs reached the dispatcher; T-Kernel
     *                        still boots afterwards (no deadlock/hang).
     * The cert count N is taken from smp.c's SMP_MAX_CPUS via SMP_CERT_N so
     * the driver and the kernel can never disagree. docs/architecture/
     * full-smp-plan.md §7 ②.0; ②.1b N=4; ②.N8 N=8 generalization. */
    {
        /* SMP_CERT_MAX is the ARRAY CEILING (== smp.c's SMP_MAX_CPUS). The
         * cert count N is now the RUNTIME-DETECTED active core count, read from
         * smp_ncpu() AFTER smp_selftest_run() ran the GICD_TYPER autodetect —
         * so the SAME binary asserts the right total under -smp 2/4/8 with no
         * recompile. The stack arrays stay ceiling-sized (no VLA in this
         * freestanding TU); the loops run 0..N-1 over the cores actually
         * woken. */
        #define SMP_CERT_MAX 8
        const unsigned long K = 200000UL;       /* per-task increments */
        print("[SMP] autodetecting core count (GICD_TYPER) + releasing secondaries (BKL)...\r\n");
        int rc = smp_selftest_run(K);

        /* The runtime active count detected by smp_selftest_run() (1..8). The
         * whole point of the cert: the SAME binary wakes EXACTLY this many. */
        unsigned long N = (unsigned long)smp_ncpu();
        if (N < 1) N = 1;
        if (N > SMP_CERT_MAX) N = SMP_CERT_MAX;
        print("[SMP] detected "); print_long((long)N);
        print(" cpus via GICD_TYPER (g_smp_ncpu)\r\n");

        /* [smp-boot-survives]: did ALL woken secondaries reach the dispatcher? */
        int sec_live = (smp_wait_secondary_live() == 0);
        unsigned long e[SMP_CERT_MAX];
        void *t[SMP_CERT_MAX];
        for (int c = 0; c < (int)N; c++) {
            e[c] = smp_exec_count(c);
            t[c] = smp_running_tcb(c);
        }
        unsigned long total = smp_get_counter();
        unsigned long expect = N * K;

        for (int c = 0; c < (int)N; c++) {
            print("[SMP] cpu"); print_long((long)c);
            print(" exec_count="); print_long((long)e[c]); print("\r\n");
        }
        print("[SMP] shared counter=");  print_long((long)total);
        print(" expected=");             print_long((long)expect); print("\r\n");

        /* [smp-2-tasks-run]: EVERY woken CPU advanced its OWN task, and all N
         * per-CPU current tasks are DISTINCT (different TCB ptrs). */
        int all_ran = 1, all_distinct = 1;
        for (int c = 0; c < (int)N; c++)
            if (!(e[c] > 0 && t[c] != 0)) all_ran = 0;
        for (int a = 0; a < (int)N && all_distinct; a++)
            for (int b = a + 1; b < (int)N; b++)
                if (t[a] == t[b]) { all_distinct = 0; break; }
        if (rc == 0 && all_ran && all_distinct) {
            print("SMP-RUN: PASS\r\n");
        } else {
            print("SMP-RUN: FAIL rc="); print_long((long)rc);
            print(" (need cpu0..N-1 exec>0 and N distinct tasks)\r\n");
        }

        /* [smp-mutual-exclusion]: no lost updates under the BKL. */
        if (rc == 0 && total == expect) {
            print("SMP-MUTEX: PASS\r\n");
        } else {
            print("SMP-MUTEX: FAIL total="); print_long((long)total);
            print(" expected=");             print_long((long)expect);
            print("\r\n");
        }

        /* [smp-boot-survives]: both CPUs in the dispatcher, no hang. */
        if (sec_live && rc != -100) {
            print("SMP-BOOT: PASS\r\n");
        } else {
            print("SMP-BOOT: FAIL (secondary not live or join timeout)\r\n");
        }
        /* Fall through to the normal T-Kernel boot — proves the primary
         * scheduler still runs AFTER the SMP slice. */
        #undef SMP_CERT_MAX
    }
#endif

#ifdef SMP_PREEMPT_TEST
    /* ②.1a cross-CPU preemption via GIC SGI IPI: CPU B runs a LOW-prio spin
     * task; CPU A readies a HIGH-prio task for B and smp_send_reschedule(B);
     * B takes the SGI and switches to the high-prio task within a watchdog
     * bound. Prove [smp-cross-preempt]: SMP-PREEMPT: PASS with the evidence
     * that B preempted. FALSIFIER -DSMP_NO_IPI: no send → B never preempts →
     * SMP-PREEMPT: FAIL. docs/architecture/smp-1-ipi-preempt-plan.md §1/§4. */
    {
        print("[SMP] ②.1a cross-CPU preempt cert (GIC SGI IPI)...\r\n");
        int rc = smp_preempt_test_run();

        unsigned long pa     = smp_preempted_at(1);
        unsigned long hr     = smp_highprio_ran(1);
        unsigned long sgis   = smp_sgi_taken(1);
        void *b_ctx          = smp_running_tcb(1);
        void *hp             = smp_highprio_taskptr();

        print("[SMP] cpu1 preempted_at="); print_long((long)pa);
        print(" ran_high-prio=");          print_long((long)hr);
        print(" sgi_taken=");              print_long((long)sgis); print("\r\n");
        print("[SMP] cpu1 ctxtsk=");       print_long((long)(unsigned long)b_ctx);
        print(" highprio_taskptr=");       print_long((long)(unsigned long)hp);
        print("\r\n");

        /* [smp-cross-preempt] PASS: ALL of — rc==0, B observed the resched
         * (preempted_at != 0), the high-prio task ran on B (highprio_ran),
         * an SGI was actually taken, and B's per-CPU current task is the
         * high-prio one — within the driver watchdog. */
        if (rc == 0 && pa != 0 && hr == 1 && sgis >= 1 && b_ctx == hp) {
            print("SMP-PREEMPT: PASS\r\n");
        } else {
            print("SMP-PREEMPT: FAIL rc="); print_long((long)rc);
            print(" (B did not switch to the high-prio task after the SGI)\r\n");
        }
        /* Fall through to the normal T-Kernel boot — the missed/served
         * preempt must NOT crash the primary (NO_IPI must still boot). */
    }
#endif

    print("[BOOT] Starting T-Kernel...\r\n");
    {
        /* μT-Kernel 3.0: システムメモリ領域の設定 → sysinit（knl_main）。
         * 初期タスクはコア側 inittask.c が生成する。 */
        extern void knl_startup_hw(void);
        extern int  knl_main(void);
#ifdef SMP_SELFTEST
        /* WB SMP-fix (fable5 Wave-B): ARM the production-boot scheduler bridge.
         * Until now g_smp_prod==0 so the SMP self-test ran with the asm
         * dispatcher untouched (per-CPU slots only).  From here CPU 0 enters the
         * real μT-Kernel 3.0, whose core writes the GLOBAL knl_schedtsk/knl_ctxtsk;
         * setting g_smp_prod=1 makes cpu_support.S's .Ldispatch_loop sync the
         * per-CPU slots to/from those globals so the stale self-test residue in
         * g_smpcpu[0] can no longer wild-jump the first dispatch (the 0x3000000
         * EL1 instruction abort).  See arch/aarch64/smp.c for the regression note. */
        {
            extern volatile int g_smp_prod;
            g_smp_prod = 1;
        }
#endif
        knl_startup_hw();
        knl_main();
    }

    /* Never reached */
    print("[ERROR] T-Kernel returned!\r\n");
    for (;;) {
        __asm__ volatile ("wfe");
    }
}
