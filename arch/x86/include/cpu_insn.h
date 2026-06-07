/*
 *  cpu_insn.h (x86)
 *  x86 CPU instruction wrappers: interrupt control, I/O, etc.
 */

#ifndef _CPU_INSN_
#define _CPU_INSN_

#include <sysinfo.h>
#include "sysdef_depend.h"

/* -----------------------------------------------------------------------
 *  Interrupt enable / disable
 *  disint() returns current EFLAGS (to save IF state)
 *  enaint() restores EFLAGS IF from saved value
 * --------------------------------------------------------------------- */

static inline UINT disint(void)
{
    UINT eflags;
    __asm__ volatile (
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(eflags) : : "memory"
    );
    return eflags;
}

static inline void enaint(UINT eflags)
{
    if (eflags & EFLAGS_IF) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

/* TRUE if interrupts were disabled in saved eflags */
#define isDI(eflags)    (((eflags) & EFLAGS_IF) == 0)

/* -----------------------------------------------------------------------
 *  I/O port access
 * --------------------------------------------------------------------- */

static inline void outb(UH port, UB val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline UB inb(UH port)
{
    UB ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* -----------------------------------------------------------------------
 *  Interrupt vector table
 *  knl_intvec[] is populated by idt.c (existing boot/x86 code).
 *  T-Kernel uses it via knl_define_inthdr / knl_hll_inthdr.
 * --------------------------------------------------------------------- */

/* Forward declarations - actual arrays are in cpu_init.c */
IMPORT FP knl_intvec[256];

static inline void knl_define_inthdr(INT vecno, FP inthdr)
{
    if ((UINT)vecno < 256) {
        knl_intvec[vecno] = inthdr;
    }
}

/* -----------------------------------------------------------------------
 *  Task independent part counter
 * --------------------------------------------------------------------- */

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

/* -----------------------------------------------------------------------
 *  BASEPRI equivalent: return current EFLAGS
 * --------------------------------------------------------------------- */

static inline UINT knl_getBASEPRI(void)
{
    UINT eflags;
    __asm__ volatile (
        "pushfl\n\t"
        "popl %0"
        : "=r"(eflags)
    );
    return eflags;
}

#endif /* _CPU_INSN_ */
