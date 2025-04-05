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
 *	patch.h (FM3)
 *	System-dependent initialize process
 */

#ifndef _PATCH_
#define _PATCH_

/* Define '1' when using patch processes */
#define USE_SYSDEPEND_PATCH1	0
#define USE_SYSDEPEND_PATCH2	0

EXPORT void sysdepend_patch1( void );
EXPORT void sysdepend_patch2( void );

#endif /* _PATCH_ */
