/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 2.0 Software Package
 *
 *    Copyright (C) 2006-2014 by Ken Sakamura.
 *    This software is distributed under the T-License 2.0.
 *----------------------------------------------------------------------
 *
 *    Released by T-Engine Forum(http://www.t-engine.org/) at 2014/09/01.
 *
 *----------------------------------------------------------------------
 */

/*
 *	@(#)sysdef_depend.h (FM3)
 *
 *	Definition about FM3
 *
 *	Included also from assembler program.
 */

#ifndef __TK_SYSDEF_DEPEND_H__
#define __TK_SYSDEF_DEPEND_H__

/*
 * Program status register (PSR)
 */
#define PSR_N		0x80000000	/* Condition flag Negative */
#define PSR_Z		0x40000000	/* Zero */
#define PSR_C		0x20000000	/* Carry */
#define PSR_V		0x10000000	/* Overflow */
#define PSR_Q		0x08000000	/* Saturation */

#define PSR_INT_MSK	0x000000FF	/* Interrupt status mask */

/* Exception model. */
#define EXP_M(n)	(n)		/* Exception model */
#define EXP_USR		EXP_M(0)	/* User mode, No Exception */
#define EXP_RST		EXP_M(1)	/* Reset */
#define EXP_NMI		EXP_M(2)	/* Non Maskable Interrupt */
#define EXP_HDF		EXP_M(3)	/* Hardware fault */
#define EXP_MEM		EXP_M(4)	/* Memory fault */
#define EXP_BUS		EXP_M(5)	/* Bus fault */
#define EXP_USF		EXP_M(6)	/* Usage fault */
#define EXP_SVC		EXP_M(11)	/* SVC call */
#define EXP_DBG		EXP_M(12)	/* Debug monitor */
#define EXP_PSV		EXP_M(14)	/* Software asynchronous System call */
#define EXP_STK		EXP_M(15)	/* System tick */
#define EXP_EXT(n)	(EXP_M(16) + n)	/* External interrupt */


/* ------------------------------------------------------------------------ */

/*
 * Parameter of SVC instruction
 */
#define SVC_SYSCALL		0x00	/* micro T-Kernel system call/extension SVC */
#define SVC_EXTENDED_SVC	0x10	/* Extended SVC */
#define SVC_DEBUG_SUPPORT	0xFF	/* Debugger support function */

/* ------------------------------------------------------------------------ */

/*
 * System control block
 */
#define SCB_ICSR	0xE000ED04
#define SCB_VTOR	0xE000ED08
#define SCB_AIRCR	0xE000ED0C

#define SCB_SCR		0xE000ED10
#define SCB_CCR		0xE000ED14
#define SCB_SHPR1	0xE000ED18
#define SCB_SHPR2	0xE000ED1C
#define SCB_SHPR3	0xE000ED20

#define SCB_STIR	0xE000EF00

#define ICSR_PENDSVSET	0x10000000	/* Trigger PendSV exception. */
#define ICSR_PENDSVCLR	0x08000000	/* Remove the pending state from the PendSV exception. */

/*
 * System Timer
 */
#define SYST_CSR	0xE000E010	/* SysTick Control and Status */
#define SYST_RVR	0xE000E014	/* SysTick Reload value */
#define SYST_CVR	0xE000E018	/* SysTick Current value */

/* ------------------------------------------------------------------------ */

/*
 * Clock Control
 */
/* Registers */
// #define FM3_CRG_BASE		0x40010000
// #define FM3_CRG_SCM_CTL		(*(volatile UW*)(FM3_CRG_BASE + 0x00))
// #define FM3_CRG_SCM_STR		(*(volatile UW*)(FM3_CRG_BASE + 0x04))
// #define FM3_CRG_STB_CTL		(*(volatile UW*)(FM3_CRG_BASE + 0x08))
// #define FM3_CRG_RST_STR		(*(volatile UW*)(FM3_CRG_BASE + 0x0C))
// #define FM3_CRG_BSC_PSR		(*(volatile UW*)(FM3_CRG_BASE + 0x10))
// #define FM3_CRG_APBC0_PSR	(*(volatile UW*)(FM3_CRG_BASE + 0x14))
// #define FM3_CRG_APBC1_PSR	(*(volatile UW*)(FM3_CRG_BASE + 0x18))
// #define FM3_CRG_APBC2_PSR	(*(volatile UW*)(FM3_CRG_BASE + 0x1C))
// #define FM3_CRG_SWC_PSR		(*(volatile UW*)(FM3_CRG_BASE + 0x20))
// #define FM3_CRG_TTC_PSR		(*(volatile UW*)(FM3_CRG_BASE + 0x28))
// #define FM3_CRG_CSW_TMR		(*(volatile UW*)(FM3_CRG_BASE + 0x30))
// #define FM3_CRG_PSW_TMR		(*(volatile UW*)(FM3_CRG_BASE + 0x34))
// #define FM3_CRG_PLL_CTL1	(*(volatile UW*)(FM3_CRG_BASE + 0x38))
// #define FM3_CRG_PLL_CTL2	(*(volatile UW*)(FM3_CRG_BASE + 0x3C))
// #define FM3_CRG_CSV_CTL		(*(volatile UW*)(FM3_CRG_BASE + 0x40))
// #define FM3_CRG_CSV_STR		(*(volatile UW*)(FM3_CRG_BASE + 0x44))
// #define FM3_CRG_FCSWH_CTL	(*(volatile UW*)(FM3_CRG_BASE + 0x48))
// #define FM3_CRG_FCSWL_CTL	(*(volatile UW*)(FM3_CRG_BASE + 0x4C))
// #define FM3_CRG_FCSWD_CTL	(*(volatile UW*)(FM3_CRG_BASE + 0x50))
// #define FM3_CRG_DBWDT_CTL	(*(volatile UW*)(FM3_CRG_BASE + 0x54))
// #define FM3_CRG_INT_ENR		(*(volatile UW*)(FM3_CRG_BASE + 0x60))
// #define FM3_CRG_INT_STR		(*(volatile UW*)(FM3_CRG_BASE + 0x64))
// #define FM3_CRG_INT_CLR		(*(volatile UW*)(FM3_CRG_BASE + 0x68))

#define CLOCK_SETUP	1
#define HWWD_DISABLE	1
#define CR_TRIM_SETUP	1
#define SCM_CTL_Val	0x00000052
#define CSW_TMR_Val	0x0000005C
#define PSW_TMR_Val	0x00000000
#define PLL_CTL1_Val	0x00000001
#define PLL_CTL2_Val	0x00000009
#define BSC_PSR_Val	0x00000000
#define APBC0_PSR_Val	0x00000001
#define APBC1_PSR_Val	0x00000081
#define APBC2_PSR_Val	0x00000081
#define SWC_PSR_Val	0x00000003
#define TTC_PSR_Val	0x00000000

/* ------------------------------------------------------------------------ */

/*
 *  Flash interface
 */
/* Registers */
// #define FM3_FLASH_IF_BASE	0x40000000
// #define FM3_FLASH_IF_FASZR	(*(volatile UW*)(FM3_FLASH_IF_BASE + 0x00))
// #define FM3_FLASH_IF_FRWTR	(*(volatile UW*)(FM3_FLASH_IF_BASE + 0x04))
// #define FM3_FLASH_IF_FSTR	(*(volatile UW*)(FM3_FLASH_IF_BASE + 0x08))
// #define FM3_FLASH_IF_FSYNDN	(*(volatile UW*)(FM3_FLASH_IF_BASE + 0x10))
// #define FM3_FLASH_IF_CRTRMM	(*(volatile UW*)(FM3_FLASH_IF_BASE + 0x100))

/* ------------------------------------------------------------------------ */

/*
 *  Hardware watchdog
 */
/* Registers */
// #define FM3_HWWDT_BASE		0x40011000
// #define FM3_HWWDT_WDG_LDR	(*(volatile UW*)(FM3_HWWDT_BASE + 0x00))
// #define FM3_HWWDT_WDG_VLR	(*(volatile UW*)(FM3_HWWDT_BASE + 0x04))
// #define FM3_HWWDT_WDG_CTL	(*(volatile UW*)(FM3_HWWDT_BASE + 0x08))
// #define FM3_HWWDT_WDG_ICL	(*(volatile UW*)(FM3_HWWDT_BASE + 0x0C))
// #define FM3_HWWDT_WDG_RIS	(*(volatile UW*)(FM3_HWWDT_BASE + 0x10))
// #define FM3_HWWDT_WDG_LCK	(*(volatile UW*)(FM3_HWWDT_BASE + 0xC00))

/* ------------------------------------------------------------------------ */

/*
 *  CR trimming
 */
/* Registers */
// #define FM3_CRTRIM_BASE		0x4002E000
// #define FM3_CRTRIM_MCR_PSR	(*(volatile UW*)(FM3_CRTRIM_BASE + 0x00))
// #define FM3_CRTRIM_MCR_FTRM	(*(volatile UW*)(FM3_CRTRIM_BASE + 0x04))
// #define FM3_CRTRIM_MCR_RLR	(*(volatile UW*)(FM3_CRTRIM_BASE + 0x0C))

/* ------------------------------------------------------------------------ */

/*
 * NVIC (Nested Vectored Interrupt Controller)
 */
#define NVIC_ICTR	0xE000E004

#define NVIC_ISER_BASE	0xE000E100
#define NVIC_ISER(x)	(NVIC_ISER_BASE + (((x) / 32) << 2))

#define NVIC_ICER_BASE	0xE000E180
#define NVIC_ICER(x)	(NVIC_ICER_BASE + (((x) / 32) << 2))

#define NVIC_ISPR_BASE	0xE000E200
#define NVIC_ISPR(x)	(NVIC_ISPR_BASE + (((x) / 32) << 2))

#define NVIC_ICPR_BASE	0xE000E280
#define NVIC_ICPR(x)	(NVIC_ICPR_BASE + (((x) / 32) << 2))

#define NVIC_IABR_BASE	0xE000E300
#define NVIC_IABR(x)	(NVIC_IABR_BASE + (((x) / 32) << 2))

#define NVIC_IPR_BASE	0xE000E400
#define NVIC_IPR(x)	(NVIC_IPR_BASE  + (x))

/* ------------------------------------------------------------------------ */
/*
 * The number of the implemented bit width for priority value fields.
 */
#define INTPRI_BITWIDTH		4

/*
 * force dispatch interrupt for micro T-Kernel
 */
#define INTNO_FORCE_DISPATCH	47
#define NVIC_IPR_FORCE_DISPATCH	NVIC_IPR(INTNO_FORCE_DISPATCH)

/*
 * Highest Ext. interrupt level for disint()
 */
#define MAX_EXTINT_PRI		0x10

/* ------------------------------------------------------------------------ */

#endif /* __TK_SYSDEF_DEPEND_H__ */
