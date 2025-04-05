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

#include <typedef.h>

// #define	TERM_PORT		0

// #define CPU_CLOCK		(20000000UL)
// #define BAUD_RATE		(115200UL)

// #define GPIO_BASE		(0x40033000UL)
// #define GPIO_PFR1		((_UW*)(GPIO_BASE + 0x0004UL))
// #define GPIO_PFR2		((_UW*)(GPIO_BASE + 0x0008UL))
// #define GPIO_PFR3		((_UW*)(GPIO_BASE + 0x000cUL))
// #define GPIO_PFR4		((_UW*)(GPIO_BASE + 0x0010UL))
// #define GPIO_EPFR07		((_UW*)(GPIO_BASE + 0x061cUL))
// #define GPIO_EPFR08		((_UW*)(GPIO_BASE + 0x0620UL))
// #define	GPIO_ADE		((_UW*)(GPIO_BASE + 0x0500UL))

// #if TERM_PORT == 0		/* Sxx0_0 */
// #define UART_BASE		(0x40038000UL)
// #elif TERM_PORT == 1		/* Sxx1_1 */
// #define UART_BASE		(0x40038100UL)
// #elif TERM_PORT == 2		/* Sxx2_2 */
// #define UART_BASE		(0x40038200UL)
// #elif TERM_PORT == 3		/* Sxx3_2 */
// #define UART_BASE		(0x40038300UL)
// #elif TERM_PORT == 4		/* Sxx4_1 */
// #define UART_BASE		(0x40038400UL)
// #elif TERM_PORT == 5		/* Sxx5_2 */
// #define UART_BASE		(0x40038500UL)
// #elif TERM_PORT == 6		/* Sxx6_1 */
// #define UART_BASE		(0x40038600UL)
// #elif TERM_PORT == 7		/* Sxx7_1 */
// #define UART_BASE		(0x40038700UL)
// #elif TERM_PORT == 10		/* Sxx0_1 */
// #define UART_BASE		(0x40038000UL)
// #else				/* default Sxx0_0 */
// #define UART_BASE		(0x40038000UL)
// #endif	/* TERM_PORT */
// #define UART_SMR		((_UB*)(UART_BASE + 0x00UL))
// #define UART_SCR		((_UB*)(UART_BASE + 0x01UL))
// #define UART_ESCR		((_UB*)(UART_BASE + 0x04UL))
// #define UART_SSR		((_UB*)(UART_BASE + 0x05UL))
// #define UART_DR			((_UB*)(UART_BASE + 0x08UL))
// #define UART_BGR		((_UH*)(UART_BASE + 0x0cUL))

LOCAL void sendLine( const UB *buf )
{
	// while( *buf++ != '\0' ){
		// while( (*UART_SSR & 0x02U) == 0 );
		// *UART_DR = *buf;
	// }
}

LOCAL void sendChar( const UB *buf )
{
	// while( (*UART_SSR & 0x02U) == 0 );
	// *UART_DR = *buf;
}

LOCAL UB getChar( UB *buf )
{
	// while( (*UART_SSR & 0x04U) == 0 );
	// *buf = *UART_DR;

	return *buf;
}

EXPORT void sio_send_frame( const UB* buf, INT size )
{
	if(size == 1) {			/* for tm_putchar */
		sendChar(buf);
	}
	else if (size >= 2) {		/* for tm_putstring */
		sendLine(buf);
	}
	else {
	}
}


EXPORT void sio_recv_frame( UB* buf, INT size )
{
	if(size == 1) {			/* for tm_getchar */
		getChar( buf );
	}
	else {
	}
}


EXPORT void sio_init(void)
{
//	UW	r;

// #if TERM_PORT == 0
	// r = *GPIO_ADE;
	// r &= 0xffffff7f;
	// *GPIO_ADE = r;
	// r = *GPIO_PFR2;
	// r |= 0x00000006U;
	// *GPIO_PFR2 = r; 	/* use P21, P22, P23 for Serial I/O */
	// r = *GPIO_EPFR07;
	// r |= 0x00000040U;
	// *GPIO_EPFR07 = r; 	/* use UART0_0 for IN/OUT/CLOCK */

// #elif TERM_PORT == 1
	// r = *GPIO_ADE;
	// r &= 0xfffffff1;
	// *GPIO_ADE = r;
	// *GPIO_PFR1 = 0x0000000eU; /* use P11, P12, P13 for Serial I/O */
	// *GPIO_EPFR07 = 0x0000a800U; /* use UART1_1 for IN/OUT/CLOCK */

// #elif TERM_PORT == 2
	// r = *GPIO_ADE;
	// r &= 0xfffffc7f;
	// *GPIO_ADE = r;
	// *GPIO_PFR1 = 0x00000380U; /* use P17, P18, P19 for Serial I/O */
	// *GPIO_EPFR07 = 0x003f0000U; /* use UART2_2 for IN/OUT/CLOCK */

// #elif TERM_PORT == 3
	// /* There are no bits for the UART3 in the GPIO_ADE register */
	// *GPIO_PFR4 = 0x00000700U; /* use P48, P49, P4A for Serial I/O */
	// *GPIO_EPFR07 = 0x0fc00000U; /* use UART3_2 for IN/OUT/CLOCK */

// #elif TERM_PORT == 4
	// r = *GPIO_ADE;
	// r &= 0xffffe3ff;
	// *GPIO_ADE = r;
	// *GPIO_PFR1 = 0x00001c00U; /* use P1A, P1B, P1C for Serial I/O */
	// *GPIO_EPFR08 = 0x000002a0U; /* use UART4_1 for IN/OUT/CLOCK */

// #elif TERM_PORT == 5
	/* There are no bits for the UART5 in the GPIO_ADE register */
	// *GPIO_PFR3 = 0x000001c0U; /* use P36, P37, P38 for Serial I/O */
	// *GPIO_EPFR08 = 0x0000fc00U; /* use UART5_2 for IN/OUT/CLOCK */

// #elif TERM_PORT == 6
	/* There are no bits for the UART6 in the GPIO_ADE register */
	// *GPIO_PFR3 = 0x0000000eU; /* use P33, P32, P31 for Serial I/O */
	// *GPIO_EPFR08 = 0x002a0000U; /* use UART6_1 for IN/OUT/CLOCK */

// #elif TERM_PORT == 7
	/* There are no bits for the UART7 in the GPIO_ADE register */
	// *GPIO_PFR4 = 0x00007000U; /* use P4C, P4D, P4E for Serial I/O */
	// *GPIO_EPFR08 = 0x0a800000U; /* use UART7_1 for IN/OUT/CLOCK */

// #elif TERM_PORT == 10
	// r = *GPIO_ADE;
	// r &= 0xffffff8f;
	// *GPIO_ADE = r;
	// *GPIO_PFR1 = 0x00000070U; /* use P14, P15, P16 for Serial I/O */
	// *GPIO_EPFR07 = 0x000002a0U; /* use UART0_1 for IN/OUT/CLOCK */

// #else
	// r = *GPIO_ADE;
	// r &= 0xfff8ffff;
	// *GPIO_ADE = r;
	// *GPIO_PFR2 = 0x0000000eU; /* use P21, P22, P23 for Serial I/O */
	// *GPIO_EPFR07 = 0x00000040U; /* use UART0_0 for IN/OUT/CLOCK */

// #endif	/* TERM_PORT */

	// *UART_SCR = 0x80U; /* Clear */
	// *UART_SMR = 0x01U; /* async normal mode, stop-bit = 1, LSB-first, output enable */

	// *UART_BGR = (((CPU_CLOCK)/ BAUD_RATE) - 1); /* baud rate */
	// *UART_SCR = 0x80U; /* Clear */
	// *UART_ESCR = 0x00U; /* no hardware flow control, non-parity, stop-bit = 1, data length = 8 bits */
	// *UART_SCR = 0x03U; /* RX, TX enable */
}
