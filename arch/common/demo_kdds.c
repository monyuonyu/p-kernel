/*
 *  demo_kdds.c
 *
 *  Cross-arch K-DDS pub/sub demonstration. See demo_kdds.h.
 *
 *  Two tasks per node, both on topic "demo/heartbeat":
 *    - demo_pub_task: every DEMO_PERIOD_MS, publishes "n{id} {arch} t{N}"
 *    - demo_sub_task: blocks on kdds_sub, prints every received message
 *      with a "[kdemo-rx] " prefix
 *
 *  Because kdds_pub signals all OTHER handles on the same topic (not the
 *  publisher's own), opening separate handles for pub and sub gives the
 *  subscriber a local echo as well as remote messages. That makes the
 *  demo readable even when running standalone (no peers up yet), and
 *  it lets the shell observer see both "I sent" and "I received from
 *  the cross-arch peer" lines without extra wiring.
 */

#include "kernel.h"
#include "kdds.h"
#include "drpc.h"
#include "demo_kdds.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

#define DEMO_TOPIC      "demo/heartbeat"
#define DEMO_PERIOD_MS  2000
#define DEMO_MSG_MAX    64
#define DEMO_PUB_PRI    8
#define DEMO_SUB_PRI    7
#define DEMO_STACK      4096

static const char *demo_arch = "unknown";
static W   demo_pub_h = -1;
static W   demo_sub_h = -1;
static UB  demo_running = 0;

static void d_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

/* Tiny formatter for a uint into a fixed buffer. Returns chars written. */
static INT u_to_dec(UW v, char *out)
{
    char tmp[12];
    INT t = 0;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v > 0 && t < (INT)sizeof(tmp)) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    for (INT i = 0; i < t; i++) out[i] = tmp[t - 1 - i];
    return t;
}

static void demo_pub_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    UW tick = 0;
    char msg[DEMO_MSG_MAX];

    for (;;) {
        tk_dly_tsk(DEMO_PERIOD_MS);

        INT pos = 0;
        msg[pos++] = 'n';
        pos += u_to_dec((UW)drpc_my_node, &msg[pos]);
        msg[pos++] = ' ';
        for (const char *p = demo_arch; *p && pos < DEMO_MSG_MAX - 8; p++) msg[pos++] = *p;
        msg[pos++] = ' ';
        msg[pos++] = 't';
        msg[pos++] = '=';
        pos += u_to_dec(tick, &msg[pos]);
        msg[pos] = '\0';

        kdds_pub(demo_pub_h, msg, (W)(pos + 1));    /* include the NUL terminator */
        tick++;
    }
}

static void demo_sub_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    char buf[DEMO_MSG_MAX];

    for (;;) {
        W n = kdds_sub(demo_sub_h, buf, (W)sizeof(buf), -1);     /* block forever */
        if (n <= 0) { tk_dly_tsk(50); continue; }

        /* Ensure null-terminated before printing */
        if (n >= (W)sizeof(buf)) n = (W)sizeof(buf) - 1;
        buf[n] = '\0';

        d_puts("[kdemo-rx] ");
        d_puts(buf);
        d_puts("\r\n");
    }
}

static ID create_demo_task(FP fn, INT pri)
{
    T_CTSK ct = {
        .exinf  = NULL,
        .tskatr = TA_HLNG | TA_RNG0,
        .task   = fn,
        .itskpri = pri,
        .stksz  = DEMO_STACK
    };
    ID id = tk_cre_tsk(&ct);
    if (id >= E_OK) tk_sta_tsk(id, 0);
    return id;
}

void cmd_kdemo(const char *arch_label)
{
    if (demo_running) { d_puts("[kdemo] already running\r\n"); return; }

    demo_arch = (arch_label && *arch_label) ? arch_label : "unknown";

    demo_pub_h = kdds_open(DEMO_TOPIC, KDDS_QOS_BEST_EFFORT);
    demo_sub_h = kdds_open(DEMO_TOPIC, KDDS_QOS_BEST_EFFORT);
    if (demo_pub_h < 0 || demo_sub_h < 0) {
        d_puts("[kdemo] kdds_open failed\r\n");
        return;
    }

    if (create_demo_task((FP)demo_sub_task, DEMO_SUB_PRI) < E_OK) {
        d_puts("[kdemo] sub task spawn failed\r\n"); return;
    }
    if (create_demo_task((FP)demo_pub_task, DEMO_PUB_PRI) < E_OK) {
        d_puts("[kdemo] pub task spawn failed\r\n"); return;
    }

    demo_running = 1;
    d_puts("[kdemo] running — topic \"" DEMO_TOPIC "\", every ");
    {
        char b[16]; INT n = u_to_dec(DEMO_PERIOD_MS, b);
        sio_send_frame((const UB *)b, n);
    }
    d_puts(" ms\r\n");
}
