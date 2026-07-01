/*
 *  arch/linux/x86_64/net_dispatch.c
 *
 *  Tiny shim that owns the four public arch_linux_net_* symbols the
 *  rtl8139 driver shim calls into, and delegates to either:
 *    - net_unix_*  (loopback UDP transport, default)
 *    - net_relay_* (Phase B v1/v2 wire over public relay, selected
 *                    when PKERNEL_RELAY (multi-relay HA list), the legacy
 *                    PKERNEL_RELAY_HOST, or PKERNEL_SEED (N-4 seed list;
 *                    a relay is just a seed) is set in the environment)
 *
 *  Decided at arch_linux_net_init() time and stable for the process
 *  lifetime. Same source compiled identically for x86_64; promote to
 *  arch/common/linux/ once the existing net_unix.c duplication is
 *  cleaned up.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>     /* SLICE 4: CLOCK_MONOTONIC + nanosleep for the selector */

extern int net_unix_init(void);
extern int net_unix_send(const void *frame, int len);
extern int net_unix_recv(void *buf, int maxlen);
extern int net_unix_node_id(void);

extern int net_relay_init(void);
extern int net_relay_send(const void *frame, int len);
extern int net_relay_recv(void *buf, int maxlen);
extern int net_relay_node_id(void);

extern int net_lan_init(void);
extern int net_lan_send(const void *frame, int len);
extern int net_lan_recv(void *buf, int maxlen);
extern int net_lan_node_id(void);

/* connect-anywhere SLICE 3: PLAIN-TCP relay fallback (net_relay_tcp.c),
 * selected by PKERNEL_RELAY_TCP=1 for networks that block UDP entirely.
 * Joins the SAME relay mesh — the relay serves TCP and UDP clients on one
 * shared node table, so a TCP node and a UDP node exchange packets through it. */
extern int net_relay_tcp_init(void);
extern int net_relay_tcp_send(const void *frame, int len);
extern int net_relay_tcp_recv(void *buf, int maxlen);
extern int net_relay_tcp_node_id(void);

static int (*g_send)(const void *, int) = NULL;
static int (*g_recv)(void *, int)       = NULL;
static int (*g_node_id)(void)           = NULL;
static int  g_node = 1;

/* ====================================================================== *
 *  connect-anywhere SLICE 4 — automatic relay-transport fallback (UDP<->TCP)
 *
 *  A happy-eyeballs selector that brings up relay-UDP and, if the relay does
 *  not ANSWER within a bounded head start, races relay-TCP to the SAME
 *  endpoint and adopts whichever the relay answers first (lower rung / UDP
 *  preferred on a tie). Periodic re-eval prefers UDP once it recovers, with a
 *  hysteresis so a short blip never flaps the transport. NO human sets
 *  PKERNEL_RELAY_TCP; the manual override is preserved (it short-circuits this
 *  selector in arch_linux_net_init above).
 *
 *  Mechanism per docs/architecture/20-architecture/connect-anywhere.md §4S.a. Hosted-only;
 *  the bare-metal crown is unaffected by construction.
 * ====================================================================== */

/* New backend hooks (defined in net_relay.c / net_relay_tcp.c). */
extern int  net_relay_contacted(void);
extern void net_relay_probe(void);
extern void net_relay_reregister(void);
extern void net_relay_close(void);
extern int  net_relay_tcp_contacted(void);
extern void net_relay_tcp_probe(void);
extern void net_relay_tcp_reregister(void);
extern void net_relay_tcp_close(void);
extern void net_relay_tcp_set_connect_tmo(int ms);

/* §4.1 windows (ms) + re-eval/hysteresis periods (s). Shared by every node so
 * the ladder resolves identically everywhere. */
#define UDP_HEADSTART_MS     300
#define RACE_CONNECT_TMO_MS  700
#define ADOPT_DEADLINE_MS    2500
#define RE_EVAL_PERIOD_S     30
#define UDP_RECOVER_K_S      20
#define XP_POLL_MS           20

/* Live transport: 0 none, 3 relay-udp, 4 relay-tcp (rung numbers, §4). */
static int  xport_now          = 0;
/* Re-eval / hysteresis state (selector clock, ms). */
static long xp_udp_stable_since = 0;   /* 0 = UDP not continuously contacted   */
static long xp_last_eval_ms     = 0;   /* last UDP warm-probe sweep start       */
static int  xp_udp_warm         = 0;   /* 1 = UDP re-opened for recovery probing*/

/* Drain scratch — file-static, NEVER a task-stack local (hosted-relay stack
 * lesson: see feedback_hosted_relay_stack_overflow). */
static unsigned char xp_scratch[2048];

/* ---- cert mock seam (compiled OUT in production -> .text unchanged) ------ */
#ifdef AUTOXPORT_CERT
static int  xp_use_mock        = 0;    /* 1 = divert backend calls + mock clock */
static long xp_now_ms          = 0;    /* mock monotonic clock (ms)             */
static long xp_udp_contact_at  = -1;   /* mock: ms at/after which UDP contacts   */
static long xp_tcp_contact_at  = -1;   /* mock: ms FROM tcp start until contact  */
static int  xp_tcp_init_count  = 0;    /* mock: # of TCP inits the selector ran  */
static int  xp_adopted         = 0;    /* outcome: 0 none / 3 udp / 4 tcp        */
static long xp_adopt_ms        = -1;   /* outcome: ms-to-adopt                   */
static long xp_tcp_start_ms    = -1;   /* mock: when TCP init was called         */
#endif

/* ---- selector clock + cadence (divert to the mock under -DAUTOXPORT_CERT) - */
static long xp_clock_ms(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) return xp_now_ms;
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}
static void xp_poll_sleep(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) { xp_now_ms += XP_POLL_MS; return; }
#endif
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)XP_POLL_MS * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- UDP backend wrappers (mock-divertible) ----------------------------- */
static int  xp_udp_init(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) return 1;
#endif
    return net_relay_init();
}
static void xp_udp_do_probe(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) return;
#endif
    net_relay_probe();
}
static void xp_udp_pump(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) return;
#endif
    (void)net_relay_recv(xp_scratch, (int)sizeof(xp_scratch));
}
static int  xp_udp_is_contacted(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) return (xp_udp_contact_at >= 0 && xp_now_ms >= xp_udp_contact_at);
#endif
    return net_relay_contacted();
}
static void xp_udp_do_close(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) return;
#endif
    net_relay_close();
}
static void xp_udp_do_reregister(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) return;
#endif
    net_relay_reregister();
}

/* ---- TCP backend wrappers (mock-divertible) ----------------------------- */
static int  xp_tcp_init(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) { xp_tcp_init_count++; xp_tcp_start_ms = xp_now_ms; return 1; }
#endif
    net_relay_tcp_set_connect_tmo(RACE_CONNECT_TMO_MS);   /* lower the cap for the race */
    return net_relay_tcp_init();
}
static void xp_tcp_do_probe(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) return;
#endif
    net_relay_tcp_probe();
}
static void xp_tcp_pump(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) return;
#endif
    (void)net_relay_tcp_recv(xp_scratch, (int)sizeof(xp_scratch));
}
static int  xp_tcp_is_contacted(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) return (xp_tcp_contact_at >= 0 && xp_tcp_start_ms >= 0 &&
                             (xp_now_ms - xp_tcp_start_ms) >= xp_tcp_contact_at);
#endif
    return net_relay_tcp_contacted();
}
static void xp_tcp_do_close(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) return;
#endif
    net_relay_tcp_close();
}
static void xp_tcp_do_reregister(void)
{
#ifdef AUTOXPORT_CERT
    if (xp_use_mock) return;
#endif
    net_relay_tcp_reregister();
}

/* ---- low-stack loggers (write(2), never glibc stdio on a task stack — the
 * re-eval path runs on the 4 KB heartbeat task; see net_relay.c rl_append). - */
static void xp_emit(const char *s) { (void)write(2, s, strlen(s)); }
static void xp_emit_dec(long v)
{
    char t[24]; int n = 0; unsigned long u;
    if (v < 0) { (void)write(2, "-", 1); u = (unsigned long)(-v); }
    else u = (unsigned long)v;
    if (u == 0) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + (int)(u % 10)); u /= 10; }
    char o[24]; int j = 0;
    while (n > 0) o[j++] = t[--n];
    (void)write(2, o, (size_t)j);
}

/* THE SELECTOR. Brings up relay-UDP, races relay-TCP after the head start if
 * UDP hasn't been answered, adopts the first transport the relay answers, tears
 * down the loser, and points g_send/g_recv/g_node_id at the winner. Returns the
 * adopted node id (>0), or <=0 to let the caller fall through. */
static int xport_auto_init(void)
{
    long t0          = xp_clock_ms();
    int  udp_node    = xp_udp_init();   /* relay-UDP is always brought up first */
    int  tcp_node    = 0;
    int  tcp_started = 0;
    int  adopted     = 0;               /* 0 none / 3 udp / 4 tcp               */
    long adopt_ms    = -1;
    long last_reprobe = t0;

    xp_udp_do_probe();                  /* FORCE the first UDP keepalive now    */

    for (;;) {
        long now = xp_clock_ms();
        long el  = now - t0;

#ifndef AUTOXPORT_NOFALLBACK
        /* lower-rung head start: relay-TCP is not raced before UDP_HEADSTART_MS,
         * and never at all if UDP already contacted inside the head start. */
        if (!tcp_started && el >= UDP_HEADSTART_MS && !xp_udp_is_contacted()) {
            tcp_node    = xp_tcp_init();     /* bounded connect (RACE_CONNECT_TMO_MS) */
            tcp_started = 1;
            xp_tcp_do_probe();
        }
#endif
        xp_udp_pump();                       /* drain echoes so contact can flip */
        if (tcp_started) xp_tcp_pump();

        /* first-contact-wins; UDP (lower rung) preferred on a tie -> checked 1st */
        if (xp_udp_is_contacted())              { adopted = 3; adopt_ms = el; break; }
        if (tcp_started && xp_tcp_is_contacted()){ adopted = 4; adopt_ms = el; break; }

        if (el >= ADOPT_DEADLINE_MS) break;

        if (now - last_reprobe >= 200) {     /* re-fire so one dropped probe
                                              * can't starve contact in-window */
            xp_udp_do_probe();
            if (tcp_started) xp_tcp_do_probe();
            last_reprobe = now;
        }
        xp_poll_sleep();
    }

    if (adopted == 3) {
        if (tcp_started) xp_tcp_do_close();  /* tear down the TCP loser         */
        xp_udp_do_reregister();              /* settle the relay entry on UDP   */
        g_send = net_relay_send; g_recv = net_relay_recv; g_node_id = net_relay_node_id;
        g_node = (udp_node > 0) ? udp_node : g_node;
        xport_now = 3;
        xp_emit("[net] auto: adopted relay-udp (contact ");
        xp_emit_dec(adopt_ms); xp_emit("ms)\n");
        xp_emit("[net] transport = relay (node "); xp_emit_dec(g_node); xp_emit(")\n");
    } else if (adopted == 4) {
        xp_udp_do_close();                   /* tear down the UDP loser         */
        xp_tcp_do_reregister();              /* settle the relay entry on TCP   */
        g_send = net_relay_tcp_send; g_recv = net_relay_tcp_recv; g_node_id = net_relay_tcp_node_id;
        g_node = (tcp_node > 0) ? tcp_node : g_node;
        xport_now = 4;
        xp_emit("[net] auto: relay-udp no contact (");
        xp_emit_dec(adopt_ms); xp_emit("ms) \xe2\x80\x94 adopted relay-tcp\n");
        xp_emit("[net] transport = relay-tcp (node "); xp_emit_dec(g_node); xp_emit(")\n");
    } else {
        if (tcp_started) xp_tcp_do_close();
        g_send = net_relay_send; g_recv = net_relay_recv; g_node_id = net_relay_node_id;
        g_node = (udp_node > 0) ? udp_node : g_node;
        xport_now = 3;                       /* provisional UDP pointer, NO mesh */
        xp_emit("[net] auto: no relay contact (udp+tcp) \xe2\x80\x94 provisional relay-udp, no mesh\n");
        xp_emit("[net] transport = relay (node "); xp_emit_dec(g_node); xp_emit(")\n");
    }

    xp_last_eval_ms     = xp_clock_ms();
    xp_udp_stable_since = 0;
    xp_udp_warm         = 0;

#ifdef AUTOXPORT_CERT
    xp_adopted  = adopted;
    xp_adopt_ms = adopt_ms;
#endif
    return (g_node > 0) ? g_node : udp_node;
}

/* PERIODIC RE-EVAL + HYSTERESIS. Called from the hosted heartbeat task. Only
 * acts while on relay-TCP: every RE_EVAL_PERIOD_S it re-opens relay-UDP "warm",
 * probes it, and switches back to UDP ONLY after UDP has reported contact
 * CONTINUOUSLY for UDP_RECOVER_K_S — a short blip never flaps the transport. */
void net_xport_reeval(void)
{
    if (xport_now != 4) return;
    long now = xp_clock_ms();

    if (!xp_udp_warm) {
        if (now - xp_last_eval_ms < (long)RE_EVAL_PERIOD_S * 1000L) return;
        (void)xp_udp_init();             /* re-open relay-UDP for a probe sweep  */
        xp_udp_warm         = 1;
        xp_udp_stable_since = 0;
        xp_last_eval_ms     = now;
        xp_udp_do_probe();
        return;
    }

    xp_udp_do_probe();
    xp_udp_pump();
    if (xp_udp_is_contacted()) {
        if (xp_udp_stable_since == 0) {
            xp_udp_stable_since = now;
        } else if (now - xp_udp_stable_since >= (long)UDP_RECOVER_K_S * 1000L) {
            xp_tcp_do_close();                       /* drop the TCP loser       */
            xp_udp_do_reregister();                  /* settle the entry on UDP  */
            g_send = net_relay_send; g_recv = net_relay_recv; g_node_id = net_relay_node_id;
            xport_now           = 3;
            xp_udp_warm         = 0;
            xp_udp_stable_since = 0;
            xp_last_eval_ms     = now;
            xp_emit("[net] auto: relay-udp recovered (stable ");
            xp_emit_dec(UDP_RECOVER_K_S);
            xp_emit("s) \xe2\x80\x94 switched from relay-tcp\n");
        }
    } else {
        xp_udp_stable_since = 0;          /* contact broke -> restart the clock   */
        /* bounded sweep life: if UDP stays unreachable, drop it back down so its
         * keepalives don't fight the TCP {node->peer} entry, and wait a period. */
        if (now - xp_last_eval_ms >= ((long)UDP_RECOVER_K_S + 5) * 1000L) {
            xp_udp_do_close();
            xp_udp_warm     = 0;
            xp_last_eval_ms = now;
        }
    }
}

/* ---- IN-PROC cert: `autoxport test` ------------------------------------- */
#ifdef AUTOXPORT_CERT
static int axp_fail;
static void axp_check(void (*pr)(const char *), int ok, const char *d)
{
    pr(ok ? "[autoxport]   PASS " : "[autoxport]   FAIL ");
    pr(d); pr("\r\n");
    if (!ok) axp_fail = 1;
}
static void axp_putdec(void (*pr)(const char *), long v)
{
    char b[24]; int i = 0, neg = 0; unsigned long u;
    if (v < 0) { neg = 1; u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (u == 0) b[i++] = '0';
    while (u) { b[i++] = (char)('0' + (int)(u % 10)); u /= 10; }
    char o[26]; int j = 0;
    if (neg) o[j++] = '-';
    while (i > 0) o[j++] = b[--i];
    o[j] = '\0';
    pr(o);
}

void net_xport_select_self_test(void (*pr)(const char *))
{
    axp_fail = 0;
    pr("[autoxport] connect-anywhere SLICE 4 cert (auto relay-transport fallback)\r\n");

    /* save the production statics we borrow */
    int  sv_xport  = xport_now;       long sv_stable = xp_udp_stable_since;
    long sv_eval   = xp_last_eval_ms; int  sv_warm   = xp_udp_warm;

    xp_use_mock = 1;

    /* ---- CASE A: UDP open within the head start -> adopt UDP, NO TCP init -- */
    xp_now_ms = 0; xp_udp_contact_at = 200; xp_tcp_contact_at = -1;
    xp_tcp_init_count = 0; xp_tcp_start_ms = -1; xp_adopted = 0; xp_adopt_ms = -1;
    xport_now = 0;
    xport_auto_init();
    axp_check(pr, xp_adopted == 3,        "A: UDP open -> adopted relay-udp (3)");
    axp_check(pr, xp_tcp_init_count == 0, "A: relay-TCP NEVER initialised (no connect emitted)");
    pr("[autoxport]   info A adopted="); axp_putdec(pr, xp_adopted);
    pr(" tcp_init=");                     axp_putdec(pr, xp_tcp_init_count);
    pr(" adopt_ms=");                     axp_putdec(pr, xp_adopt_ms); pr("\r\n");

    /* ---- CASE B: UDP blocked, TCP open -> auto-adopt TCP in the window ----- */
    xp_now_ms = 0; xp_udp_contact_at = -1; xp_tcp_contact_at = 400;
    xp_tcp_init_count = 0; xp_tcp_start_ms = -1; xp_adopted = 0; xp_adopt_ms = -1;
    xport_now = 0;
    xport_auto_init();
    axp_check(pr, xp_tcp_init_count == 1, "B: relay-TCP started after the head start (init==1)");
    axp_check(pr, xp_adopted == 4,        "B: UDP blocked -> adopted relay-tcp (4)");
    axp_check(pr, xp_adopt_ms >= 0 && xp_adopt_ms <= ADOPT_DEADLINE_MS,
              "B: adopt_ms <= 2500 (bounded window)");
    pr("[autoxport]   info B adopted="); axp_putdec(pr, xp_adopted);
    pr(" tcp_init=");                     axp_putdec(pr, xp_tcp_init_count);
    pr(" adopt_ms=");                     axp_putdec(pr, xp_adopt_ms); pr("\r\n");

    /* ---- HYSTERESIS: on TCP, a 5 s UDP blip does NOT switch; >=20 s does --- */
    {
        xport_now           = 4;     /* start adopted on relay-tcp        */
        xp_udp_warm         = 1;     /* re-eval already warming UDP        */
        xp_udp_stable_since = 0;
        xp_last_eval_ms     = 100000;

        /* (a) 5 s blip: UDP contactable for 5 s, then gone -> NO switch */
        xp_udp_contact_at = 100000;
        int switched_during_blip = 0;
        for (long t = 100000; t <= 105000; t += 1000) {
            xp_now_ms = t; net_xport_reeval();
            if (xport_now != 4) switched_during_blip = 1;
        }
        xp_udp_contact_at = -1;                       /* blip ends */
        xp_now_ms = 106000; net_xport_reeval();
        axp_check(pr, switched_during_blip == 0 && xport_now == 4,
                  "HYST: 5s UDP blip does NOT switch back (still relay-tcp)");

        /* (b) continuous recovery: UDP contactable from 110000 on; switch ONLY
         *     after >= UDP_RECOVER_K_S (20 s) of continuous contact. */
        xp_udp_contact_at   = 110000;
        xp_udp_stable_since = 0;
        int switched_before_20s = 0;
        for (long t = 110000; t < 130000; t += 1000) {   /* 0..19 s -> NO switch */
            xp_now_ms = t; net_xport_reeval();
            if (xport_now != 4) switched_before_20s = 1;
        }
        axp_check(pr, switched_before_20s == 0,
                  "HYST: NO switch before 20s of continuous UDP contact");
        xp_now_ms = 130000; net_xport_reeval();          /* exactly 20 s */
        axp_check(pr, xport_now == 3,
                  "HYST: switch back to relay-udp after >=20s continuous contact");
    }

    /* restore production statics */
    xport_now = sv_xport; xp_udp_stable_since = sv_stable;
    xp_last_eval_ms = sv_eval; xp_udp_warm = sv_warm;
    xp_use_mock = 0;

    if (axp_fail) pr("[autoxport] RESULT: FAIL\r\n");
    else          pr("[autoxport] RESULT: 8/8 PASS\r\n");
}
#endif /* AUTOXPORT_CERT */

int arch_linux_net_init(void)
{
    /* N-1 LAN-DIRECT: same-WiFi mesh with no central relay, selected by
     * PKERNEL_LAN=1. Checked before the relay so an explicit LAN opt-in
     * wins; relay + loopback paths below are unchanged. */
    const char *lan = getenv("PKERNEL_LAN");
    if (lan && *lan && lan[0] != '0') {
        int n = net_lan_init();
        if (n > 0) {
            g_send    = net_lan_send;
            g_recv    = net_lan_recv;
            g_node_id = net_lan_node_id;
            g_node    = n;
            dprintf(2, "[net] transport = lan-direct (node %d)\n", n);
            return n;
        }
        dprintf(2, "[net] lan-direct init failed; falling back\n");
    }

    const char *list = getenv("PKERNEL_RELAY");
    const char *host = getenv("PKERNEL_RELAY_HOST");
    const char *seed = getenv("PKERNEL_SEED");
    /* N-4: PKERNEL_SEED selects the relay backend exactly like PKERNEL_RELAY
     * (a relay is just a seed that answers REL_REGISTER). net_relay_init()
     * itself reads PKERNEL_SEED first and, in seed-mode, boots SOLO
     * (relay_count=0) rather than hard-failing when no seed answers — so a
     * seed-only node still returns a usable node id here. */
    /* connect-anywhere SLICE 3: when PKERNEL_RELAY_TCP=1 AND a relay endpoint
     * is configured, join over a TCP stream instead of UDP (same mesh, same
     * v2 wire + a 2-byte length prefix). Checked before the UDP relay so the
     * opt-in wins; when unset, behaviour is exactly the UDP path below. */
    const char *tcp = getenv("PKERNEL_RELAY_TCP");
    if (tcp && *tcp && tcp[0] != '0' &&
        ((list && *list) || (host && *host) || (seed && *seed))) {
        int n = net_relay_tcp_init();
        if (n > 0) {
            g_send    = net_relay_tcp_send;
            g_recv    = net_relay_tcp_recv;
            g_node_id = net_relay_tcp_node_id;
            g_node    = n;
            dprintf(2, "[net] transport = relay-tcp (node %d)\n", n);
            return n;
        }
        dprintf(2, "[net] relay-tcp init failed; falling back\n");
    }

    /* connect-anywhere SLICE 4: AUTOMATIC relay-transport fallback. When a
     * relay endpoint is configured AND no manual transport override is set
     * (PKERNEL_RELAY_TCP unset, handled above) AND auto is not disabled
     * (PKERNEL_RELAY_AUTOFALLBACK != "0"), race relay-UDP vs relay-TCP to the
     * SAME endpoint and adopt whichever the relay answers first. Sits BETWEEN
     * the force-TCP branch above and the plain-UDP branch below so both manual
     * escape hatches are byte-for-byte unchanged. */
    {
        const char *af = getenv("PKERNEL_RELAY_AUTOFALLBACK");
        int auto_on    = !(af && af[0] == '0');
        if (auto_on && !(tcp && *tcp && tcp[0] != '0') &&
            ((list && *list) || (host && *host) || (seed && *seed))) {
            int n = xport_auto_init();
            if (n > 0) return n;
            dprintf(2, "[net] auto relay-transport select made no node; "
                       "falling back\n");
        }
    }

    if ((list && *list) || (host && *host) || (seed && *seed)) {
        int n = net_relay_init();
        if (n > 0) {
            g_send    = net_relay_send;
            g_recv    = net_relay_recv;
            g_node_id = net_relay_node_id;
            g_node    = n;
            dprintf(2, "[net] transport = relay (node %d)\n", n);
            return n;
        }
        dprintf(2, "[net] relay init failed; falling back to loopback\n");
    }
    int n = net_unix_init();
    g_send    = net_unix_send;
    g_recv    = net_unix_recv;
    g_node_id = net_unix_node_id;
    g_node    = (n > 0) ? n : g_node;
    dprintf(2, "[net] transport = loopback (node %d)\n", g_node);
    return n;
}

int arch_linux_net_send(const void *frame, int len)
{
    return g_send ? g_send(frame, len) : -1;
}

int arch_linux_net_recv(void *buf, int maxlen)
{
    return g_recv ? g_recv(buf, maxlen) : 0;
}

int arch_linux_net_node_id(void)
{
    return g_node_id ? g_node_id() : g_node;
}
