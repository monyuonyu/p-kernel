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
 *	@(#)machine_depend.h (FM3)
 *
 *	Machine type definition (system dependent)
 */

#ifndef __SYS_MACHINE_DEPEND_H__
#define __SYS_MACHINE_DEPEND_H__

/*
 * CPU_xxxx		CPU type
 * ALLOW_MISALIGN	1 if access to misalignment data is allowed
 * BIGENDIAN		1 if big endian
 * VIRTUAL_ADDRESS	1 if virtual memory
 * ALLOCA_NOSPT		1 if alloca() is not supported
 */

/* ----- FM3 (ARM Cortex-M3) definition ----- */
//#undef _APP_MB9AF312K_

//#define	_APP_MB9AF312K_		1

//#define CPU_ARMV7_M		1
//#define CPU_ARM_CORTEX_M3	1
//#define CPU_MB9AF		1
#define _APP_RL78G13_R5F100ADASP_		1
#define ALLOW_MISALIGN		0
#define BIGENDIAN		0
#define VIRTUAL_ADDRESS		0
//#define ALLOCA_NOSPT		0
#define INT_BITWIDTH		32

//#ifndef _Csym
// #define _Csym			1
//#endif

#endif /* __SYS_MACHINE_DEPEND_H__ */
