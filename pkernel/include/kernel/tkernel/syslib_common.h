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
 *	@(#)syslib_common.h (tk)
 *
 *	System Library
 */

#ifndef __TK_SYSLIB_COMMON_H__
#define __TK_SYSLIB_COMMON_H__

/*
 * CPU-dependent
 */
// #if APP_AT91
// #include <syslib_depend.h>
// #endif

// #if APP_H8S2212
// #include <syslib_depend.h>
// #endif

// #if APP_MB9AF312K
// #include <syslib_depend.h>
// #endif

#if _APP_RL78G13_R5F100ADASP_
#include <syslib_depend.h>
#endif

#endif /* __TK_SYSLIB_COMMON_H__ */
