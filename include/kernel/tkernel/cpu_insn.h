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
 *	cpu_insn.h (FM3)
 *	LM3S9B92-Dependent Operation
 */

#ifndef _CPU_INSN_
#define _CPU_INSN_

#include <sysinfo.h>

/* ------------------------------------------------------------------------ */
/*
 *	Control register operation
 */

IMPORT UINT knl_getXPSR(void);
IMPORT UINT knl_getBASEPRI(void);
IMPORT UINT knl_getPRIMASK(void);

/* ------------------------------------------------------------------------ */
/*
 *	EIT-related
 */


/*
 * Set interrupt handler
 */
void knl_define_inthdr( INT vecno, FP inthdr )
{
	knl_intvec[vecno] = inthdr;
}

/*
 * If it is the task-independent part, TRUE
 */
BOOL knl_isTaskIndependent( void )
{
	return ( knl_taskindp > 0 )? TRUE: FALSE;
}

/*
 * Move to/Restore task independent part
 */
void knl_EnterTaskIndependent( void )
{
	knl_taskindp++;
}
void knl_LeaveTaskIndependent( void )
{
	knl_taskindp--;
}

/* ------------------------------------------------------------------------ */

#endif /* _CPU_INSN_ */
