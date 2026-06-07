/*
 *  arch/linux/x86_64/include/sysdef_depend.h
 *
 *  System dependencies for hosted x86_64-linux.
 *
 *  On bare-metal aarch64 this header defines DAIF bits and the EL1
 *  physical timer PPI (INTNO_TIMER=30). x86_64-linux has neither —
 *  DI/EI route through arch_irq_disabled_flag and the timer is just
 *  SIGALRM. All we need is a knl_intvec slot id for the timer.
 */

#ifndef __TK_SYSDEF_DEPEND_H__
#define __TK_SYSDEF_DEPEND_H__

/* Arbitrary slot in knl_intvec[N_INTVEC]; the SIGALRM handler dispatches
 * via this slot. Value matches the aarch64 sibling for cross-arch
 * consistency, but the number itself has no hardware meaning here. */
#define INTNO_TIMER         30

#endif /* __TK_SYSDEF_DEPEND_H__ */
