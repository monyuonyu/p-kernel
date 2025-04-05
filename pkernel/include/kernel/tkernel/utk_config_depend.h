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
 *	utk_config_depend.h (MB9AF312K)
 *	System Configuration Definition
 */

/* RAMINFO */
#define SYSTEMAREA_TOP		0x1FFFE000	/* RAM system area top */
#define SYSTEMAREA_END		0x20002000	/* RAM system area end */

/* User definition */
#define RI_USERAREA_TOP		0x1FFFE000	/* RAM user area top */
#define RI_USERINIT		NULL		/* User initialization program */


/* SYSCONF */
#define CFN_TIMER_PERIOD	10
#define CFN_MAX_TSKID		32
#define CFN_MAX_SEMID		16
#define CFN_MAX_FLGID		16
#define CFN_MAX_MBXID		8
#define CFN_MAX_MTXID		2
#define CFN_MAX_MBFID		8
#define CFN_MAX_PORID		4
#define CFN_MAX_MPLID		2
#define CFN_MAX_MPFID		8
#define CFN_MAX_CYCID		4
#define CFN_MAX_ALMID		8
#define CFN_MAX_SSYID		4
#define CFN_MAX_SSYPRI		16

#define CFN_MAX_REGDEV		(8)
#define CFN_MAX_OPNDEV		(16)
#define CFN_MAX_REQDEV		(16)
#define CFN_DEVT_MBFSZ0		(-1)
#define CFN_DEVT_MBFSZ1		(-1)

#define CFN_VER_MAKER		0x011C
#define CFN_VER_PRID		0
#define CFN_VER_SPVER		0x6101
#define CFN_VER_PRVER		0x0101
#define CFN_VER_PRNO1		0
#define CFN_VER_PRNO2		0
#define CFN_VER_PRNO3		0
#define CFN_VER_PRNO4		0

#define CFN_REALMEMEND		((void *)0x20002000)

/*
 * Use zero-clear bss section
 */
#define USE_NOINIT		(0)

/*
 * Stack size for each mode
 */
#define EXC_STACK_SIZE		0x200
#define TMP_STACK_SIZE		0x80
#define USR_STACK_SIZE		0		/* not used */

#define EXCEPTION_STACK_TOP	SYSTEMAREA_END
#define TMP_STACK_TOP		(EXCEPTION_STACK_TOP - EXC_STACK_SIZE)
#define APPLICATION_STACK_TOP	(TMP_STACK_TOP - TMP_STACK_SIZE)

/*
 * Use dynamic memory allocation
 */
#define USE_IMALLOC		(1)

/*
 * Use program trace function (in debugger support)
 */
#define USE_HOOK_TRACE		(0)

/*
 * Use clean-up sequence
 */
#define USE_CLEANUP		(1)

/*
 * Use high level programming language support routine
 */
#define USE_HLL_INTHDR		(1)
