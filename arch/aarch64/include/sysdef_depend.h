/*
 *  sysdef_depend.h (aarch64)
 *  DAIF bits, interrupt numbers, exception syndrome constants
 */

#ifndef __TK_SYSDEF_DEPEND_H__
#define __TK_SYSDEF_DEPEND_H__

/* DAIF bits (as they appear in the DAIF system register, bits 9:6) */
#define DAIF_D      (1U << 9)   /* Debug */
#define DAIF_A      (1U << 8)   /* SError */
#define DAIF_I      (1U << 7)   /* IRQ */
#define DAIF_F      (1U << 6)   /* FIQ */

/* GIC interrupt numbers (QEMU virt / RPi3 ARM Generic Timer PPI 30) */
#define INTNO_TIMER         30  /* EL1 physical timer PPI */

/* SVC immediate for system call */
#define INTNO_SYSCALL       0

#endif /* __TK_SYSDEF_DEPEND_H__ */
