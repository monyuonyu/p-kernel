/*
 *  arch/linux/aarch64/usermain.c
 *
 *  Initial task for the UMP build. Brings up the AI primitives, the
 *  pub/sub layer, and the Distributed Transformer in single-node
 *  mode, then drops into an interactive shell that the user drives
 *  over the host terminal (termios stdin/stdout).
 *
 *  Distributed-layer task creation (SWIM / Raft / DRPC echo etc.) is
 *  intentionally absent — those need a real NIC, and arch/linux only
 *  stubs that out. Phase B (TUN/TAP or Android NDK with a relay)
 *  re-enables them.
 */

#include "kernel.h"
#include <tmonitor.h>
#include "arch_reboot.h"

IMPORT void sio_send_frame(const UB *buf, INT size);
IMPORT INT  sio_read_line(UB *buf, INT maxlen);

IMPORT void ai_kernel_init(void);
IMPORT void ai_stats_print(void);
IMPORT void kdds_init(void);
IMPORT void kdds_list(void);
IMPORT void dtr_init(void);
IMPORT void dtr_stat(void);

static void print(const char *s)
{
    sio_send_frame((const UB *)s, (INT)__builtin_strlen(s));
}

/* Case-sensitive prefix match: true if `a` starts with `b` and `b`
 * is a complete token (followed by '\0' / space / end-of-line). */
static int starts_with(const UB *a, INT alen, const char *b)
{
    INT i = 0;
    for (; b[i] != '\0'; i++) {
        if (i >= alen) return 0;
        if (a[i] != (UB)b[i]) return 0;
    }
    return 1;
}

static void cmd_help(void)
{
    print("commands:\r\n");
    print("  help   - this list\r\n");
    print("  ai     - AI primitive statistics (inferences, jobs, FL rounds)\r\n");
    print("  dtr    - Distributed Transformer status\r\n");
    print("  kdds   - K-DDS topic table\r\n");
    print("  ver    - build identity\r\n");
    print("  exit   - terminate the UMP process\r\n");
    print("  (any other text is echoed back)\r\n");
}

static void cmd_ver(void)
{
    print("p-kernel UMP — User-Mode p-kernel\r\n");
    print("  host arch    : aarch64-linux\r\n");
    print("  T-Kernel core: micro T-Kernel 2.0 + p-kernel layers\r\n");
    print("  context-switch: raw asm (cooperative)\r\n");
    print("  IRQ source   : SIGALRM @ 100 Hz\r\n");
}

EXPORT INT usermain(void)
{
    print("\r\n p-kernel  [linux / aarch64 userspace]\r\n\r\n");

    /* AI Body layer — tensor pool, ai_job queue, pipeline, MLP. */
    ai_kernel_init();
    /* K-DDS — pub/sub. Single-node mode without a NIC. */
    kdds_init();
    /* DTR — distributed Transformer (the AI brain layer). */
    dtr_init();

    print("\r\n  T-Kernel is alive inside a Linux process.\r\n");
    print("  Type 'help' for commands.\r\n\r\n");

    UB line[128];
    for (;;) {
        print("p-kernel> ");
        INT n = sio_read_line(line, sizeof(line));
        if (n <= 0) {
            print("\r\n");
            continue;
        }

        if (starts_with(line, n, "help")) {
            cmd_help();
        } else if (starts_with(line, n, "ai")) {
            ai_stats_print();
        } else if (starts_with(line, n, "dtr")) {
            dtr_stat();
        } else if (starts_with(line, n, "kdds")) {
            kdds_list();
        } else if (starts_with(line, n, "ver")) {
            cmd_ver();
        } else if (starts_with(line, n, "exit")) {
            print("bye.\r\n");
            arch_reboot();   /* exit(0) on Linux */
        } else {
            print("[echo] ");
            sio_send_frame(line, n);
            print("\r\n");
        }
    }
    return 0;
}
