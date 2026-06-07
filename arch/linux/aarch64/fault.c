/*
 *  arch/linux/aarch64/fault.c
 *  Synchronous fault capture for task-level fault isolation (wave 7).
 *
 *  SIGSEGV / SIGBUS / SIGFPE handlers running on the sigaltstack that
 *  arch_signals_init() installed (so a smashed task stack cannot take
 *  the handler down too). On a fault in a GUARDED task we do not let
 *  the process die: guard_fault_isolate() (arch/common/guard.c) hands
 *  back an emergency stack, and we rewrite the interrupted context so
 *  sigreturn resumes the task inside guard_task_killer(), which
 *  tk_exd_tsk()s it. Kernel process survives; only the task dies.
 *
 *  REGISTER-PATCHING RULE (learned the hard way on x86_64): never
 *  memcpy a whole mcontext_t — on x86_64 it contains an fpregs
 *  POINTER that goes stale at sigreturn. Patch ONLY the named
 *  registers you mean to change. The aarch64 mcontext is flat, but we
 *  follow the same discipline: pc, sp, x29 (fp), x30 (lr) only.
 *
 *  Like preempt.c, this file includes no T-Kernel headers — it talks
 *  to the guard layer through two plain-C-typed hooks.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

/* preempt.c — set inside DISABLE_INTERRUPT windows. A fault taken
 * there means kernel state is mid-mutation: not recoverable. */
extern volatile int arch_irq_disabled_flag;

/* arch/common/guard.c hooks (see guard.h) */
extern void *guard_fault_isolate(int sig, unsigned long pc,
                                 unsigned long addr);
extern void guard_task_killer(void);

static void fault_handler(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t   *uc   = (ucontext_t *)ucv;
    unsigned long pc   = (unsigned long)uc->uc_mcontext.pc;
    unsigned long addr = (unsigned long)si->si_addr;

    void *sp = NULL;
    if (!arch_irq_disabled_flag)
        sp = guard_fault_isolate(sig, pc, addr);

    if (sp == NULL) {
        /* Unguarded task / kernel-critical fault: restore the default
         * action and return. The faulting instruction re-executes,
         * re-faults, and the default action (core dump) fires — an
         * honest crash with the original fault PC intact. */
        signal(sig, SIG_DFL);
        return;
    }

    /* Patch ONLY named registers (see header comment). Land in
     * guard_task_killer on the emergency stack; zero fp/lr so a
     * backtrace from the killer terminates cleanly instead of
     * walking the dead task's (possibly corrupt) frames. */
    uc->uc_mcontext.pc       = (unsigned long)guard_task_killer;
    uc->uc_mcontext.sp       = (unsigned long)sp;
    uc->uc_mcontext.regs[29] = 0;   /* fp */
    uc->uc_mcontext.regs[30] = 0;   /* lr */
}

/* Called from tkdev_init.c right after arch_signals_init(), which
 * installed the sigaltstack these handlers depend on. */
void arch_fault_init(void)
{
    struct sigaction sa;
    __builtin_memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGALRM);   /* no timer ticks mid-rewrite */
    if (sigaction(SIGSEGV, &sa, NULL) < 0 ||
        sigaction(SIGBUS,  &sa, NULL) < 0 ||
        sigaction(SIGFPE,  &sa, NULL) < 0) {
        perror("sigaction(fault)");
        exit(1);
    }
}
