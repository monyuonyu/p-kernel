/*
 *  sysdef_depend.h (x86)
 *  x86 EFLAGS / interrupt vector definitions
 */

#ifndef __TK_SYSDEF_DEPEND_H__
#define __TK_SYSDEF_DEPEND_H__

/* EFLAGS bits */
#define EFLAGS_CF       0x00000001
#define EFLAGS_IF       0x00000200  /* Interrupt enable flag */
#define EFLAGS_IOPL     0x00003000
#define EFLAGS_VM       0x00020000

/* x86 exception vectors */
#define EXP_DE          0   /* Divide Error */
#define EXP_DB          1   /* Debug */
#define EXP_NMI         2   /* NMI */
#define EXP_BP          3   /* Breakpoint */
#define EXP_OF          4   /* Overflow */
#define EXP_BR          5   /* Bound Range Exceeded */
#define EXP_UD          6   /* Invalid Opcode */
#define EXP_NM          7   /* Device Not Available */
#define EXP_DF          8   /* Double Fault */
#define EXP_TS          10  /* Invalid TSS */
#define EXP_NP          11  /* Segment Not Present */
#define EXP_SS          12  /* Stack-Segment Fault */
#define EXP_GP          13  /* General Protection */
#define EXP_PF          14  /* Page Fault */

/* IRQ base vector (after PIC remap) */
#define IRQ_VECTOR_BASE 32
#define INTNO_TIMER     (IRQ_VECTOR_BASE + 0)   /* PIT IRQ0 */
#define INTNO_KBD       (IRQ_VECTOR_BASE + 1)   /* Keyboard IRQ1 */

/* System call vector (software int) */
#define INTNO_SYSCALL   0x80

#endif /* __TK_SYSDEF_DEPEND_H__ */
