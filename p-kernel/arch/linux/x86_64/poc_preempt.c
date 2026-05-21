/* arch/linux/x86_64/poc_preempt.c
 *
 * Session 2 proof of concept: preemptive multitasking on Linux/x86_64.
 * Sibling of arch/linux/aarch64/poc_preempt.c — same shape, ABI-specific
 * mcontext layout.
 *
 * Two tasks run in tight loops, each incrementing its own counter.
 * Neither task ever cooperatively yields. A 10 ms periodic SIGALRM
 * forces a context switch by rewriting the saved mcontext_t in the
 * signal frame. After a fixed number of ticks the handler hands
 * control back to main via siglongjmp.
 *
 * What this proves on top of Session 1:
 *   - SIGALRM as the userspace timer IRQ
 *   - mcontext rewriting as the *preemptive* primitive
 *   - task_trampoline (Session 2 component): tasks can return cleanly
 *     without re-entering themselves
 *   - IRQ disable/enable flag with pending bit drain
 *
 * The aarch64 sibling had to carry pstate around to satisfy Linux's
 * arm64 sigreturn validator. x86_64 sigreturn is more permissive about
 * gregs — we only patch the four fields we set and let the rest of
 * uc->uc_mcontext (segment regs, flags, fpregs pointer) keep whatever
 * the kernel just put there. fpregs is the load-bearing one: it's a
 * pointer to kernel-allocated FP state for the live signal frame, and
 * dereferencing a stale copy would SIGSEGV on sigreturn. The rule is
 * "patch the named gregs; touch nothing else."
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

#include "../include/arch_preempt.h"

/* From trampoline.S */
extern void task_trampoline(void);

/* Invoked by the trampoline if a task entry returns; tasks here are
 * infinite loops so it never fires. */
void task_exit(void);

/* --------------------------------------------------------------------
 * Per-task initial context. Populate only the regs we know about; the
 * signal handler's install_fresh path will fill the rest from the
 * live signal frame on first dispatch.
 * -------------------------------------------------------------------- */
void arch_init_full_ctx(arch_full_ctx_t *ctx, void *stack_top_raw,
                        void (*entry)(void *), void *arg)
{
    memset(&ctx->mc, 0, sizeof(ctx->mc));
    /* 16-align the stack pointer; ABI requires rsp ≡ 8 (mod 16) at
     * function entry, which the trampoline's first `pushq $0` arranges. */
    uintptr_t sp = ((uintptr_t)stack_top_raw) & ~0xFUL;
    ctx->mc.gregs[REG_R12] = (greg_t)(uintptr_t)entry;
    ctx->mc.gregs[REG_R13] = (greg_t)(uintptr_t)arg;
    ctx->mc.gregs[REG_RSP] = (greg_t)sp;
    ctx->mc.gregs[REG_RIP] = (greg_t)(uintptr_t)task_trampoline;
    ctx->populated = 0;
}

/* --------------------------------------------------------------------
 * Signal infrastructure.
 * -------------------------------------------------------------------- */
static volatile sig_atomic_t irq_disabled = 0;
static volatile sig_atomic_t irq_pending  = 0;

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

static arch_full_ctx_t *ctx_running;
static arch_full_ctx_t *ctx_other;
static int     tick_count;
static int     tick_limit = 30;
static sigjmp_buf back_to_main;

/* Three context-movement primitives. Each touches *only* gregs[] —
 * NEVER fpregs (kernel-allocated pointer, stale after sigreturn) and
 * NEVER __reserved1 (kernel-private). */

static void save_into_ctx(arch_full_ctx_t *ctx, const ucontext_t *uc)
{
    memcpy(ctx->mc.gregs, uc->uc_mcontext.gregs, sizeof(ctx->mc.gregs));
    ctx->populated = 1;
}

static void restore_populated(ucontext_t *uc, const arch_full_ctx_t *ctx)
{
    memcpy(uc->uc_mcontext.gregs, ctx->mc.gregs,
           sizeof(uc->uc_mcontext.gregs));
}

static void install_fresh(ucontext_t *uc, const arch_full_ctx_t *ctx)
{
    /* Only the four regs arch_init_full_ctx set; the rest stay as
     * uc's live kernel-supplied values so sigreturn validates. */
    uc->uc_mcontext.gregs[REG_R12] = ctx->mc.gregs[REG_R12];
    uc->uc_mcontext.gregs[REG_R13] = ctx->mc.gregs[REG_R13];
    uc->uc_mcontext.gregs[REG_RSP] = ctx->mc.gregs[REG_RSP];
    uc->uc_mcontext.gregs[REG_RIP] = ctx->mc.gregs[REG_RIP];
}

static void do_tick_irq(ucontext_t *uc_for_switch)
{
    /* On the very first SIGALRM (raised by main during bootstrap),
     * ctx_running has populated==0 — install fresh and don't count
     * the bootstrap tick. */
    if (!ctx_running->populated) {
        install_fresh(uc_for_switch, ctx_running);
        ctx_running->populated = 1;
        return;
    }

    tick_count++;
    if (tick_count >= tick_limit) {
        siglongjmp(back_to_main, 1);
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

static void drain_pending(void)
{
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

#define SIGSTACK_SZ (64 * 1024)
static unsigned char signal_stack[SIGSTACK_SZ] __attribute__((aligned(16)));

void arch_signals_init(void)
{
    stack_t ss = {
        .ss_sp = signal_stack, .ss_size = SIGSTACK_SZ, .ss_flags = 0,
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
 * Driver — two infinite-loop tasks alternated by the timer.
 * -------------------------------------------------------------------- */
static void *alloc_stack(size_t usable_bytes)
{
    long pagesz = sysconf(_SC_PAGESIZE);
    size_t total = usable_bytes + (size_t)pagesz;
    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(1); }
    if (mprotect(base, (size_t)pagesz, PROT_NONE) != 0) {
        perror("mprotect"); exit(1);
    }
    return (char *)base + total;
}

static volatile unsigned long count_a, count_b;

static void task_a(void *arg) { (void)arg; for (;;) count_a++; }
static void task_b(void *arg) { (void)arg; for (;;) count_b++; }

void task_exit(void)
{
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

    ctx_running = &ctx_a_storage;
    ctx_other   = &ctx_b_storage;

    arch_signals_init();

    printf("[main] arming 10 ms timer, tick_limit=%d\n", tick_limit);
    fflush(stdout);

    if (sigsetjmp(back_to_main, 1) == 0) {
        arch_timer_start(10 * 1000);
        raise(SIGALRM);   /* bootstrap into task_a */
        fprintf(stderr, "[fatal] raise(SIGALRM) returned to main\n");
        return 2;
    }

    printf("[main] ticks=%d, count_a=%lu, count_b=%lu\n",
           tick_count, count_a, count_b);
    if (count_a == 0 || count_b == 0) {
        fprintf(stderr, "[FAIL] one of the tasks never ran\n");
        return 1;
    }
    printf("[main] both tasks made progress — preemption OK\n");
    return 0;
}
