/* arch/linux/aarch64/poc_preempt.c
 *
 * Session 2 proof of concept: preemptive multitasking on Linux.
 *
 * Two tasks run in tight loops, each incrementing its own counter.
 * Neither task ever cooperatively yields. A 10 ms periodic SIGALRM
 * forces a context switch by rewriting the saved mcontext_t in the
 * signal frame. After a fixed number of ticks, the handler hands
 * control back to main via siglongjmp.
 *
 * What this proves on top of Session 1:
 *
 *   - SIGALRM as the userspace timer IRQ (component 4).
 *   - mcontext rewriting as the *preemptive* context-switch primitive,
 *     complementary to Session 1's cooperative arch_ctx_switch.
 *   - task_trampoline (component 3): tasks can return cleanly without
 *     re-entering themselves.
 *   - IRQ disable/enable flag with pending bit drain (component 4).
 *
 * Both signal-stack mechanics and mcontext layout are AArch64-Linux
 * specific. The x86_64 sibling lives in arch/linux/x86_64/ when
 * written; the API surface in arch_preempt.h stays identical.
 *
 * For self-containment the preemption infrastructure (init_full_ctx,
 * signal handler, IRQ flags, timer) lives in this PoC file. Session 3
 * will split it out into a permanent preempt.c that the kernel build
 * picks up.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <setjmp.h>
#include <sys/mman.h>

#include "../include/arch_preempt.h"

/* From trampoline.S */
extern void task_trampoline(void);

/* Forward declaration — invoked by the trampoline if a task entry
 * returns. The PoC's task entries are infinite loops, so this never
 * fires here. */
void task_exit(void);

/* --------------------------------------------------------------------
 * Per-task initial context. Populate mc just enough that a sigreturn
 * onto it lands at task_trampoline with x19/x20/sp set up. pstate is
 * patched in by the first SIGALRM (which carries a valid userland
 * pstate value in its own mcontext).
 * -------------------------------------------------------------------- */
void arch_init_full_ctx(arch_full_ctx_t *ctx, void *stack_top_raw,
                        void (*entry)(void *), void *arg)
{
    memset(&ctx->mc, 0, sizeof(ctx->mc));
    ctx->mc.regs[19] = (unsigned long)entry;
    ctx->mc.regs[20] = (unsigned long)arg;
    ctx->mc.regs[30] = 0;            /* lr — set by BLR inside trampoline */
    ctx->mc.sp       = (unsigned long)stack_top_raw & ~0xFUL;
    ctx->mc.pc       = (unsigned long)task_trampoline;
    /* pstate left zero; the signal handler patches it in from the
     * first interrupted context so the kernel will accept it. */
    ctx->populated = 0;
}

/* --------------------------------------------------------------------
 * Signal infrastructure.
 *
 * The handler is the only place where context switching actually
 * happens in this PoC. It runs on a dedicated 64 KB signal stack so
 * a task's own stack is never touched while we manipulate state.
 * -------------------------------------------------------------------- */
static volatile sig_atomic_t irq_disabled = 0;
static volatile sig_atomic_t irq_pending  = 0;
static unsigned long         captured_pstate = 0;

void arch_irq_disable(void) { irq_disabled = 1; }

static void drain_pending(void); /* fwd */

void arch_irq_enable(void)
{
    irq_disabled = 0;
    if (irq_pending) {
        irq_pending = 0;
        drain_pending();
    }
}

/* The two tasks the PoC alternates between, plus the marker for
 * "back to main" via siglongjmp. Provided by the driver code below. */
static arch_full_ctx_t *ctx_running;
static arch_full_ctx_t *ctx_other;
static int     tick_count;
static int     tick_limit = 30;
static sigjmp_buf back_to_main;

/* Three context-movement primitives, factored out because the
 * mcontext_t.__reserved tail has rules that make naive memcpy unsafe
 * for the "fresh task" case. */

static void save_into_ctx(arch_full_ctx_t *ctx, const ucontext_t *uc)
{
    /* Full clone is safe here: uc's mcontext was built by the kernel
     * for this signal delivery, so __reserved is a well-formed list
     * (FPSIMD section + terminator). Round-tripping it into ctx and
     * back later is exactly what the kernel expects. */
    memcpy(&ctx->mc, &uc->uc_mcontext, sizeof(mcontext_t));
    ctx->populated = 1;
}

static void restore_populated(ucontext_t *uc, const arch_full_ctx_t *ctx)
{
    /* Same reasoning in reverse: ctx->mc was captured from a real
     * signal frame, so its __reserved is valid. */
    memcpy(&uc->uc_mcontext, &ctx->mc, sizeof(mcontext_t));
}

static void install_fresh(ucontext_t *uc, const arch_full_ctx_t *ctx)
{
    /* DO NOT memcpy the whole mc here — ctx->mc.__reserved is zero,
     * and Linux's arm64 sigreturn rejects sigframes without a valid
     * FPSIMD context (returns -EINVAL, signal becomes SIGSEGV).
     *
     * Patch only the GP regs / sp / pc we care about, leaving uc's
     * existing __reserved tail in place (it was just built by the
     * kernel for the currently-delivered signal and is therefore
     * well-formed). */
    for (int i = 0; i < 31; i++) {
        uc->uc_mcontext.regs[i] = ctx->mc.regs[i];
    }
    uc->uc_mcontext.sp = ctx->mc.sp;
    uc->uc_mcontext.pc = ctx->mc.pc;
    /* pstate stays as the currently-captured live value. */
}

static void do_tick_irq(ucontext_t *uc_for_switch)
{
    /* First signal — raised by main during bootstrap. Capture pstate
     * for future bookkeeping, install ctx_running using the fresh
     * (partial-patch) path so the kernel's sigreturn finds a valid
     * __reserved tail, and return without bumping tick_count. */
    if (captured_pstate == 0) {
        captured_pstate = uc_for_switch->uc_mcontext.pstate;
        ctx_running->mc.pstate = captured_pstate;
        ctx_other->mc.pstate   = captured_pstate;
        install_fresh(uc_for_switch, ctx_running);
        ctx_running->populated = 1;
        return;
    }

    tick_count++;

    if (tick_count >= tick_limit) {
        siglongjmp(back_to_main, 1);
        /* unreachable */
    }

    save_into_ctx(ctx_running, uc_for_switch);

    /* Round-robin */
    arch_full_ctx_t *next = ctx_other;
    ctx_other   = ctx_running;
    ctx_running = next;

    if (ctx_running->populated) {
        restore_populated(uc_for_switch, ctx_running);
    } else {
        install_fresh(uc_for_switch, ctx_running);
        ctx_running->populated = 1;
    }
}

/* When SIGALRM fires while IRQs are disabled, we can't manipulate the
 * mcontext (the running task is "in kernel"). We remember a pending
 * tick and drain it from arch_irq_enable(). The drain path does NOT
 * have a live uc to rewrite — it just bumps the tick counter; the
 * next un-deferred SIGALRM will do the actual switch. */
static void drain_pending(void)
{
    /* Conservative: count the missed tick toward tick_limit so an
     * "interrupts disabled" hot path can't extend total run forever. */
    tick_count++;
    if (tick_count >= tick_limit) {
        siglongjmp(back_to_main, 1);
    }
}

static void sigalrm_handler(int sig, siginfo_t *si, void *uc_ptr)
{
    (void)sig; (void)si;
    if (irq_disabled) {
        irq_pending = 1;
        return;
    }
    do_tick_irq((ucontext_t *)uc_ptr);
}

/* Signal stack lives in BSS — 64 KB is plenty for the handler frame
 * plus glibc internals. */
#define SIGSTACK_SZ (64 * 1024)
static unsigned char signal_stack[SIGSTACK_SZ] __attribute__((aligned(16)));

void arch_signals_init(void)
{
    stack_t ss = {
        .ss_sp    = signal_stack,
        .ss_size  = SIGSTACK_SZ,
        .ss_flags = 0,
    };
    if (sigaltstack(&ss, NULL) < 0) { perror("sigaltstack"); exit(1); }

    struct sigaction sa = { 0 };
    sa.sa_sigaction = sigalrm_handler;
    sa.sa_flags     = SA_ONSTACK | SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, NULL) < 0) { perror("sigaction"); exit(1); }
}

void arch_timer_start(unsigned long period_us)
{
    static timer_t tid;
    static int     created = 0;
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
        .it_interval = { .tv_sec  = period_us / 1000000,
                         .tv_nsec = (long)(period_us % 1000000) * 1000 },
        .it_value    = { .tv_sec  = period_us / 1000000,
                         .tv_nsec = (long)(period_us % 1000000) * 1000 },
    };
    if (timer_settime(tid, 0, &its, NULL) < 0) {
        perror("timer_settime"); exit(1);
    }
}

/* --------------------------------------------------------------------
 * Driver — same stack allocator as Session 1, two tasks that just
 * bump their own counters forever.
 * -------------------------------------------------------------------- */
static void *alloc_stack(size_t usable_bytes)
{
    long pagesz = sysconf(_SC_PAGESIZE);
    size_t total = usable_bytes + (size_t)pagesz;
    void *base = mmap(NULL, total,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                      -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(1); }
    if (mprotect(base, (size_t)pagesz, PROT_NONE) != 0) {
        perror("mprotect"); exit(1);
    }
    return (char *)base + total;
}

/* Tasks are infinite loops; they get preempted by the timer.
 * Volatile so the compiler can't decide the loop is dead. */
static volatile unsigned long count_a, count_b;

static void task_a(void *arg)
{
    (void)arg;
    for (;;) {
        count_a++;
    }
}

static void task_b(void *arg)
{
    (void)arg;
    for (;;) {
        count_b++;
    }
}

void task_exit(void)
{
    /* Should be unreachable for this PoC. */
    fprintf(stderr, "[fatal] task_exit reached — task entry returned\n");
    abort();
}

static arch_full_ctx_t ctx_a_storage, ctx_b_storage;

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    const size_t stack_size = 16 * 4096;
    void *stack_a = alloc_stack(stack_size);
    void *stack_b = alloc_stack(stack_size);

    arch_init_full_ctx(&ctx_a_storage, stack_a, task_a, NULL);
    arch_init_full_ctx(&ctx_b_storage, stack_b, task_b, NULL);

    /* Round-robin globals: A runs first, B is the other side. */
    ctx_running = &ctx_a_storage;
    ctx_other   = &ctx_b_storage;

    arch_signals_init();

    printf("[main] arming 10 ms timer, tick_limit=%d\n", tick_limit);
    fflush(stdout);

    if (sigsetjmp(back_to_main, 1) == 0) {
        arch_timer_start(10 * 1000);   /* 10 ms */
        /* Kick the scheduler with an immediate SIGALRM. The handler
         * sees captured_pstate == 0 on first entry and installs
         * ctx_a_storage's mcontext, then sigreturn drops us into
         * task_a. From there everything is timer-driven. */
        raise(SIGALRM);
        /* The signal handler does not return normally on the first
         * call (it sigreturns into task_a). If it ever does, that's
         * a bug. */
        fprintf(stderr, "[fatal] raise(SIGALRM) returned to main\n");
        return 2;
    }

    /* siglongjmp landed us here after tick_limit ticks. */
    printf("[main] ticks=%d, count_a=%lu, count_b=%lu\n",
           tick_count, count_a, count_b);
    if (count_a == 0 || count_b == 0) {
        fprintf(stderr, "[FAIL] one of the tasks never ran\n");
        return 1;
    }
    printf("[main] both tasks made progress — preemption OK\n");
    return 0;
}
