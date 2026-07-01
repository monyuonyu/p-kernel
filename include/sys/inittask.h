/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.00
 *
 *    Copyright (C) 2006-2019 by Ken Sakamura.
 *    This software is distributed under the T-License 2.1.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2019/12/11.
 *
 *----------------------------------------------------------------------
 */

/*
 *	inittask.h
 *	Initial task definition
 */

#ifndef _INITTASK_DEF_
#define _INITTASK_DEF_

/*
 * Initial task parameter
 *
 * p-kernel 変更: ターゲット側（sysdef.h 等）で上書きできるよう
 * #ifndef ガードを追加。Linux ユーザモードポートは usermain が
 * 大きなスタックを必要とするため INITTASK_STKSZ を差し替える。
 */
#ifndef INITTASK_EXINF
#define INITTASK_EXINF		(0x0)
#endif
#ifndef INITTASK_ITSKPRI
#define INITTASK_ITSKPRI	(1)
#endif
#ifndef INITTASK_STKSZ
#define INITTASK_STKSZ		(1*1024)
#endif
#ifndef INITTASK_DSNAME
#define INITTASK_DSNAME		"inittsk"
#endif

#if USE_IMALLOC

#define INITTASK_TSKATR		(TA_HLNG | TA_RNG0)
#define INITTASK_STACK		(NULL)

#else

#define INITTASK_TSKATR		(TA_HLNG | TA_RNG0 | TA_USERBUF)
#define INITTASK_STACK		init_task_stack

#endif

#endif /* _INITTASK_DEF_ */
