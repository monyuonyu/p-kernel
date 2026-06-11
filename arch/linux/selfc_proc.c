/*
 *  arch/linux/selfc_proc.c — the germ-process crash boundary for self-built
 *  units (docs/architecture/selfc-ring3.md §2.1 CDN-S1(a), §1.2, §1.4).
 *
 *  The immune system: a self-compiled unit runs in a fork()ed child, where
 *  a crash is REAPED (waitpid) instead of killing ./p-kernel. The previous
 *  version restarts from the p-fs DAG (§1.4); germination/reap/rollback are
 *  appended to the self/lin autobiography (§1.3); the galaxy sees the star
 *  rebuild (world_note_rebuild, §8).
 *
 *  Hosted-only. fork/waitpid/socketpair are port-layer; this file lives in
 *  arch/linux and is compiled only when HAVE_LIBTCC. The single-threaded
 *  port (verified: zero pthread_create in arch/linux) is what makes fork()
 *  safe (§2.1) — a fact to PRESERVE.
 */

#include "kernel.h"
#include <tmonitor.h>
#include "kdds.h"
#include "pfs_dag.h"
#include "pfs_block.h"
#include "drpc.h"
#include "world.h"
#include "lm_self.h"
#include "selfc_proc.h"

#ifdef HAVE_LIBTCC

#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/resource.h>

/* errno shim (net_relay.c pattern): the T-Kernel placeholder errno.h shadows
 * the system one and lacks EINTR, so reach the real per-thread errno via
 * __errno_location and define the one constant we need locally. */
extern int *__errno_location(void) __attribute__((__const__));
#undef errno
#define errno (*__errno_location())
#ifndef EINTR
#define EINTR 4
#endif

/* ------------------------------------------------------------------ */
/* knobs (CDN-S5)                                                       */
/* ------------------------------------------------------------------ */

#define SELFC_PROBATION_MS   10000     /* §1.4: new version probation      */
/* runtime probation window (default = the §1.4 constant). The cert lowers
 * it via selfc_sup_set_probation so [selfc-rollback]'s in-probation death
 * is observable in bounded CI time WITHOUT weakening the shipped default. */
static UW selfc_probation_ms = SELFC_PROBATION_MS;
#define SELFC_RLIMIT_CPU_S   30        /* child CPU budget                 */
#define SELFC_RLIMIT_AS_MIB  64        /* child address-space budget       */
#define SELFC_SUP_POLL_MS    100       /* supervisor poll cadence          */
#define SELFC_LOG_RATE_MS    50        /* min ms between proxied log lines  */
#define SELFC_NAME_MAX       16        /* == PFS_NAME_MAX                  */
#define SELFC_SUP_MAX        4         /* supervised units this boot       */
/* reuse guard.c's give-up numbers for post-probation transient faults */
#define SELFC_BACKOFF_MS     200
#define SELFC_MAX_DEATHS     5

/* ------------------------------------------------------------------ */
/* child-side globals (the proxy stubs marshal over this fd)           */
/* ------------------------------------------------------------------ */

volatile int selfc_in_child = 0;
static int   selfc_child_fd = -1;       /* the child end of the socketpair */
static UW    selfc_child_log_last = 0;  /* rate-limit clock (child-local)  */
static char  selfc_child_name[SELFC_NAME_MAX + 1];

/* observability (cert) */
volatile U4  selfc_reaped_count  = 0;
volatile int selfc_last_termsig  = 0;
volatile int selfc_last_signaled = 0;

/* ------------------------------------------------------------------ */
/* tiny helpers (no <stdio.h> on the child path — we must stay simple) */
/* ------------------------------------------------------------------ */

static UW sp_now_ms(void)
{
    SYSTIM t; tk_get_otm(&t);
    return t.lo;
}

/* portable monotonic-ish ms for the CHILD (no T-Kernel after fork) */
static unsigned long child_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000UL);
}

/* ================================================================== */
/* CHILD SIDE — the capability proxy stubs                             */
/* ================================================================== */
/* Bound into selfc's API table (when isolation is on) IN PLACE OF the   */
/* real kernel functions. In the child they marshal a frame to the       */
/* parent; in the parent they must never run (selfc_in_child==0 -> no-op  */
/* with a defensive guard). vsnprintf is the ONE host-libc convenience    */
/* we allow on the child path — the rendered text is data, not a kernel   */
/* call.                                                                  */

#include <stdio.h>   /* vsnprintf — child-local string rendering only */

static int child_send(U1 op, U1 flags, U4 arg, const void *payload, U2 plen)
{
    if (selfc_child_fd < 0) return -1;
    if (plen > SELFC_FRAME_PAYLOAD_MAX) plen = SELFC_FRAME_PAYLOAD_MAX;

    U1 frame[sizeof(SELFC_FRAME_HDR) + SELFC_FRAME_PAYLOAD_MAX];
    SELFC_FRAME_HDR h; h.op = op; h.flags = flags; h.len = plen; h.arg = arg;
    memcpy(frame, &h, sizeof h);
    if (plen && payload) memcpy(frame + sizeof h, payload, plen);

    ssize_t w = send(selfc_child_fd, frame, sizeof h + plen, MSG_NOSIGNAL);
    return (w < 0) ? -1 : 0;
}

int selfc_proxy_printf(const unsigned char *fmt, ...)
{
    if (!selfc_in_child) return 0;     /* parent must not call this         */
    /* rate-limit: a log-spinning unit cannot starve the console (§1.2). */
    unsigned long now = child_now_ms();
    if (selfc_child_log_last && now - selfc_child_log_last < SELFC_LOG_RATE_MS) {
        /* drop spammed lines (still advance the clock so a burst settles) */
        selfc_child_log_last = now;
        return 0;
    }
    selfc_child_log_last = now;

    char buf[SELFC_FRAME_PAYLOAD_MAX];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, (const char *)fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    child_send(SELFC_SYS_LOG, 0, 0, buf, (U2)n);
    return n;
}

int selfc_proxy_dly_tsk(unsigned int ms)
{
    if (!selfc_in_child) return 0;
    /* child-local sleep — no round-trip; identical semantics for a sleeper */
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { /* resume remainder */ }
    return 0;
}

int selfc_proxy_kdds_open(const char *name, int qos)
{
    if (!selfc_in_child || !name) return -1;
    U2 nl = (U2)strnlen(name, SELFC_FRAME_PAYLOAD_MAX);
    if (child_send(SELFC_SYS_OPEN, (U1)qos, 0, name, nl) < 0) return -1;
    /* the parent validates the topic + opens; the child gets a handle back */
    SELFC_FRAME_HDR r;
    ssize_t got = recv(selfc_child_fd, &r, sizeof r, 0);
    if (got != (ssize_t)sizeof r || r.op != SELFC_SYS_OPEN) return -1;
    return (int)(W)(U4)r.arg;             /* handle, or -1 if the parent refused */
}

int selfc_proxy_kdds_pub(int h, const void *data, int len)
{
    if (!selfc_in_child || h < 0 || !data || len < 0) return -1;
    if (len > SELFC_FRAME_PAYLOAD_MAX) len = SELFC_FRAME_PAYLOAD_MAX;
    if (child_send(SELFC_SYS_PUB, 0, (U4)h, data, (U2)len) < 0) return -1;
    SELFC_FRAME_HDR r;
    ssize_t got = recv(selfc_child_fd, &r, sizeof r, 0);
    if (got != (ssize_t)sizeof r || r.op != SELFC_SYS_PUB) return -1;
    return (int)(W)(U4)r.arg;
}

int selfc_proxy_kdds_sub(int h, void *buf, int buflen, int timeout_ms)
{
    if (!selfc_in_child || h < 0 || !buf || buflen < 0) return -1;
    if (child_send(SELFC_SYS_SUB, 0, (U4)h, &timeout_ms, (U2)sizeof timeout_ms) < 0)
        return -1;
    U1 rbuf[sizeof(SELFC_FRAME_HDR) + SELFC_FRAME_PAYLOAD_MAX];
    ssize_t got = recv(selfc_child_fd, rbuf, sizeof rbuf, 0);
    if (got < (ssize_t)sizeof(SELFC_FRAME_HDR)) return -1;
    SELFC_FRAME_HDR r; memcpy(&r, rbuf, sizeof r);
    if (r.op != SELFC_SYS_SUB) return -1;
    int dlen = (int)r.len;
    if (dlen > buflen) dlen = buflen;
    if (dlen > 0) memcpy(buf, rbuf + sizeof r, (size_t)dlen);
    return (int)(W)(U4)r.arg;             /* bytes, or <0 on miss/timeout */
}

/* ================================================================== */
/* CHILD SIDE — the sandbox setup + entry call                         */
/* ================================================================== */

/* the kernel's cooperative-preemption gate (preempt.c). Setting it disables
 * T-Kernel dispatch. In the germ child we pin it HIGH for life so the
 * COW-copied scheduler can NEVER run: the child must only ever execute the
 * unit's entry(), never resume the kernel's task loop (the fork/cooperative-
 * scheduler conflict — see the long comment in sup_spawn). */
extern volatile int arch_irq_disabled_flag;

static void child_sandbox_and_run(void (*entry)(void), int child_fd,
                                   const char *name)
{
    selfc_in_child = 1;                 /* FIRST: mark child for all guards */
    arch_irq_disabled_flag = 1;         /* no dispatch in the child, ever */
    /* (0) IMMEDIATELY sever the shell's stdin: point fd 0 at /dev/null before
     *     ANYTHING else, so even within the fork window the child can never
     *     read (and steal) the console input the parent shell is consuming.
     *     fd 1/2 are re-pointed in step (2) after the close sweep. */
    {
        int z = open("/dev/null", O_RDWR);
        if (z >= 0) { dup2(z, 0); if (z != 0) close(z); }
        else close(0);
    }
    /* (1) reset inherited signal dispositions to SIG_DFL and unblock all.
     *     The parent's SIGALRM POSIX timer is NOT inherited across fork
     *     (timer_create timers don't survive fork), but the HANDLER
     *     disposition IS — a SIGSEGV here must terminate the child with a
     *     real signal (so the parent's waitpid sees WTERMSIG==SIGSEGV),
     *     not run the parent's fault handler. */
    for (int s = 1; s < 32; s++) signal(s, SIG_DFL);
    sigset_t all; sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);

    /* (2) the wire fd lives at child_fd. The unit must NOT hold the shell's
     *     stdin pipe (or it would steal the shell's input), the console fd,
     *     the relay socket, or p-fs fds. Plan: move the wire to a fixed fd
     *     (3), point 0/1/2 at /dev/null, then close everything above 3. The
     *     order matters — redirect BEFORE the close sweep so /dev/null lands
     *     deterministically. */
    #define SELFC_WIRE_FD 3
    int nul = open("/dev/null", O_RDWR);
    if (nul < 0) _exit(126);
    dup2(nul, 0);                       /* no stdin pipe for the unit          */
    dup2(nul, 1);                       /* no direct console (LOG cap only)    */
    dup2(nul, 2);
    if (dup2(child_fd, SELFC_WIRE_FD) < 0) _exit(126);   /* wire -> fd 3       */
    int wire = SELFC_WIRE_FD;
    selfc_child_fd = wire;
    if (nul > 2 && nul != wire) close(nul);
    if (child_fd != wire) close(child_fd);
    /* close everything above the wire (relay socket, p-fs fds, the original
     * stdin pipe fd if it was > 3). close_range is one syscall (RLIMIT_NOFILE
     * can be ~1M — a per-fd loop would burn seconds of sys time per fork). */
#if defined(__linux__) && defined(SYS_close_range)
    syscall(SYS_close_range, (unsigned)wire + 1u, ~0U, 0);
#else
    {
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 64 || maxfd > 4096) maxfd = 1024;
        for (long fd = wire + 1; fd <= maxfd; fd++) close((int)fd);
    }
#endif

    /* (3) resource edges (§2.1 step 3): the capability set has a resource
     *     boundary, not just an API boundary. (RLIMIT_NOFILE not set: the
     *     wire is fd 3 and we already closed everything else; capping NOFILE
     *     too low would break the few fds we keep.) */
    struct rlimit rl;
    rl.rlim_cur = rl.rlim_max = SELFC_RLIMIT_CPU_S;     setrlimit(RLIMIT_CPU, &rl);
    rl.rlim_cur = rl.rlim_max = (rlim_t)SELFC_RLIMIT_AS_MIB << 20; setrlimit(RLIMIT_AS, &rl);
    rl.rlim_cur = rl.rlim_max = 0;                      setrlimit(RLIMIT_FSIZE, &rl);
    rl.rlim_cur = rl.rlim_max = 0;                      setrlimit(RLIMIT_CORE, &rl);

    /* a recv timeout so a proxy round-trip can NEVER block the unit forever
     * (the parent might be busy or gone): proxies treat a timeout as failure. */
    {
        struct timeval tv = { 1, 0 };   /* 1s */
        setsockopt(selfc_child_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }

    /* (4) arm the proxies and run the real compiled entry via COW. */
    strncpy(selfc_child_name, name ? name : "?", SELFC_NAME_MAX);
    selfc_child_name[SELFC_NAME_MAX] = '\0';
    selfc_child_log_last = 0;
    selfc_in_child = 1;

    entry();                            /* the REAL tcc image (COW), not a stub */

    _exit(0);                           /* clean return -> clean death (§1.1) */
}

/* ================================================================== */
/* PARENT SIDE — supervisor table + frame dispatcher + rollback        */
/* ================================================================== */

#include "selfc.h"   /* selfc_compile_and_run prototype (re-compile rollback) */

typedef struct {
    char  name[SELFC_NAME_MAX + 1];
    int   used;
    pid_t pid;            /* running germ pid, -1 = none                     */
    int   wire_fd;        /* parent end of the socketpair, -1 = none         */
    void (*entry)(void);  /* current resolved entry (COW source for fork)    */
    U4    running_seq;    /* unit version currently running                  */
    UW    probation_until;/* sp_now_ms() deadline; in probation while now <  */
    int   deaths;         /* deaths for the running seq (transient backoff)  */
    int   total_deaths;   /* cumulative real deaths this sequence (cert §5.2) */
    UW    bad[16];        /* BAD-marked seqs (§1.4)                          */
    int   nbad;
    int   dead;           /* unit DEAD (seq 1 BAD, §1.4 rule 4)              */
    /* kdds handle confinement: a handle the unit opened is bound to its     */
    /* unit/<name>/ namespace; the parent owns the real handle.              */
    W     kh[8];          /* parent-side kdds handles (index = child handle) */
    int   nkh;
    /* pending-launch request (set by selfc_germ_launch from the SHELL task;
     * the SUPERVISOR task performs the actual fork() — forking from the shell
     * task corrupts the shell's stdin). */
    int   want_spawn;
    U4    pending_seq;
} SELFC_SUP;

static SELFC_SUP sup[SELFC_SUP_MAX];

static SELFC_SUP *sup_find(const char *name)
{
    for (int i = 0; i < SELFC_SUP_MAX; i++)
        if (sup[i].used && strncmp(sup[i].name, name, SELFC_NAME_MAX) == 0)
            return &sup[i];
    return NULL;
}

static SELFC_SUP *sup_alloc(const char *name)
{
    SELFC_SUP *s = sup_find(name);
    if (s) return s;
    for (int i = 0; i < SELFC_SUP_MAX; i++) if (!sup[i].used) {
        s = &sup[i];
        memset(s, 0, sizeof *s);
        strncpy(s->name, name, SELFC_NAME_MAX);
        s->name[SELFC_NAME_MAX] = '\0';
        s->used = 1; s->pid = -1; s->wire_fd = -1;
        return s;
    }
    return NULL;
}

static int seq_is_bad(SELFC_SUP *s, U4 seq)
{
    for (int i = 0; i < s->nbad; i++) if (s->bad[i] == seq) return 1;
    return 0;
}
static void mark_bad(SELFC_SUP *s, U4 seq)
{
    if (seq_is_bad(s, seq)) return;
    if (s->nbad < (int)(sizeof s->bad / sizeof s->bad[0])) s->bad[s->nbad++] = seq;
}

/* ---- parent-side capability validation: the topic allowlist (§1.2) --- */
/* a unit may open only topics under unit/<name>/ . */
static int topic_allowed(const SELFC_SUP *s, const char *topic, int tlen)
{
    char pfx[SELFC_NAME_MAX + 8];
    int pl = 0;
    const char *u = "unit/";
    for (int i = 0; u[i]; i++) pfx[pl++] = u[i];
    for (int i = 0; s->name[i] && i < SELFC_NAME_MAX; i++) pfx[pl++] = s->name[i];
    pfx[pl++] = '/';
    if (tlen < pl) return 0;
    for (int i = 0; i < pl; i++) if (topic[i] != pfx[i]) return 0;
    return 1;
}

/* drain + dispatch the unit's frames; every frame is hostile input. */
static void sup_drain_frames(SELFC_SUP *s)
{
    if (s->wire_fd < 0) return;
    for (int guard = 0; guard < 64; guard++) {     /* bounded per poll */
        U1 rbuf[sizeof(SELFC_FRAME_HDR) + SELFC_FRAME_PAYLOAD_MAX];
        ssize_t n = recv(s->wire_fd, rbuf, sizeof rbuf, MSG_DONTWAIT);
        if (n < (ssize_t)sizeof(SELFC_FRAME_HDR)) break;   /* EAGAIN / closed */
        SELFC_FRAME_HDR h; memcpy(&h, rbuf, sizeof h);
        if (h.len > SELFC_FRAME_PAYLOAD_MAX) break;        /* malformed */
        if ((ssize_t)(sizeof h + h.len) > n) break;        /* truncated */
        const U1 *pl = rbuf + sizeof h;

        switch (h.op) {
        case SELFC_SYS_LOG: {
            char line[SELFC_FRAME_PAYLOAD_MAX + 1];
            U2 ln = h.len; if (ln > SELFC_FRAME_PAYLOAD_MAX) ln = SELFC_FRAME_PAYLOAD_MAX;
            memcpy(line, pl, ln); line[ln] = '\0';
            tm_printf((const UB *)"[unit:%s] %s", (const UB *)s->name, (const UB *)line);
            break;
        }
        case SELFC_SYS_OPEN: {
            SELFC_FRAME_HDR r; r.op = SELFC_SYS_OPEN; r.flags = 0; r.len = 0; r.arg = (U4)-1;
            char topic[KDDS_NAME_MAX];
            int tl = (int)h.len; if (tl >= (int)sizeof topic) tl = (int)sizeof topic - 1;
            memcpy(topic, pl, tl); topic[tl] = '\0';
            if (topic_allowed(s, topic, tl) && s->nkh < (int)(sizeof s->kh / sizeof s->kh[0])) {
                W real = kdds_open_poll(topic, (W)h.flags);
                if (real >= 0) { s->kh[s->nkh] = real; r.arg = (U4)s->nkh; s->nkh++; }
            } else {
                tm_printf((const UB *)"[unit:%s] OPEN '%s' REFUSED "
                          "(topic outside unit/%s/ — selfc-ring3 §1.2)\n",
                          (const UB *)s->name, (const UB *)topic, (const UB *)s->name);
            }
            send(s->wire_fd, &r, sizeof r, MSG_NOSIGNAL);
            break;
        }
        case SELFC_SYS_PUB: {
            SELFC_FRAME_HDR r; r.op = SELFC_SYS_PUB; r.flags = 0; r.len = 0; r.arg = (U4)-1;
            int hi = (int)h.arg;
            if (hi >= 0 && hi < s->nkh)
                r.arg = (U4)(int)kdds_pub(s->kh[hi], pl, (W)h.len);
            send(s->wire_fd, &r, sizeof r, MSG_NOSIGNAL);
            break;
        }
        case SELFC_SYS_SUB: {
            U1 obuf[sizeof(SELFC_FRAME_HDR) + KDDS_DATA_MAX];
            SELFC_FRAME_HDR r; r.op = SELFC_SYS_SUB; r.flags = 0; r.len = 0; r.arg = (U4)-1;
            int hi = (int)h.arg;
            if (hi >= 0 && hi < s->nkh) {
                UB data[KDDS_DATA_MAX];
                W got = kdds_sub(s->kh[hi], data, sizeof data, 0 /* poll */);
                if (got >= 0) {
                    int dl = (int)got; if (dl > KDDS_DATA_MAX) dl = KDDS_DATA_MAX;
                    r.arg = (U4)dl; r.len = (U2)dl;
                    memcpy(obuf + sizeof r, data, dl);
                } else r.arg = (U4)(int)got;
            }
            memcpy(obuf, &r, sizeof r);
            send(s->wire_fd, obuf, sizeof r + r.len, MSG_NOSIGNAL);
            break;
        }
        default:
            /* unknown op — reject by dropping (the proxy will time-out). The
             * dispatcher is boundary code; it never trusts h.op blindly. */
            tm_printf((const UB *)"[unit:%s] dropped unknown frame op=%d "
                      "(selfc-ring3 §1.2 op-whitelist)\n",
                      (const UB *)s->name, (int)h.op);
            break;
        }
    }
}

/* close the wire + reap any handles for a finished germ. */
static void sup_release_germ(SELFC_SUP *s)
{
    if (s->wire_fd >= 0) { close(s->wire_fd); s->wire_fd = -1; }
    for (int i = 0; i < s->nkh; i++) kdds_close(s->kh[i]);
    s->nkh = 0;
    s->pid = -1;
}

/* fork a germ for (entry, name, seq); fills s->pid + s->wire_fd. 0 = ok. */
static int sup_spawn(SELFC_SUP *s, void (*entry)(void), U4 seq)
{
    if (selfc_in_child) _exit(0);       /* a child must never fork (see pump) */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
        tm_printf((const UB *)"[selfc-proc] socketpair failed\n");
        return -1;
    }
    /* FORK SAFETY (the real conflict the design flagged): the kernel is one
     * thread preempted by a SIGALRM POSIX timer that runs the T-Kernel
     * scheduler. fork() must be ATOMIC w.r.t. that preemption — a SIGALRM
     * landing mid-fork() (the glibc wrapper takes locks / touches TLS on the
     * raw-asm task stack) corrupts the parent's scheduler state and wedges it
     * in the idle sigsuspend afterwards. Disable T-Kernel dispatch across the
     * fork via arch_irq_disabled_flag (the kernel's own critical-section gate,
     * preempt.c) and re-enable+drain after — the bare-metal DI/EI discipline,
     * hosted. POSIX timers do not survive fork, so the CHILD has no
     * preemption; it also pins the flag high for life (child_sandbox). */
    extern volatile int arch_irq_disabled_flag;
    extern void arch_irq_enable_with_drain(void);
    arch_irq_disabled_flag = 1;             /* enter critical section (parent) */
    pid_t pid = fork();
    if (pid < 0) {
        arch_irq_enable_with_drain();
        close(sv[0]); close(sv[1]);
        tm_printf((const UB *)"[selfc-proc] fork failed\n");
        return -1;
    }
    if (pid == 0) {
        /* CHILD: sandbox (resets signals to SIG_DFL, pins dispatch off) and
         * run only entry(); never returns into the kernel task loop. */
        child_sandbox_and_run(entry, sv[1], s->name);
        _exit(127);                      /* unreachable */
    }
    /* PARENT: leave the critical section, draining any ticks SIGALRM
     * collapsed while dispatch was disabled (so tk_dly_tsk timing is intact). */
    arch_irq_enable_with_drain();
    close(sv[1]);
    s->pid     = pid;
    s->wire_fd = sv[0];
    s->entry   = entry;
    s->running_seq = seq;
    s->probation_until = sp_now_ms() + selfc_probation_ms;
    s->deaths  = 0;
    s->nkh     = 0;
    return 0;
}

/* recompile + resolve the entry for version <seq> of unit <name>, used by
 * rollback. Reuses selfc_compile_and_run's compile path indirectly via the
 * helper exported from selfc.c (selfc_resolve_unit). Returns entry or NULL. */
IMPORT void *selfc_resolve_unit(const char *name, U4 seq);

/* roll back to seq-1 (skip BAD seqs); restart it. §1.4 rule 2/4. */
static void sup_rollback(SELFC_SUP *s)
{
    U4 from = s->running_seq;
    U4 seq  = (from > 0) ? from - 1 : 0;
    while (seq >= 1 && seq_is_bad(s, seq)) seq--;
    if (seq < 1) {
        s->dead = 1;
        tm_printf((const UB *)"[selfc-proc] unit '%s' DEAD — no good version "
                  "below seq %u (evolution may fail; the node lives, §1.4)\n",
                  (const UB *)s->name, (unsigned)from);
        return;
    }
    void *entry = selfc_resolve_unit(s->name, seq);
    if (!entry) {
        tm_printf((const UB *)"[selfc-proc] rollback: could not resolve "
                  "'%s'@%u\n", (const UB *)s->name, (unsigned)seq);
        s->dead = 1;
        return;
    }
    (void)lm_self_append_unit_event(LM_UNIT_EV_ROLLBACK, from, 0);
    world_note_rebuild();
    tm_printf((const UB *)"[selfc-proc] rollback '%s' %u->%u — restarting the "
              "previous version\n", (const UB *)s->name, (unsigned)from, (unsigned)seq);
    if (sup_spawn(s, (void (*)(void))entry, seq) == 0) {
        (void)lm_self_append_unit_event(LM_UNIT_EV_GERM, seq, 0);
    }
}

/* a germ exited: apply the §1.4 rollback rule. */
static void sup_on_death(SELFC_SUP *s, int status)
{
    int signaled = WIFSIGNALED(status);
    int termsig  = signaled ? WTERMSIG(status) : 0;
    int exitcode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    UW  now      = sp_now_ms();
    int in_probation = (now < s->probation_until);

    selfc_reaped_count++;
    selfc_last_signaled = signaled;
    selfc_last_termsig  = termsig;

    /* clean exit (returned / _exit(0)) is NOT a rollback trigger (§1.1):
     * the unit finished its work — mark it DONE, drain remaining frames. */
    if (!signaled && exitcode == 0) {
        tm_printf((const UB *)"[selfc-proc] unit '%s'@%u exited cleanly "
                  "(DONE, not a rollback)\n",
                  (const UB *)s->name, (unsigned)s->running_seq);
        sup_release_germ(s);
        return;
    }

    /* a real death: record it autobiographically. */
    s->total_deaths++;        /* cumulative for the cert (§5.2 deaths==1) */
    (void)lm_self_append_unit_event(LM_UNIT_EV_REAP, s->running_seq, (UB)termsig);
    tm_printf((const UB *)"[selfc-proc] REAPED '%s'@%u  signaled=%d termsig=%d "
              "exit=%d  (kernel untouched — parent observed the death)\n",
              (const UB *)s->name, (unsigned)s->running_seq,
              signaled, termsig, exitcode);

    sup_release_germ(s);

    if (in_probation) {
        /* §1.4 rule 2: death during probation -> BAD + immediate rollback. */
        mark_bad(s, s->running_seq);
        sup_rollback(s);
    } else {
        /* §1.4 rule 3: post-probation transient fault -> backoff respawn,
         * up to SELFC_MAX_DEATHS, then BAD + rollback. */
        s->deaths++;
        if (s->deaths > SELFC_MAX_DEATHS) {
            mark_bad(s, s->running_seq);
            sup_rollback(s);
        } else {
            UW backoff = (UW)SELFC_BACKOFF_MS << (s->deaths - 1);
            tm_printf((const UB *)"[selfc-proc] '%s'@%u transient death %d/%d "
                      "— respawn after %ums\n", (const UB *)s->name,
                      (unsigned)s->running_seq, s->deaths, SELFC_MAX_DEATHS, (unsigned)backoff);
            tk_dly_tsk((INT)backoff);
            if (sup_spawn(s, s->entry, s->running_seq) == 0)
                (void)lm_self_append_unit_event(LM_UNIT_EV_GERM, s->running_seq, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* public launch (§2.1): fork the germ, hand it to the supervisor.     */
/* ------------------------------------------------------------------ */

/* perform an enqueued launch — runs ONLY in the supervisor task: it COMPILES
 * (selfc_resolve_unit, so all tcc state lives in this one task) and fork()s.
 * Returns 0 on a successful fork. */
static int sup_do_spawn_request(SELFC_SUP *s)
{
    U4 seq = s->pending_seq;
    s->want_spawn = 0;
    s->dead = 0; s->nbad = 0; s->total_deaths = 0;
    void *entry = selfc_resolve_unit(s->name, seq);   /* compile in THIS task */
    if (!entry) {
        tm_printf((const UB *)"[selfc-proc] '%s'@%u: resolve failed\n",
                  (const UB *)s->name, (unsigned)seq);
        s->dead = 1;
        return -1;
    }
    if (sup_spawn(s, (void (*)(void))entry, seq) < 0) return -1;
    (void)lm_self_append_unit_event(LM_UNIT_EV_GERM, seq, 0);
    world_note_rebuild();
    tm_printf((const UB *)"[selfc-proc] germinated '%s'@%u as pid %d "
              "(probation %ums — a crash here is REAPED, not fatal)\n",
              (const UB *)s->name, (unsigned)seq, (int)s->pid, (unsigned)selfc_probation_ms);
    return 0;
}

/* ENQUEUE a germination (called from the shell task). The supervisor task
 * compiles + fork()s — the shell task (which owns the console stdin) never
 * forks and never runs tcc concurrently with the supervisor. Returns 0. */
int selfc_germ_launch(const char *name, U4 unit_seq)
{
    if (!name) return -1;
    SELFC_SUP *s = sup_alloc(name);
    if (!s) { tm_printf((const UB *)"[selfc-proc] supervisor table full\n"); return -1; }
    if (s->pid > 0 || s->want_spawn) {
        tm_printf((const UB *)"[selfc-proc] '%s' already germinating/running\n",
                  (const UB *)name);
        return -1;
    }
    s->pending_seq = unit_seq;
    s->want_spawn  = 1;         /* the supervisor task picks this up */
    return 0;
}

/* ------------------------------------------------------------------ */
/* the supervisor task: poll waitpid(WNOHANG), drain frames, act.      */
/* ------------------------------------------------------------------ */

/* one supervisor pass: service pending launches (fork HERE, in the supervisor
 * task), drain frames, reap exited germs + roll back. MUST run only in the
 * supervisor task — it fork()s, and forking from the shell task corrupts the
 * shell's stdin (the COW child re-enters the cooperative scheduler). */
void selfc_proc_pump(void)
{
    /* SELF-DEFENSE against the fork/cooperative-scheduler hazard: if a germ
     * CHILD ever re-enters supervisor code (its COW copy of the kernel can
     * resume the supervisor task after entry() yields/blocks), it must NOT
     * fork/waitpid as if it were the parent — it must just die. selfc_in_child
     * is set as the child's first action, so this is a reliable guard. */
    if (selfc_in_child) _exit(0);
    for (int i = 0; i < SELFC_SUP_MAX; i++) {
        SELFC_SUP *s = &sup[i];
        if (!s->used) continue;
        if (s->want_spawn && s->pid <= 0) sup_do_spawn_request(s);  /* fork here */
        if (s->pid <= 0) continue;
        sup_drain_frames(s);
        int status;
        pid_t r = waitpid(s->pid, &status, WNOHANG);
        if (r == s->pid) {
            sup_drain_frames(s);          /* drain any last frames */
            sup_on_death(s, status);      /* may rollback -> sup_spawn (in-task) */
        }
    }
}

void selfc_proc_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for (;;) {
        selfc_proc_pump();
        tk_dly_tsk(SELFC_SUP_POLL_MS);
    }
}

/* ------------------------------------------------------------------ */
/* cert read hooks                                                     */
/* ------------------------------------------------------------------ */

int selfc_sup_running_seq(const char *name)
{
    SELFC_SUP *s = sup_find(name);
    if (!s || s->pid <= 0) return -1;
    return (int)s->running_seq;
}
int selfc_sup_seq_is_bad(const char *name, U4 seq)
{
    SELFC_SUP *s = sup_find(name);
    return (s && seq_is_bad(s, seq)) ? 1 : 0;
}
int selfc_sup_deaths(const char *name)
{
    SELFC_SUP *s = sup_find(name);
    return s ? s->total_deaths : -1;       /* cumulative deaths (§5.2)        */
}
int selfc_sup_is_dead(const char *name)
{
    SELFC_SUP *s = sup_find(name);
    return (s && s->dead) ? 1 : 0;
}
void selfc_sup_set_probation(UW ms) { selfc_probation_ms = ms; }
void selfc_sup_reset(void)
{
    if (selfc_in_child) _exit(0);       /* a child must never reap/kill germs */
    /* SIGKILL every germ first, then reap WITHOUT blocking indefinitely: a
     * blocking waitpid could stall the shell task if a child is mid-syscall.
     * Poll WNOHANG with a bounded yield budget; SIGKILL is delivered promptly. */
    for (int i = 0; i < SELFC_SUP_MAX; i++)
        if (sup[i].used && sup[i].pid > 0) kill(sup[i].pid, SIGKILL);
    for (int i = 0; i < SELFC_SUP_MAX; i++) {
        if (sup[i].used && sup[i].pid > 0) {
            int st, reaped = 0;
            for (int t = 0; t < 50; t++) {       /* up to ~500ms */
                if (waitpid(sup[i].pid, &st, WNOHANG) == sup[i].pid) { reaped = 1; break; }
                tk_dly_tsk(10);
            }
            (void)reaped;
        }
        if (sup[i].wire_fd >= 0) close(sup[i].wire_fd);
        memset(&sup[i], 0, sizeof sup[i]);
        sup[i].pid = -1; sup[i].wire_fd = -1;
    }
    /* sweep up ANY orphan germ (incl. COW children the fork/scheduler hazard
     * may have spawned outside the table): SIGKILL the process group's stray
     * children and reap them non-blocking, so none linger holding inherited
     * pipe fds after the cert. */
    {
        pid_t r; int st, guard = 0;
        while ((r = waitpid(-1, &st, WNOHANG)) > 0 && guard++ < 64) { /* drain */ }
    }
}

#endif /* HAVE_LIBTCC */
