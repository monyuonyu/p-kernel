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

/*
 * Definition of subsystem control block
 */
typedef struct subsystem_control_block	SSYCB;
struct subsystem_control_block {
	SVC	svchdr;		/* Extended SVC handler */
#if TA_GP
	void	*gp;		/* Global pointer */
#endif
};

IMPORT SSYCB knl_ssycb_table[];	/* Subsystem control block */

#define get_ssycb(id)	( &knl_ssycb_table[INDEX_SSY(id)] )

/*
 * Undefined extended SVC function 
 */
IMPORT INT knl_no_support( void *pk_para, FN fncd );

#endif /* _SUBSYSTEM_H_ */
