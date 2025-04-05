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
 *	inittask_def.c
 *	Initial task definition
 */
/*--------------------------------------------------------------------*/
/*  Include definition                                                */
/*--------------------------------------------------------------------*/
/*--------------------------------------------------------------------*/
/*  Struct definition                                                 */
/*--------------------------------------------------------------------*/
/*--------------------------------------------------------------------*/
/*  Constant definition                                               */
/*--------------------------------------------------------------------*/
/*--------------------------------------------------------------------*/
/*  Variable definition                                               */
/*--------------------------------------------------------------------*/
/*--------------------------------------------------------------------*/
/*  Macro definition                                                  */
/*--------------------------------------------------------------------*/
/*--------------------------------------------------------------------*/
/*  Prototype declaration                                             */
/*--------------------------------------------------------------------*/
/*--------------------------------------------------------------------*/
/*  Function definition                                               */
/*--------------------------------------------------------------------*/

#include "inittask_def.h"

IMPORT void knl_init_task(void);

#if !USE_IMALLOC
INT	init_task_stack[INITTASK_STKSZ/sizeof(INT)];
#endif

/*
 * Initial task creation parameter
 */
EXPORT const T_CTSK knl_c_init_task = {
	(void *)INITTASK_EXINF,		/* exinf */
#if USE_IMALLOC
	INITTASK_TSKATR,		/* tskatr */
#else
	INITTASK_TSKATR|TA_USERBUF,	/* tskatr */
#endif
	(FP)&knl_init_task,		/* task */
	INITTASK_ITSKPRI,		/* itskpri */
	INITTASK_STKSZ,			/* stksz */
#if USE_OBJECT_NAME
	INITTASK_DSNAME,		/* dsname */
#endif
#if USE_IMALLOC
	INITTASK_STACK,			/* bufptr */
#else
	init_task_stack,		/* bufptr */
#endif
};
