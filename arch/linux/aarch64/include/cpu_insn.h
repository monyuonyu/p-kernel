/*
 *  arch/linux/aarch64/include/cpu_insn.h
 *  Interrupt control / task-independent helpers — userspace flavour.
 *
 *  Shadows arch/aarch64/include/cpu_insn.h. The DAIF-touching inlines
 *  there are EL1-privileged; here they map to the flag manipulated by
 *  the SIGALRM handler in preempt.c.
 */

#ifndef _CPU_INSN_
#define _CPU_INSN_

#include <sysinfo.h>
#include "sysdef_depend.h"
#include "tkdev_conf.h"

IMPORT volatile int arch_irq_disabled_flag;
IMPORT void arch_irq_enable_with_drain(void);

static inline UINT disint(void)
{
    UINT prev = (UINT)arch_irq_disabled_flag;
    arch_irq_disabled_flag = 1;
    return prev;
}

static inline void enaint(UINT prev)
{
    /* Only release if the caller's saved state had IRQs enabled. This
     * keeps nested DI/EI pairs balanced — a routine that disables IRQs
     * does not accidentally enable them on inner exit. */
    if (!prev) {
        arch_irq_enable_with_drain();
    }
}

/* T-Kernel's syslib_depend.h supplies a DAIF-based isDI macro; we
 * undefine and re-supply so the flag-based version wins. */
#undef  isDI
#define isDI(saved)  ((saved) != 0)

/* Memory barriers — identical to the bare-metal AArch64 port. These
 * are unprivileged AArch64 instructions, safe to emit from EL0. */
#define DSB()   __asm__ volatile ("dsb sy" ::: "memory")
#define ISB()   __asm__ volatile ("isb"    ::: "memory")

static inline UINT knl_getBASEPRI(void)
{
    return (UINT)arch_irq_disabled_flag;
}

/* -----------------------------------------------------------------------
 *  Interrupt vector table — same semantics as the AArch64 port.
 *  Hooked from the SIGALRM dispatcher in preempt.c (specifically, the
 *  timer-tick path calls knl_intvec[INTNO_TIMER]).
 * --------------------------------------------------------------------- */

IMPORT FP knl_intvec[N_INTVEC];

static inline void knl_define_inthdr(INT vecno, FP inthdr)
{
    if ((UINT)vecno < N_INTVEC) {
        knl_intvec[vecno] = inthdr;
    }
}

IMPORT W knl_taskindp;

static inline BOOL knl_isTaskIndependent(void)
{
    return (knl_taskindp > 0) ? TRUE : FALSE;
}

static inline void knl_EnterTaskIndependent(void)
{
    knl_taskindp++;
}

static inline void knl_LeaveTaskIndependent(void)
{
    knl_taskindp--;
}

#endif /* _CPU_INSN_ */
