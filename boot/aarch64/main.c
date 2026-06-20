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

#ifdef MC2_SMP_SELFTEST
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

    print("[BOOT] Starting T-Kernel...\r\n");
    knl_t_kernel_main((void *)&knl_c_init_task);

    /* Never reached */
    print("[ERROR] T-Kernel returned!\r\n");
    for (;;) {
        __asm__ volatile ("wfe");
    }
}
