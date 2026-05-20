/*
 *  usermain.c (aarch64)
 *  Phase 2: initial task brings up the AI kernel primitives + minimal shell.
 *
 *  AI: tensor / ai_job / pipeline pools (no NIC needed — single-node).
 *  Shell: echo + 'ai' command shows AI stats.
 */

#include "kernel.h"
#include <tmonitor.h>
#include "ai_kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);
IMPORT INT  sio_read_line(UB *buf, INT maxlen);
IMPORT void ai_stats_print(void);

static void print(const char *s)
{
    sio_send_frame((const UB *)s, (INT)__builtin_strlen(s));
}

static int strneq(const UB *a, const char *b, INT n)
{
    for (INT i = 0; i < n; i++) {
        if (a[i] != (UB)b[i]) return 0;
        if (b[i] == 0) return 1;
    }
    return 1;
}

EXPORT INT usermain(void)
{
    print("\r\n");
    print("====================================\r\n");
    print(" p-kernel  [aarch64 / QEMU virt]\r\n");
    print(" Phase 2: AI kernel up\r\n");
    print("====================================\r\n");

    /* AI kernel primitives — tensor / ai_job / pipeline */
    ai_kernel_init();

    print("\r\nCommands: ai (show stats)  echo: any text\r\n");
    print("p-kernel> ");

    for (;;) {
        UB line[128];
        INT n = sio_read_line(line, sizeof(line));
        if (n == 0) {
            print("\r\np-kernel> ");
            continue;
        }
        print("\r\n");
        if (n >= 2 && strneq(line, "ai", 2)) {
            ai_stats_print();
        } else {
            print("[echo] ");
            sio_send_frame(line, n);
            print("\r\n");
        }
        print("p-kernel> ");
    }

    return 0;
}
