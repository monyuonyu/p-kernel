/*
 *  arch/linux/aarch64/preempt.c
 *  Signal-as-IRQ infrastructure, factored out of poc_preempt.c so the
 *  T-Kernel build can link against it.
 *
 *  Three things live here:
 *    1. arch_irq_disabled_flag and arch_irq_enable_with_drain —
 *       referenced by cpu_support.S and by cpu_status.h's macros.
 *    2. The SIGALRM signal handler. It calls into T-Kernel's
 *       knl_timer_handler_startup, which advances time and may set
 *       knl_schedtsk != knl_ctxtsk. We do NOT manipulate mcontext
 *       here — preemption happens at the next END_CRITICAL_SECTION
 *       (matching T-Kernel's bare-metal "preempt at safe point"
 *       semantics).
 *    3. arch_signals_init and arch_timer_start — called from
 *       tkdev_init.c during boot.
 *
 *  The Session 2 mcontext-rewrite approach is interesting but does not
 *  match T-Kernel's expectation that knl_dispatch is only entered from
 *  task context. Bridging that into the timer signal handler is
 *  Session 4 work.
 */

/* preempt.c does NOT include any T-Kernel headers — it only references
 * arch_irq_disabled_flag (a plain int) and the asm-defined symbol
 * knl_timer_handler_startup. Pure POSIX is enough. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

/* The flag itself — read and written by cpu_support.S via adrp/add.
 * Volatile sig_atomic_t so concurrent updates from signal handler and
 * task context don't tear. */
volatile int arch_irq_disabled_flag = 0;

static volatile sig_atomic_t pending_timer_ticks = 0;

/* The POSIX timer that delivers SIGALRM. File-scope so the signal
 * handler can call timer_getoverrun() to recover ticks Linux collapsed
 * while we were not running. */
static timer_t arch_alarm_tid;
static int     arch_alarm_tid_created = 0;

/* T-Kernel timer bridge — defined in cpu_support.S. Increments
 * knl_taskindp around the call to knl_timer_handler. */
extern void knl_timer_handler_startup(void);

extern void sio_send_frame(const unsigned char *buf, int size);

void arch_linux_trampoline_marker(void)
{
    sio_send_frame((const unsigned char *)"[asm] trampoline reached\r\n", 26);
}

void arch_linux_dispatch_loop_marker(void)
{
    sio_send_frame((const unsigned char *)"[asm] dispatch loop entered\r\n", 29);
}

static void hexdump(const char *lbl, int lbl_len, unsigned long v) {
    unsigned char buf[24];
    sio_send_frame((const unsigned char *)lbl, lbl_len);
    for (int i = 0; i < 16; i++) {
        int n = (v >> ((15 - i) * 4)) & 0xF;
        buf[i] = (n < 10) ? ('0' + n) : ('a' + n - 10);
    }
    buf[16] = '\r'; buf[17] = '\n';
    sio_send_frame(buf, 18);
}

void arch_linux_dispatch_after_cbz(unsigned long sched) { hexdump("[asm] after cbz, sched=", 23, sched); }
void arch_linux_dispatch_after_sp_switch(unsigned long sp_v) { hexdump("[asm] after mov sp, sp=", 23, sp_v); }
void arch_linux_dispatch_after_restore(unsigned long x30_v) { hexdump("[asm] after restore, x30=", 25, x30_v); }

void arch_linux_trampoline_about_to_br(unsigned long x17)
{
    unsigned char buf[24];
    sio_send_frame((const unsigned char *)"[asm] br x17 target=", 20);
    for (int i = 0; i < 16; i++) {
        int n = (x17 >> ((15 - i) * 4)) & 0xF;
        buf[i] = (n < 10) ? ('0' + n) : ('a' + n - 10);
    }
    buf[16] = '\r'; buf[17] = '\n';
    sio_send_frame(buf, 18);
}

void arch_irq_enable_with_drain(void)
{
    arch_irq_disabled_flag = 0;
    /* Drain every deferred tick. Bursts at re-enable time are
     * acceptable; dropping ticks is not, because tk_dly_tsk and other
     * timeout-bearing waits drift behind wall-clock by the amount we
     * drop. The handler call is short (queue walk + a few comparisons)
     * so even a 100-tick catch-up after a 1 s IRQ-disabled window
     * costs well under a millisecond. */
    sig_atomic_t to_replay = pending_timer_ticks;
    pending_timer_ticks = 0;
    while (to_replay-- > 0) {
        knl_timer_handler_startup();
    }
}

static void sigalrm_handler(int sig)
{
    (void)sig;
    /* Linux collapses missed expirations of a periodic POSIX timer
     * into a single signal delivery and exposes the count via
     * timer_getoverrun(). Replay all of them so tick count tracks
     * wall-clock even after the process was descheduled. */
    int overrun = arch_alarm_tid_created ? timer_getoverrun(arch_alarm_tid) : 0;
    if (overrun < 0) overrun = 0;
    int to_run = 1 + overrun;

    if (arch_irq_disabled_flag) {
        pending_timer_ticks += to_run;
        return;
    }
    while (to_run-- > 0) {
        knl_timer_handler_startup();
    }
}

#define SIGSTACK_SZ (64 * 1024)
static unsigned char signal_stack[SIGSTACK_SZ] __attribute__((aligned(16)));

void arch_signals_init(void)
{
    stack_t ss = {
        .ss_sp = signal_stack, .ss_size = SIGSTACK_SZ, .ss_flags = 0,
    };
    if (sigaltstack(&ss, NULL) < 0) { perror("sigaltstack"); exit(1); }

    struct sigaction sa = { 0 };
    sa.sa_handler = sigalrm_handler;
    sa.sa_flags = SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, NULL) < 0) { perror("sigaction"); exit(1); }
}

void arch_timer_start(unsigned long period_us)
{
    if (!arch_alarm_tid_created) {
        struct sigevent sev = { 0 };
        /* Pin SIGALRM delivery to THIS thread (the T-Kernel thread).
         *
         * SIGEV_SIGNAL is process-directed: when the kernel runs on a
         * pthread inside a launcher (so_node, the Android JNI bridge),
         * Linux may deliver the tick to the launcher's main thread
         * instead. knl_timer_handler_startup then runs CONCURRENTLY
         * with task code on the kernel thread — arch_irq_disabled_flag
         * cannot protect across threads — and corrupts the ready/timer
         * queues. Observed in multi-node so_node runs as (a) a survivor
         * segfault and (b) a livelock: two nodes spinning forever at
         * the same PC in knl_tstdlib_bitsearch1 under
         * knl_ready_queue_delete, whose inner loop never terminates
         * once the bitmap/top_priority invariant is broken.
         *
         * arch_timer_start() is called during boot on the kernel
         * thread, so gettid() here is exactly the thread the signal-
         * as-IRQ emulation expects. */
#if defined(SIGEV_THREAD_ID)
        sev.sigev_notify = SIGEV_THREAD_ID;
# if defined(sigev_notify_thread_id)
        sev.sigev_notify_thread_id = (pid_t)syscall(SYS_gettid);
# else
        sev._sigev_un._tid         = (pid_t)syscall(SYS_gettid);
# endif
#else
        sev.sigev_notify = SIGEV_SIGNAL;   /* single-threaded fallback */
#endif
        sev.sigev_signo  = SIGALRM;
        if (timer_create(CLOCK_MONOTONIC, &sev, &arch_alarm_tid) < 0) {
            perror("timer_create"); exit(1);
        }
        arch_alarm_tid_created = 1;
    }
    struct itimerspec its = {
        .it_interval = { .tv_sec = period_us / 1000000,
                         .tv_nsec = (long)(period_us % 1000000) * 1000 },
        .it_value    = { .tv_sec = period_us / 1000000,
                         .tv_nsec = (long)(period_us % 1000000) * 1000 },
    };
    if (timer_settime(arch_alarm_tid, 0, &its, NULL) < 0) {
        perror("timer_settime"); exit(1);
    }
}
