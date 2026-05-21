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

/* The flag itself — read and written by cpu_support.S via adrp/add.
 * Volatile sig_atomic_t so concurrent updates from signal handler and
 * task context don't tear. */
volatile int arch_irq_disabled_flag = 0;

static volatile sig_atomic_t pending_timer_ticks = 0;

/* T-Kernel timer bridge — defined in cpu_support.S. Increments
 * knl_taskindp around the call to knl_timer_handler. */
extern void knl_timer_handler_startup(void);

void arch_irq_enable_with_drain(void)
{
    arch_irq_disabled_flag = 0;
    /* Drain any deferred ticks. We do NOT attempt to swallow them all
     * at once — calling the timer handler multiple times in a row
     * could starve the very task that just re-enabled IRQs. One tick
     * per re-enable is plenty; the next SIGALRM will deliver the
     * rest at its natural cadence. */
    if (pending_timer_ticks > 0) {
        pending_timer_ticks = 0;
        knl_timer_handler_startup();
    }
}

static void sigalrm_handler(int sig)
{
    (void)sig;
    if (arch_irq_disabled_flag) {
        pending_timer_ticks++;
        return;
    }
    knl_timer_handler_startup();
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
    static timer_t tid;
    static int created = 0;
    if (!created) {
        struct sigevent sev = { 0 };
        sev.sigev_notify = SIGEV_SIGNAL;
        sev.sigev_signo  = SIGALRM;
        if (timer_create(CLOCK_MONOTONIC, &sev, &tid) < 0) {
            perror("timer_create"); exit(1);
        }
        created = 1;
    }
    struct itimerspec its = {
        .it_interval = { .tv_sec = period_us / 1000000,
                         .tv_nsec = (long)(period_us % 1000000) * 1000 },
        .it_value    = { .tv_sec = period_us / 1000000,
                         .tv_nsec = (long)(period_us % 1000000) * 1000 },
    };
    if (timer_settime(tid, 0, &its, NULL) < 0) {
        perror("timer_settime"); exit(1);
    }
}
