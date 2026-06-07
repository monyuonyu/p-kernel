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
 *	@(#)util.h
 *
 *	General Utilities 
 */

#ifndef	__TK_UTIL_H__
#define __TK_UTIL_H__

#include <basic.h>
#include <tk_typedef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fast Lock
 */
typedef struct {
	INT	cnt;
	ID	id;
} FastLock;

IMPORT ER CreateLock( FastLock *lock, CONST UB *name );
IMPORT void DeleteLock( FastLock *lock );
IMPORT void Lock( FastLock *lock );
IMPORT void Unlock( FastLock *lock );

/*
 * Multi Lock
 *	Can use the maximum of 16 or 32 independent locks with a single FastMLock.
 *	Divided by the lock number (no). Can specify 0-15 or 0-31 for 'no.'
 *	(Slightly less efficient than FastLock)
 */
typedef struct {
	UINT	flg;
	INT	wai;
	ID	id;
} FastMLock;

IMPORT ER CreateMLock( FastMLock *lock, CONST UB *name );
IMPORT ER DeleteMLock( FastMLock *lock );
IMPORT ER MLockTmo( FastMLock *lock, INT no, TMO tmout );
IMPORT ER MLock( FastMLock *lock, INT no );
IMPORT ER MUnlock( FastMLock *lock, INT no );



#ifdef __cplusplus
}
#endif
#endif /* __TK_UTIL_H__ */
