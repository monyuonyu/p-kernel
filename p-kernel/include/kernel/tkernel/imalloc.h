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
 *	@(#)imalloc.h (sys)
 *
 *	Kernel memory allocation 
 *
 *	A function for allocating memory used in parts of micro T-Kernel.
 *	Not for general use.
 */

#ifndef __SYS_IMALLOC_H__
#define __SYS_IMALLOC_H__

#include <basic.h>
#include <tk_typedef.h>

#ifdef __cplusplus
extern "C" {
#endif

IMPORT void* knl_Imalloc( size_t size );
IMPORT void* knl_Icalloc( size_t nmemb, size_t size );
IMPORT void  knl_Ifree( void *ptr );

#ifdef __cplusplus
}
#endif
#endif /* __SYS_IMALLOC_H__ */
