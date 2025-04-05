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
 *	sysmain.h (sysmain)
 *	Kernel Main
 */

#ifndef _SYSMAIN_
#define _SYSMAIN_

#include <basic.h>
#include <tkernel.h>
#include <tmonitor.h>

/* Boot message */
#define BOOT_MESSAGE \
	"\n" \
	"micro T-Kernel Version 2.00.00\n" \
	"\n\0"

/*
 * User main
 */
IMPORT INT usermain( void );

#endif /* _SYSMAIN_ */
