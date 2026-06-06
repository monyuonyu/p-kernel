/*
 *  arch/linux/x86_64/fault.c
 *  Synchronous fault capture for task-level fault isolation (wave 7).
 *  x86_64 sibling of arch/linux/aarch64/fault.c — same contract, see
 *  that file for the full story.
 *
 *  CRITICAL x86_64 TRAP (we hit this before, in the preemption PoC):
 *  mcontext_t here contains `fpregs`, a POINTER into the signal frame
 *  that the kernel revalidates at sigreturn. memcpy'ing a whole
 *  mcontext_t (or otherwise clobbering fpregs) makes sigreturn read
 *  stale FP state or crash. Therefore: patch ONLY the named gregs we
 *  mean to change — RIP, RSP, RBP — and touch nothing else.
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
    unsigned long pc   = (unsigned long)uc->uc_mcontext.gregs[REG_RIP];
    unsigned long addr = (unsigned long)si->si_addr;

    void *sp = NULL;
    if (!arch_irq_disabled_flag)
        sp = guard_fault_isolate(sig, pc, addr);

    if (sp == NULL) {
        /* Unguarded task / kernel-critical fault: restore the default
         * action and return; the re-executed instruction re-faults and
         * the default action (core dump) fires. */
        signal(sig, SIG_DFL);
        return;
    }

    /* Named gregs ONLY (see header comment — fpregs must survive
     * untouched). SysV ABI: at function entry RSP%16 == 8 (as if a
     * call just pushed the return address), so movaps spills in the
     * killer stay aligned — bias the 16-aligned stack top by -8.
     * RBP=0 terminates frame walks cleanly. */
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)guard_task_killer;
    uc->uc_mcontext.gregs[REG_RSP] = (greg_t)((char *)sp - 8);
    uc->uc_mcontext.gregs[REG_RBP] = 0;
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
