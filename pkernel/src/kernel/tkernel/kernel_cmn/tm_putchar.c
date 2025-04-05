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

#include <typedef.h>
#include <stddef.h>

IMPORT void sio_send_frame( const UB* buf, INT size );

/*
 * Ctrl-C is not supported
 */
EXPORT INT tm_putchar( INT c )
{
	static const char CR = 0x0d;
	UB buf = (UB)c;

	if (buf == 0x0a) {
		sio_send_frame((const UB*)&CR, 1);
	}
	sio_send_frame(&buf, 1);

	return 0;
}
