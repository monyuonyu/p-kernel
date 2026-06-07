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
 *	@(#)profile_depend.h (sys/sysdepend/app_mb9af312k)
 *
 *	System common information  
 */

#ifndef __SYS_CORE_PROFILE_DEPEND_H__
#define __SYS_CORE_PROFILE_DEPEND_H__

#include <machine.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif


/*
 * T-Kernel family
 * 	TK_STDTK:   Standard T-Kernel	(always FALSE)
 * 	TK_MICROTK: micro T-Kernel	(must be TRUE)
 */
#define TK_STDTK	FALSE
#define TK_MICROTK	TRUE

/*
 * Memory misalign access is permitted
 */
#define TK_ALLOW_MISALIGN	(ALLOW_MISALIGN)
/*
 * Is Big Endian (Must be defined)
 */
#define TK_BIGENDIAN		(BIGENDIAN)
/*
 * Use virtual address(physical memory != logical memory)
 */
#define TK_VIRTUAL_ADDRESS	(VIRTUAL_ADDRESS)
/*
 * Has virtual memory (non-resident memory exists.)
 */
#define TK_VIRTUAL_MEMORY	FALSE
/*
 * Use CPU Trap instruction for system call entry
 */
#define TK_TRAP_SVC		(USE_TRAP)
/*
 * Support of 64-bit data types (D, UD, VD)
 */
#define TK_HAS_DOUBLEWORD	FALSE
/*
 * Support of sub error code
 */
#define TK_SUPPORT_SERCD	FALSE
/*
 * Task has a separate system stack.
 */
#define TK_HAS_SYSSTACK		FALSE
/*
 * Support of FPU
 */
#define TK_SUPPORT_FPU		FALSE
/*
 * Support of co-processors
 */
#define TK_SUPPORT_COP0		FALSE  
#define TK_SUPPORT_COP1		FALSE
#define TK_SUPPORT_COP2		FALSE
#define TK_SUPPORT_COP3		FALSE
/*
 * Support of resource group
 */
#define TK_SUPPORT_RESOURCE	FALSE
/*
 * Support of user-specified buffer (TA_USERBUF)
 */
#define TK_SUPPORT_USERBUF	TRUE
/*
 * Support of automatic buffer allocation (No TA_USERBUF specification)
 */
#define TK_SUPPORT_AUTOBUF	TRUE
/*
 * Support of setting task slice time (tk_chg_slt)
 */
#define TK_SUPPORT_SLICETIME	FALSE
/*
 * Support of getting task statistics (tk_inf_tsk)
 */
#define TK_SUPPORT_TASKINF	FALSE
/*
 * Support of microsecond
 */
#define TK_SUPPORT_USEC		FALSE
/*
 * Support of task space
 */
#define TK_SUPPORT_TASKSPACE	FALSE
/*
 * Support of task event
 */
#define TK_SUPPORT_TASKEVENT	FALSE
/*
 * Support of disabling wait.
 */
#define TK_SUPPORT_DISWAI	FALSE
/*
 * Support of get/set register operation
 */
#define TK_SUPPORT_REGOPS	TRUE
/*
 * Support of assembly language function entry/exit
 */
#define TK_SUPPORT_ASM		FALSE
/*
 * Support of DS object names
 */
#define TK_SUPPORT_DSNAME	(USE_OBJECT_NAME)	
/*
 * Support of task exception
 */
#define TK_SUPPORT_TASKEXCEPTION	FALSE	

/*
 * Support of power management
 */
#define TK_SUPPORT_LOWPOWER	FALSE
/*
 * Support of subsystem event processing
 */
#define TK_SUPPORT_SSYEVENT	FALSE
/*
 * Support of Large Mass-storage Device (64-bit)
 */
#define TK_SUPPORT_LARGEDEV	FALSE
/*
 * Support of interrupt controller management.
 */
#define TK_SUPPORT_INTCTRL	TRUE
/*
 * Can specify interrupt priority level
 */
#define TK_HAS_ENAINTLEVEL	TRUE
/*
 * Support of get/set of CPU interrupt mask level
 */
#define TK_SUPPORT_CPUINTLEVEL	TRUE
/*
 * Support of get/set of interrupt controller interrupt mask level
 */
#define TK_SUPPORT_CTRLINTLEVEL	FALSE
/*
 * Supoprt of interrupt mode setting
 */
#define TK_SUPPORT_INTMODE	FALSE
/*
 * Support of getting system configuration information
 */
#define TK_SUPPORT_SYSCONF	FALSE
/*
 * Support of I/O port access
 */
#define TK_SUPPORT_IOPORT	TRUE
/*
 * Support of micro wait
 */
#define TK_SUPPORT_MICROWAIT	FALSE
/*
 * support of cache control
 */
#define TK_SUPPORT_CACHECTRL	FALSE
/*
 * Support of write-back cache
 */
#define TK_SUPPORT_WBCACHE	FALSE
/*
 * Support of write-through cache
 */
#define TK_SUPPORT_WTCACHE	FALSE
/*
 * Support of system memory allocation
 */
#define TK_SUPPORT_SYSMEMBLK	FALSE
/*
 * Support of memory allocation library
 */
#define TK_SUPPORT_MEMLIB	FALSE
/*
 * Support of address space management
 */
#define TK_SUPPORT_ADDRSPACE	FALSE
/*
 * Support of physical timer
 */
#define TK_SUPPORT_PTIMER	FALSE
/*
 * Support of T-Kernel/DS
 */
#define TK_SUPPORT_DBGSPT	(USE_DBGSPT)
/*
 * Version number of micro T-Kernel
 *	TK_SPECVER_MAJOR: Major Version number of micro T-Kernel
 *	TK_SPECVER_MINOR: micro T-Kernel minor version number
 */
#define TK_SPECVER_MAJOR	(CFN_VER_MAJOR)
#define TK_SPECVER_MINOR	(CFN_VER_MINOR)
#define TK_SPECVER		((TK_SPECVER_MAJOR << 8) | TK_SPECVER_MINOR)
/*
 * Maximum task priority (>= 16)
 */
#define TK_MAX_TSKPRI		(MAX_PRI)  
/*
 * Maximum queuing number of the task wakeup requests. (>= 1)
 */
#define TK_WAKEUP_MAXCNT	(+2147483647L)
/*
 * Maximum semaphore count (>= 32767)
 */
#define TK_SEMAPHORE_MAXCNT	(+2147483647L)
/*
 * Maximum queuing count of the forced wait of tasks.
 */
#define TK_SUSPEND_MAXCNT	(+2147483647L)
/*
 * Real memory protection level of TA_RNGn (0..3)
 */
#define TK_MEM_RNG0		0
#define TK_MEM_RNG1		0
#define TK_MEM_RNG2		0
#define TK_MEM_RNG3		0
/*
 * Maximum number of physical timers. (>= 0)
 */
#define TK_MAX_PTIMER		0


#ifdef __cplusplus
}
#endif
#endif /* __SYS_CORE_PROFILE_DEPEND_H__ */
