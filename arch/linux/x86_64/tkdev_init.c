/*
 *  arch/linux/x86_64/tkdev_init.c
 *  Device init for the Linux userspace port (x86_64 sibling).
 *
 *  Bare-metal counterparts of this file set up GIC, timer hardware,
 *  PCI etc. On Linux all of that collapses into "install the SIGALRM
 *  handler and arm a periodic timer."
 */

#include "kernel.h"
#include "cpu_insn.h"
#include "tkdev_conf.h"

IMPORT void arch_signals_init(void);
IMPORT void arch_fault_init(void);
IMPORT void arch_timer_start(unsigned long period_us);
IMPORT void knl_timer_handler_startup(void);

/* T-Kernel calls this once during knl_t_kernel_main() to bring up
 * board devices. The bare-metal name is knl_tkdev_initialize. */
EXPORT ER knl_tkdev_initialize(void)
{
    arch_signals_init();

    /* SIGSEGV/SIGBUS/SIGFPE capture for task fault isolation (fault.c).
     * Must follow arch_signals_init: it relies on that sigaltstack. */
    arch_fault_init();

    /* Hook the timer-tick vector so knl_define_inthdr-style callers
     * see something registered. Real preemption goes through the
     * SIGALRM handler in preempt.c which calls knl_timer_handler_startup
     * directly — this vector slot is for code that explicitly looks
     * it up. */
    knl_intvec[INTNO_TIMER] = (FP)knl_timer_handler_startup;

    /* 10 ms tick == 100 Hz, matching the historical T-Kernel HZ. The
     * SIGALRM handler in preempt.c calls knl_timer_handler_startup
     * which in turn drives knl_timer_handler. Until the LP64 allocator
     * pointer-truncation bug (memory.h's setAreaFlag etc.) is fixed,
     * the first task never gets created and the timer's effect is
     * limited to bumping pending_timer_ticks. */
    arch_timer_start(10 * 1000);
    return E_OK;
}

#if USE_CLEANUP
EXPORT void knl_tkdev_shutdown(void)
{
}
#endif
