/* arch/linux/include/arch_preempt.h
 *
 * Signal-as-IRQ infrastructure for the Linux userspace port.
 *
 * Distinct from arch_ctx.h: that header is for cooperative switches
 * (arch_ctx_switch saves the small callee-saved set). This header is
 * for preemptive switches driven by SIGALRM, which need the full
 * register state because the C compiler had no chance to spill
 * caller-saved registers before the "switch" happened.
 *
 * The full state lives in an mcontext_t — the same struct the kernel
 * hands to a signal handler. Saving and restoring it is just memcpy.
 *
 * Note: mcontext_t is NOT part of the deprecated ucontext_t /
 * swapcontext API. It is the standard POSIX surface for signal
 * handlers to observe and modify saved CPU state.
 */
#ifndef _PKERNEL_ARCH_LINUX_ARCH_PREEMPT_H
#define _PKERNEL_ARCH_LINUX_ARCH_PREEMPT_H

#include <signal.h>
#include <ucontext.h>

typedef struct arch_full_ctx {
    mcontext_t mc;
    int        populated;  /* 0 = freshly init'd, 1 = mc holds live regs */
} arch_full_ctx_t;

/*
 * Prepare ctx so that the first time it is dispatched, the CPU enters
 * task_trampoline with x19=entry, x20=arg, sp=aligned(stack_top),
 * pc=task_trampoline. After this call, ctx is "freshly init'd"
 * (populated=0); the scheduler treats this differently from a context
 * that was preempted while running.
 */
extern void arch_init_full_ctx(arch_full_ctx_t *ctx, void *stack_top,
                               void (*entry)(void *), void *arg);

/*
 * Install SIGALRM handler on a dedicated signal stack. Idempotent;
 * subsequent calls are no-ops. Must be called before arch_timer_start.
 */
extern void arch_signals_init(void);

/*
 * Start the periodic preemption tick. Calling again replaces the
 * period (does not stack timers).
 */
extern void arch_timer_start(unsigned long period_us);

/*
 * Mask / unmask preemption. While masked, SIGALRM ticks are remembered
 * (pending) and drained on the next unmask. Models the bare-metal
 * "interrupts disabled" critical section.
 */
extern void arch_irq_disable(void);
extern void arch_irq_enable(void);

#endif /* _PKERNEL_ARCH_LINUX_ARCH_PREEMPT_H */
