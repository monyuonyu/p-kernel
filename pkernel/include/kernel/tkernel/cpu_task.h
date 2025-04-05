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
 *	cpu_task.h (FM3)
 *	CPU-Dependent Task Start Processing
 */

#ifndef _CPU_TASK_
#define _CPU_TASK_

#include "cpu_insn.h"

/*
 * System stack configuration at task startup
 */
typedef struct {
	UW	r_[8];		/* R4-R11 */
	UW	taskmode;	/* Task mode */
	UW	exp_ret;	/* Exception return */
	/* Exception entry stack */
	UW	r[4];		/* R0-R3 */
	UW	ip;		/* R12 */
	void	*lr;		/* lr */
	void	*pc;		/* pc */
	UW	xpsr;		/* xpsr */
} SStackFrame;

/*
 * Size of system stack area destroyed by 'make_dormant()'
 * In other words, the size of area required to write by 'setup_context().'
 */
#define DORMANT_STACK_SIZE	( sizeof(VW) * 7 )	/* To 'taskmode' position */


/*
 * Create stack frame for task startup
 *	Call from 'make_dormant()'
 */
void knl_setup_context( TCB *tcb )
{
	SStackFrame	*ssp;
	UW		pc, xpsr;

	ssp = tcb->isstack;
	ssp--;

	xpsr = 0x01000000;
	pc = (UW)tcb->task;

	/* CPU context initialization */
	ssp->taskmode = 0;		/* Initial taskmode */
	ssp->exp_ret = 0xFFFFFFF9;
	ssp->lr = 0;
	ssp->xpsr = xpsr;		/* Initial SR */
	ssp->pc = (void *)(pc & ~0x00000001UL);	/* Task startup address */
	tcb->tskctxb.ssp = ssp;		/* System stack */
}

/*
 * Set task startup code
 *	Called by 'tk_sta_tsk()' processing.
 */
void knl_setup_stacd( TCB *tcb, INT stacd )
{
	SStackFrame	*ssp = tcb->tskctxb.ssp;

	ssp->r[0] = stacd;
	ssp->r[1] = (VW)tcb->exinf;
}

/*
 * Delete task contexts
 */
void knl_cleanup_context( TCB *tcb )
{
}

#endif /* _CPU_TASK_ */
