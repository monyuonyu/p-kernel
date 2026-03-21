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
 *	subsystem.h
 *	Subsystem Manager
 */

#ifndef _SUBSYSTEM_H_
#define _SUBSYSTEM_H_

typedef INT  (*SVC)( void *pk_para, FN fncd );	/* Extended SVC handler */
typedef void (*SSYCLEANUP)( ID tskid );		/* Cleanup hook (called on task exit) */

/*
 * Definition of subsystem control block
 */
typedef struct subsystem_control_block	SSYCB;
struct subsystem_control_block {
	SVC		svchdr;		/* Extended SVC handler */
	SSYCLEANUP	cleanupfn;	/* Per-task cleanup hook (NULL = none) */
#if TA_GP
	void		*gp;		/* Global pointer */
#endif
};

IMPORT SSYCB knl_ssycb_table[];	/* Subsystem control block */

#define get_ssycb(id)	( &knl_ssycb_table[INDEX_SSY(id)] )

/*
 * Undefined extended SVC function
 */
IMPORT INT knl_no_support( void *pk_para, FN fncd );

/*
 * Call all registered subsystem cleanup hooks for a terminating task.
 * Must be called at task level (not interrupt context).
 */
IMPORT void knl_ssy_cleanup( ID tskid );

#endif /* _SUBSYSTEM_H_ */
