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
 *	@(#)debug.h (sys)
 *
 *	Debug support
 */

#ifndef	__SYS_DEBUG_H__
#define __SYS_DEBUG_H__

#include <basic.h>

#ifdef DEBUG
#include <tmonitor.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Example
 *	DEBUG_PRINT(("error = %d\n", ercd));
 *
 *	DO_DEBUG( if ( ercd < E_OK ) DEBUG_PRINT(("error = %d\n", ercd)); )
 */
#ifdef DEBUG

#ifndef DEBUG_MODULE
#define DEBUG_MODULE	""	/* Normally define like "(module name)" */
#endif

/*
 * Example
 * #define DEBUG_PRINT(arg)						\
 * 	(								\
 * 		printf("%s#%d%s:", __FILE__, __LINE__, DEBUG_MODULE),	\
 * 		printf arg						\
 * 	)
 */

#define DEBUG_PRINT(arg)
#define DO_DEBUG(exp)	{ exp }

#else /* DEBUG */

#define DEBUG_PRINT(arg)	/* arg */
#define DO_DEBUG(exp)		/* exp */

#endif /* DEBUG */

#ifdef __cplusplus
}
#endif
#endif /* __SYS_DEBUG_H__ */
