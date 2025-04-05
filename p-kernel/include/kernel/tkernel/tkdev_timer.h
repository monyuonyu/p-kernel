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
 *	tkdev_timer.h (FM3)
 *	Hardware-Dependent Timer Processing
 */

#ifndef _TKDEV_TIMER_
#define _TKDEV_TIMER_

#include <syslib.h>
#include <sysinfo.h>
#include "tkdev_conf.h"

#define ENAINT	Asm("ldr	r0, =0 \n" \
		    "msr	basepri, r0")

#define DISINT	Asm("ldr	r0, =MAX_EXTINT_PRI \n" \
		    "msr	basepri, r0")

/*
 * Settable interval range (millisecond)
 */
#define MIN_TIMER_PERIOD	1
#define MAX_TIMER_PERIOD	50

/*
 * Set timer
 */
void knl_init_hw_timer( void )
{
	UINT	n, imask;

	DI(imask);

	/* Set System timer CLK source to Core, Systick exception enable */
	out_w(SYST_CSR, 0x00000006);

	/* Set counter */
	n = (UW)(CFN_TIMER_PERIOD * (TMCLK * 1000) - 1);
	out_w(SYST_RVR, n);

	/* Start timer count */
	out_w(SYST_CSR, 0x00000007);

	EI(imask);
}

/*
 * Timer start processing
 *	Initialize the timer and start the periodical timer interrupt.
 */
void knl_start_hw_timer( void )
{
IMPORT	void	knl_timer_handler_startup( void );

	/* Set timer */
	knl_init_hw_timer();
}

/*
 * Clear timer interrupt
 *	Clear the timer interrupt request. Depending on the type of
 *	hardware, there are two timings for clearing: at the beginning
 *	and the end of the interrupt handler.
 *	'clear_hw_timer_interrupt()' is called at the beginning of the
 *	timer interrupt handler.
 *	'end_of_hw_timer_interrupt()' is called at the end of the timer
 *	interrupt handler.
 *	Use either or both according to hardware.
 */
IMPORT void knl_clear_hw_timer_interrupt( void );


void knl_end_of_hw_timer_interrupt( void )
{

}

/*
 * Timer stop processing
 *	Stop the timer operation.
 *	Called when system stops.
 */
void knl_terminate_hw_timer( void )
{
	/* Timer interrupt disable */
	out_w(SYST_CSR, 0x00000000);
}

/*
 * Get processing time from the previous timer interrupt to the
 * current (nanosecond)
 *	Consider the possibility that the timer interrupt occurred
 *	during the interrupt disable and calculate the processing time
 *	within the following
 *	range: 0 <= Processing time < CFN_TIMER_PERIOD * 2
 */
UW knl_get_hw_timer_nsec( void )
{
	UW	ofs;
	UINT	imask;

	DI(imask);

	ofs = (in_w(SYST_CVR) & 0xffffff) + 1;

	EI(imask);

	return (ofs * 1000 ) / TMCLK;
}

#endif /* _TKDEV_TIMER_ */
