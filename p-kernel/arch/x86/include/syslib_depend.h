/*
 *  syslib_depend.h (x86)
 *  x86 interrupt control macros for T-Kernel syslib
 */

#ifndef __TK_SYSLIB_DEPEND_H__
#define __TK_SYSLIB_DEPEND_H__

#include <errno.h>
#include <sysdef.h>
#include "sysdef_depend.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Interrupt enable/disable (inline, defined in cpu_insn.h)
 * disint() saves EFLAGS and clears IF.
 * enaint() restores IF from saved EFLAGS.
 * isDI()   checks if IF was clear in saved value.
 */
static inline unsigned int _x86_disint(void)
{
    unsigned int eflags;
    __asm__ volatile (
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(eflags) : : "memory"
    );
    return eflags;
}

static inline unsigned int _x86_enaint(unsigned int eflags)
{
    if (eflags & EFLAGS_IF) {
        __asm__ volatile ("sti" : : : "memory");
    }
    return eflags;
}

#define DI(intsts)      ( (intsts) = _x86_disint() )
#define EI(intsts)      ( _x86_enaint(intsts) )
#define isDI(intsts)    ( ((intsts) & EFLAGS_IF) == 0 )

/* Convert to interrupt definition number */
#define DINTNO(intvec)  (intvec)

#define INTLEVEL_DI     (0)
#define INTLEVEL_EI     (1)

/* Set/Get interrupt mask (simplified - x86 only has global enable/disable) */
#define SetIntLevel(level)  do { if (level) { __asm__ volatile("sti"); } else { __asm__ volatile("cli"); } } while(0)
#define GetIntLevel()       (_x86_disint())

#ifdef __cplusplus
}
#endif

#endif /* __TK_SYSLIB_DEPEND_H__ */
