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
extern const void *knl_c_init_task;

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

#if defined(MC2_SMP_SELFTEST) || defined(MC2_EQUIV_SELFTEST) || defined(MC2_SLICE_SELFTEST)
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

void main(void)
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

    print("[BOOT] Starting T-Kernel...\r\n");
    knl_t_kernel_main((void *)&knl_c_init_task);

    /* Never reached */
    print("[ERROR] T-Kernel returned!\r\n");
    for (;;) {
        __asm__ volatile ("wfe");
    }
}
