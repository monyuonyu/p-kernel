/*
 *  syslib_depend.h (aarch64)
 *  Interrupt control wrappers — thin delegation to cpu_insn.h functions
 */

#ifndef __TK_SYSLIB_DEPEND_H__
#define __TK_SYSLIB_DEPEND_H__

#include <errno.h>
#include <sysdef.h>
#include "sysdef_depend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Interrupt disable/enable wrappers (thin; actual code in cpu_insn.h) */
static inline unsigned int _aa64_disint(void)
{
    unsigned int daif;
    __asm__ volatile (
        "mrs %0, daif\n\t"
        "msr daifset, #0x3"
        : "=r"(daif) : : "memory"
    );
    return daif;
}

static inline void _aa64_enaint(unsigned int daif)
{
    if (!(daif & (1U << 7))) {
        __asm__ volatile ("msr daifclr, #0x3" : : : "memory");
    }
}

#define DI(imask)       imask = _aa64_disint()
#define EI(imask)       _aa64_enaint(imask)
#define isDI(imask)     (((imask) & (1U << 7)) != 0)

#ifdef __cplusplus
}
#endif

#endif /* __TK_SYSLIB_DEPEND_H__ */
