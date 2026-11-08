/*
 *  arch/linux/x86_64/preempt.c
 *  Signal-as-IRQ infrastructure (x86_64 sibling). Same code as the
 *  aarch64 version — POSIX-only; only the debug helpers used by the
 *  aarch64 dispatcher's diagnostic call-outs were dropped because the
 *  x86_64 cpu_support.S does not call them.
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

/* The flag itself — read and written by cpu_support.S via direct
 * RIP-relative mov. Volatile so concurrent updates from signal handler
 * and task context don't tear. */
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

/* Examined opaquely (NULL / non-NULL only) so preempt.c stays free of
 * T-Kernel headers, per the file banner. Real type is TCB *. */
extern void * volatile knl_schedtsk;

/*
 *  knl_idle_wait — sleep the kernel thread until the next SIGALRM tick.
 *
 *  Called from the dispatcher's idle path (.Lidle in cpu_support.S) in
 *  place of the old sched_yield() busy-spin, which returned immediately
 *  and pegged a core at ~100% CPU when no task was runnable (observed
 *  live: a kernel pthread eating one core for 25 min, starving the UI).
 *
 *  A task can become runnable only from running-task context (then we
 *  are not idle) or from the SIGALRM handler (knl_timer_handler_startup
 *  may set knl_schedtsk). So sleeping until SIGALRM is exactly correct.
 *
 *  Race-free: block SIGALRM, then re-check knl_schedtsk. If a task is
 *  now runnable the tick that made it so is held PENDING (not lost), so
 *  we return at once without sleeping. Otherwise sigsuspend() atomically
 *  installs a mask that unblocks SIGALRM and sleeps; the pending/next
 *  tick delivers, sigalrm_handler runs (arch_irq_disabled_flag is 0 on
 *  this path — .Lidle did IRQ_FLAG_CLEAR — so the tick runs directly and
 *  may set knl_schedtsk), and sigsuspend returns with EINTR. We restore
 *  the prior mask and the dispatcher loops to re-check schedtsk.
 *
 *  Worst-case wake latency is one tick (~10 ms at 100 Hz): the bounded,
 *  documented tradeoff for going from 100% CPU to idle. The periodic
 *  timer keeps firing, so a missed-wakeup HANG is impossible.
 *
 *  Mask choice: we unblock exactly SIGALRM and leave everything else as
 *  the caller had it — fault signals (SIGSEGV/SIGBUS/SIGFPE) and term
 *  signals (SIGINT/SIGTERM) stay deliverable, so crash handling and
 *  Ctrl-C behave exactly as before.
 */
void knl_idle_wait(void)
{
    sigset_t block_alrm, prev, wait_mask;
    sigemptyset(&block_alrm);
    sigaddset(&block_alrm, SIGALRM);
    sigprocmask(SIG_BLOCK, &block_alrm, &prev);

    if (knl_schedtsk == 0) {
        wait_mask = prev;
        sigdelset(&wait_mask, SIGALRM);
        sigsuspend(&wait_mask);   /* sleeps; -1/EINTR after a signal */
    }

    sigprocmask(SIG_SETMASK, &prev, NULL);
}

#ifndef PK_HOSTED_NO_EXIT_DISPATCH
/* ---- ROADMAP 0-1, RNG0 on hosted Linux: switch at the "interrupt" exit ----
 * (ledger RNG0-HOSTED-NO-SIGNAL-EXIT-DISPATCH). Before this, a tick that made
 * a higher-priority task ready only took effect at the running task's next
 * END_CRITICAL_SECTION, so a task spinning without kernel calls starved it
 * (bare x86 / AArch64 switch at the IRQ exit since 2026-09-29 / 10-01).
 *
 * knl_dispatch must still be entered from task context, never inside the
 * handler. So the handler only REWRITES the interrupted context: it pushes
 * the interrupted RIP below the 128-byte red zone and resumes the task at
 * arch_preempt_tramp. After sigreturn (normal mask) the trampoline moves to
 * this task's SIDE STACK, saves every register the C ABI does not (rax..r11,
 * flags, x87/SSE via fxsave64), calls arch_preempt_dispatch (whose dispatch
 * frame also lives there while the task is switched out), restores them,
 * returns to the task stack and `ret`s to the interrupted instruction.
 * The side stack (one per task id, its top word = "this task is inside the
 * trampoline") keeps the task stack's extra use to the 8-byte return
 * address (hosted tasks run on 2-4 KB stacks; ~800 B more per preemption
 * was judged too close — a precaution, not a measured overflow). The final
 * `ret $128` gives back the red zone skipped below the interrupted RSP: the
 * first two versions used a plain `ret`, left RSP 128 B low, and the resumed
 * function crashed (SIGSEGV in `handoff stream`, 2026-10-04).
 *
 * Only when the interrupted RIP is in p-kernel's own text and not in the
 * trampoline: inside libc the task may hold a libc lock (malloc's is not
 * recursive) and the next task would deadlock on it — that case keeps the
 * old behaviour (switch at the next END_CRITICAL_SECTION). "Own text" is
 * pk_text_begin..pk_text_end (first / last object on the link line), not
 * __executable_start..__etext: the build is -static, so that range holds
 * glibc too (audit-20 measured 86-138 switches a run near malloc). The
 * layout is checked once at boot (libc functions outside, this file inside);
 * if it fails, the exit dispatch stays off and says so. A task's busy word
 * is cleared when the task is (re)initialised (knl_setup_context), so a task
 * deleted while switched out inside the trampoline does not leave its id
 * unpreemptable for the next task with that id. Limits: AVX upper
 * halves are not saved (the hosted build does not use -mavx). The aarch64 /
 * Android sibling returns through rt_sigreturn instead (see its preempt.c).
 * -DPK_HOSTED_NO_EXIT_DISPATCH: the old behaviour (negative control). */
#include <ucontext.h>
#include <stdint.h>
#include <pthread.h>

extern void * volatile knl_ctxtsk;
extern volatile int knl_dispatch_disabled;
extern volatile int knl_taskindp;            /* low 32 bits; dispatch.S uses incl */
extern void knl_dispatch_entry(void);
extern int tk_get_tid(void);
extern char pk_text_begin[], pk_text_end[];  /* pk_text_begin.c / pk_text_end.c */
void arch_preempt_tramp(void);
void arch_preempt_dispatch(void);
extern char arch_preempt_tramp_end[];

#define SIDE_SLOTS 129                       /* task ids 1..128 (CNF_MAX_TSKID) */
#define SIDE_SZ    8192                      /* save area + dispatch frame + drain */
static uint64_t side_stack[SIDE_SLOTS][SIDE_SZ / 8] __attribute__((aligned(64)));
uint64_t arch_preempt_side_sp;               /* handler -> trampoline (one at a time) */
uint64_t arch_preempt_tmp_sp;                /* trampoline's first 3 instructions */
static int pk_text_ok;                       /* set by pk_text_check() at boot */

static int in_pk_text(uintptr_t a)
{
    return a >= (uintptr_t)pk_text_begin && a < (uintptr_t)pk_text_end;
}

/* the link-order bracket must hold libc outside and p-kernel inside */
static void pk_text_check(void)
{
    const uintptr_t libc[] = {
        (uintptr_t)&malloc, (uintptr_t)&free, (uintptr_t)&memcpy,
        (uintptr_t)&printf, (uintptr_t)&pthread_mutex_lock,
    };
    int ok = (uintptr_t)pk_text_begin < (uintptr_t)pk_text_end &&
             in_pk_text((uintptr_t)&arch_preempt_dispatch) &&
             in_pk_text((uintptr_t)&knl_dispatch_entry);
    for (unsigned i = 0; i < sizeof libc / sizeof libc[0]; i++)
        if (in_pk_text(libc[i])) ok = 0;
    pk_text_ok = ok;
    if (!ok)
        fprintf(stderr, "[preempt] p-kernel text bracket %p..%p holds libc or misses "
                "the kernel: exit dispatch OFF (old behaviour)\n",
                (void *)pk_text_begin, (void *)pk_text_end);
}

/* pk_nosw.h: per-task no-switch depth (index = task id) */
static unsigned short nosw_depth[SIDE_SLOTS];

/* knl_setup_context (task created / gone dormant): a task deleted while
 * switched out inside the trampoline must not leave its id marked busy,
 * nor a task deleted inside a no-switch region its id unpreemptable */
void arch_preempt_forget(int tskid)
{
    if (tskid > 0 && tskid < SIDE_SLOTS) {
        side_stack[tskid][SIDE_SZ / 8 - 1] = 0;
        nosw_depth[tskid] = 0;
    }
}

/* called by arch_preempt_tramp on the side stack, in task context */
void arch_preempt_dispatch(void)
{
    arch_irq_disabled_flag = 1;
    if (knl_ctxtsk != 0 && knl_ctxtsk != knl_schedtsk &&
        !knl_dispatch_disabled && knl_taskindp == 0)
        knl_dispatch_entry();               /* returns here when rescheduled */
    arch_irq_enable_with_drain();
}

__asm__(
    "    .text\n"
    "    .globl arch_preempt_tramp\n"
    "    .globl arch_preempt_tramp_end\n"
    "    .type  arch_preempt_tramp, @function\n"
    "    .balign 16\n"
    "arch_preempt_tramp:\n"
    "    movq  %rsp, arch_preempt_tmp_sp(%rip)\n"
    "    movq  arch_preempt_side_sp(%rip), %rsp\n"     /* = &top[-1], the busy word */
    "    pushq arch_preempt_tmp_sp(%rip)\n"            /* top[-2] = the task's rsp */
    "    pushfq\n"
    "    pushq %rax\n    pushq %rcx\n    pushq %rdx\n    pushq %rsi\n"
    "    pushq %rdi\n    pushq %r8\n     pushq %r9\n     pushq %r10\n"
    "    pushq %r11\n"
    "    pushq %rbp\n"
    "    movq  %rsp, %rbp\n"
    "    subq  $512, %rsp\n"
    "    andq  $-64, %rsp\n"
    "    fxsave64 (%rsp)\n"
    "    call  arch_preempt_dispatch\n"
    /* -DPK_PREEMPT_DROP_FP: zero xmm0-15 instead of restoring them (the
     * negative control for `tkconf fp` F1, FP-PRESERVE) */
#ifdef PK_PREEMPT_DROP_FP
    "    pxor  %xmm0, %xmm0\n"
    "    pxor  %xmm1, %xmm1\n"
    "    pxor  %xmm2, %xmm2\n"
    "    pxor  %xmm3, %xmm3\n"
    "    pxor  %xmm4, %xmm4\n"
    "    pxor  %xmm5, %xmm5\n"
    "    pxor  %xmm6, %xmm6\n"
    "    pxor  %xmm7, %xmm7\n"
    "    pxor  %xmm8, %xmm8\n"
    "    pxor  %xmm9, %xmm9\n"
    "    pxor  %xmm10, %xmm10\n"
    "    pxor  %xmm11, %xmm11\n"
    "    pxor  %xmm12, %xmm12\n"
    "    pxor  %xmm13, %xmm13\n"
    "    pxor  %xmm14, %xmm14\n"
    "    pxor  %xmm15, %xmm15\n"
#else
    "    fxrstor64 (%rsp)\n"
#endif
    /* test-holes H11, negative controls for `tkconf fp` F3 and F4: after the
     * restore, -DPK_PREEMPT_DROP_FP_HI zeroes only xmm12-15 (F1 stays green:
     * the compiler keeps fp_chunk's doubles lower) and -DPK_PREEMPT_DROP_MXCSR
     * puts back the default MXCSR (0x1f80; offset 24 of the fxsave area) */
#ifdef PK_PREEMPT_DROP_FP_HI
    "    pxor  %xmm12, %xmm12\n"
    "    pxor  %xmm13, %xmm13\n"
    "    pxor  %xmm14, %xmm14\n"
    "    pxor  %xmm15, %xmm15\n"
#endif
#ifdef PK_PREEMPT_DROP_MXCSR
    "    movl  $0x1f80, 24(%rsp)\n"
    "    ldmxcsr 24(%rsp)\n"
#endif
    "    movq  %rbp, %rsp\n"
    "    popq  %rbp\n"
    "    popq %r11\n"
    "    popq %r10\n    popq %r9\n      popq %r8\n      popq %rdi\n"
    "    popq %rsi\n    popq %rdx\n     popq %rcx\n     popq %rax\n"
    "    popfq\n"
    "    movq  $0, 8(%rsp)\n"                          /* busy word = 0 (no flags) */
    "    popq  %rsp\n"                                  /* back on the task stack */
    "    ret   $128\n"          /* pop the RIP, then give back the skipped red zone */
    "arch_preempt_tramp_end:\n"
    "    .size arch_preempt_tramp, . - arch_preempt_tramp\n"
);

extern volatile int pk_parallel_busy;        /* llm/pk_parallel.c: a pool job is out */
extern void (*pk_parallel_on_leave)(void);

/* pk_parallel.c calls this when its last dispatch is joined (task context, at
 * a call boundary): the switch maybe_preempt skipped while the pool was busy
 * happens here. Without it a task calling the pool back to back starved higher
 * tasks (`tkconf pool` P5 = 0, measured 2/2). -DPK_POOL_NO_LEAVE_DISPATCH:
 * skip it (the negative control for P5). */
static void pk_pool_left(void)
{
#ifndef PK_POOL_NO_LEAVE_DISPATCH
    /* the pool used inside a pk_nosw.h region: switching here would switch
     * inside the region; the outermost pk_nosw_leave dispatches instead
     * (no caller does this today — audit-23 grep — so nothing changes now).
     * -DPK_POOL_LEAVE_IGNORES_NOSW: the old way (negative control, N18) */
#ifndef PK_POOL_LEAVE_IGNORES_NOSW
    int id = tk_get_tid();
    if (id > 0 && id < SIDE_SLOTS && nosw_depth[id]) return;
#endif
    if (pk_text_ok && !arch_irq_disabled_flag)
        arch_preempt_dispatch();
#endif
}

/* pk_nosw.h. Outside a task (id 0, or before the kernel runs) both are no-ops. */
void pk_nosw_enter(void)
{
    int id = tk_get_tid();
    if (id > 0 && id < SIDE_SLOTS) nosw_depth[id]++;
}

void pk_nosw_leave(void)
{
    int id = tk_get_tid();
    if (id > 0 && id < SIDE_SLOTS && nosw_depth[id] && --nosw_depth[id] == 0)
        pk_pool_left();
}

static void maybe_preempt(ucontext_t *uc)
{
    if (arch_irq_disabled_flag || knl_taskindp != 0 ||
        knl_dispatch_disabled || knl_ctxtsk == 0 || knl_ctxtsk == knl_schedtsk)
        return;
    if (pk_parallel_busy)
        return;                              /* the shared pool job: next tick */
    greg_t rip = uc->uc_mcontext.gregs[REG_RIP];
    if (!pk_text_ok || !in_pk_text((uintptr_t)rip))
        return;                              /* in libc etc.: not here */
    if (rip >= (greg_t)(uintptr_t)arch_preempt_tramp &&
        rip <  (greg_t)(uintptr_t)arch_preempt_tramp_end)
        return;                              /* moving on/off the side stack */
    int id = tk_get_tid();
    if (id <= 0 || id >= SIDE_SLOTS) return;
    if (nosw_depth[id]) return;              /* pk_nosw.h region: on leaving */
    uint64_t *top = &side_stack[id][SIDE_SZ / 8];
    if (top[-1]) return;                     /* this task is already inside */
    top[-1] = 1;
    arch_preempt_side_sp = (uint64_t)(uintptr_t)&top[-1];
    greg_t rsp = uc->uc_mcontext.gregs[REG_RSP] - 128 - 8;   /* skip the red zone */
    *(greg_t *)(uintptr_t)rsp = rip;
    uc->uc_mcontext.gregs[REG_RSP] = rsp;
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)arch_preempt_tramp;
}
#else
void arch_preempt_forget(int tskid) { (void)tskid; }
void pk_nosw_enter(void) { }
void pk_nosw_leave(void) { }
#endif

static void sigalrm_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig; (void)si; (void)ucv;
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
#ifndef PK_HOSTED_NO_EXIT_DISPATCH
    maybe_preempt((ucontext_t *)ucv);
#endif
}

#define SIGSTACK_SZ (64 * 1024)
static unsigned char signal_stack[SIGSTACK_SZ] __attribute__((aligned(16)));

void arch_signals_init(void)
{
    stack_t ss = {
        .ss_sp = signal_stack, .ss_size = SIGSTACK_SZ, .ss_flags = 0,
    };
    if (sigaltstack(&ss, NULL) < 0) { perror("sigaltstack"); exit(1); }
#ifndef PK_HOSTED_NO_EXIT_DISPATCH
    pk_text_check();
    if (pk_text_ok) pk_parallel_on_leave = pk_pool_left;
#endif

    struct sigaction sa = { 0 };
    sa.sa_sigaction = sigalrm_handler;
    sa.sa_flags = SA_ONSTACK | SA_RESTART | SA_SIGINFO;
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
