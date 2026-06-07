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

#include <tk_typedef.h>
#include <errno.h>
#include <typedef.h>

/*
 * CPU-dependent initialization
 */
EXPORT ER knl_cpu_initialize( void )
{
#if USE_TRAP
IMPORT void knl_dispatch_entry( void );		/* Dispatcher call */

	/* Register exception handler used on OS */
//	knl_define_inthdr(EXP_PSV, knl_dispatch_entry);
#if USE_DBGSPT
IMPORT void knl_call_dbgspt( void );		/* Debugger support call */
//	knl_define_inthdr(EXP_DBG, knl_call_dbgspt);
#endif
#endif /* USE_TRAP */

#if USE_TRAP || CFN_MAX_SSYID > 0
IMPORT void knl_call_entry( void );			/* System call */
//	knl_define_inthdr(EXP_SVC,      knl_call_entry);
#endif

	return E_OK;
}

#if USE_CLEANUP
/*
 * CPU-dependent finalization
 */
EXPORT void knl_cpu_shutdown( void )
{
}
#endif /* USE_CLEANUP */
