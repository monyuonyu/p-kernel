/*
 *  arch/linux/x86_64/include/cpu_insn.h
 *  Interrupt control / task-independent helpers — userspace x86_64.
 *
 *  Shadows arch/x86/include/cpu_insn.h. The bare-metal i386 sibling
 *  uses cli/sti and IRQL machinery; here those collapse to a single
 *  word toggled by the SIGALRM handler in preempt.c.
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
    /* Only release if the caller's saved state had IRQs enabled, so
     * nested DI/EI pairs balance correctly. */
    if (!prev) {
        arch_irq_enable_with_drain();
    }
}

/* T-Kernel's syslib_depend.h may supply an isDI macro from the bare-
 * metal x86 header set; redefine to use the flag-based path. */
#undef  isDI
#define isDI(saved)  ((saved) != 0)

/* x86_64 memory barriers — unprivileged, safe from userspace. mfence
 * is the full barrier; the aarch64 sibling uses dsb/isb for the same
 * effect. */
#define DSB()   __asm__ volatile ("mfence" ::: "memory")
#define ISB()   __asm__ volatile ("mfence" ::: "memory")

static inline UINT knl_getBASEPRI(void)
{
    return (UINT)arch_irq_disabled_flag;
}

/* -----------------------------------------------------------------------
 *  Interrupt vector table — same semantics as the aarch64 port.
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
