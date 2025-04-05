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
 *	@(#)int.c (libtk/FM3)
 *
 *	Interrupt controller
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

#include <basic.h>
#include <syslib.h>
#include <sysdef.h>

#define INTPRI_MIN_UNIT	(0x100 >> INTPRI_BITWIDTH)
#define INTPRI_MASK	(0x100 - INTPRI_MIN_UNIT)

/*
 * Set Interrupt Mask Level in CPU
 */
IMPORT void SetCpuIntLevel( INT level )
{
	enaint((level + INTPRI_MIN_UNIT) & INTPRI_MASK);
}

/*
 * Get Interrupt Mask Level in CPU
 */
IMPORT INT GetCpuIntLevel( void )
{
	return (get_basepri() - INTPRI_MIN_UNIT) & INTPRI_MASK;
}

/*
 * Enable interrupt 
 *	Enables the interrupt specified in intno.
 *	External Interrupt can be specified. 
 */
EXPORT void EnableInt( UINT intno, INT level )
{
	UINT	imask;
	
	DI(imask);
	/* Set interrupt priority level. */
	*(_UB*)(NVIC_IPR(intno)) = (UB)level;
	/* Enables the specified interrupt. */
	*(_UW*)(NVIC_ISER(intno)) = (0x01U << (intno % 32));

	EI(imask);
}

/*
 * Disable interrupt 
 *	Disables the interrupt specified in intno.
 *	External Interrupt can be specified. 
 */
EXPORT void DisableInt( UINT intno )
{
	*(_UW*)(NVIC_ICER(intno)) = (0x01U << (intno % 32));
}

/*
 * Set-Pending
 *  Pends the associated interrupt under software control.
 *	External Interrupt can be specified. 
 */
EXPORT void SetPendingInt( UINT intno )
{
	*(_UW*)(NVIC_ISPR(intno)) = (0x01U << (intno % 32));
}

/*
 * Clear-Pending
 *	Un-pends the associated interrupt under software control.
 *	External Interrupt can be specified. 
 */
EXPORT void ClearPendingInt( UINT intno )
{
	*(_UW*)(NVIC_ICPR(intno)) = (0x01U << (intno % 32));
}

/*
 * Check active state
 *	Current active state for the associated interrupt
 *	External Interrupt can be specified. 
 */
EXPORT BOOL CheckInt( UINT intno )
{
	return (*(_UW*)(NVIC_ICPR(intno)) & (0x01U << (intno % 32)));
}
