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
#include "rtl8139.h"

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

#define NET_PRIORITY    9
#define NET_STACK       4096

static ID create_sem(INT isemcnt, INT maxsem)
{
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                  .isemcnt = isemcnt, .maxsem = maxsem };
    return tk_cre_sem(&cs);
}

static ID create_task(FP fn, INT pri, INT stksz)
{
    T_CTSK ct = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = fn, .itskpri = pri, .stksz = stksz };
    ID id = tk_cre_tsk(&ct);
    if (id >= E_OK) tk_sta_tsk(id, 0);
    return id;
}

static void net_bringup(void)
{
    static INT net_up = 0;
    if (net_up) {
        print("[net] already up\r\n");
        return;
    }

    ID net_sem = create_sem(0, 64);
    if (net_sem < E_OK) {
        print("[net] sem create failed\r\n");
        return;
    }

    ER er = rtl8139_init(net_sem);
    if (er != E_OK) {
        print("[net] init failed (add -device rtl8139?)\r\n");
        return;
    }

    if (create_task((FP)net_task, NET_PRIORITY, NET_STACK) < E_OK) {
        print("[net] task create failed\r\n");
        return;
    }

    net_up = 1;
    print("[net] RX task started\r\n");
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

    print("\r\nCommands: ai (show stats) | net (init RTL8139) | echo: any text\r\n");
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
        } else if (n >= 3 && strneq(line, "net", 3)) {
            net_bringup();
        } else {
            print("[echo] ");
            sio_send_frame(line, n);
            print("\r\n");
        }
        print("p-kernel> ");
    }

    return 0;
}
