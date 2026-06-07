/*
 *  arch/linux/x86_64/include/syslib_depend.h
 *
 *  Linux userspace flavour: DI / EI / isDI route to the flag-based
 *  disint / enaint defined in our cpu_insn.h. The bare-metal aarch64
 *  sibling inlines `msr daifset/clr` (EL1 privileged); x86_64 hosted
 *  has no privileged-instruction analogue to worry about, but the
 *  same flag-based model keeps the two ports behaviourally aligned.
 */

#ifndef __TK_SYSLIB_DEPEND_H__
#define __TK_SYSLIB_DEPEND_H__

#include <errno.h>
#include <sysdef.h>
#include "sysdef_depend.h"

#ifdef __cplusplus
extern "C" {
#endif

IMPORT volatile int arch_irq_disabled_flag;
IMPORT void arch_irq_enable_with_drain(void);

static inline unsigned int _linux_disint(void)
{
    unsigned int prev = (unsigned int)arch_irq_disabled_flag;
    arch_irq_disabled_flag = 1;
    return prev;
}

static inline void _linux_enaint(unsigned int prev)
{
    if (!prev) {
        arch_irq_enable_with_drain();
    }
}

#define DI(imask)       (imask = _linux_disint())
#define EI(imask)       _linux_enaint(imask)
#define isDI(imask)     ((imask) != 0)

#ifdef __cplusplus
}
#endif

#endif /* __TK_SYSLIB_DEPEND_H__ */
