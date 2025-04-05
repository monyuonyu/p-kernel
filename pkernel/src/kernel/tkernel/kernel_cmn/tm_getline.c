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
#include <syslib.h>

IMPORT void sio_send_frame( const UB* buf, INT size );
IMPORT void sio_recv_frame( UB* buf, INT size );

/*
 * special key is not supported
 */
EXPORT INT tm_getline( UB *buff )
{
	UB* p = buff;
	int len = 0;
	static const char LF = 0x0a;
	INT imask;

	DI(imask);

	for (;;) {
		sio_recv_frame(p, 1);
		sio_send_frame(p, 1); /* echo back */
		if (*p == 0x0d) {
			sio_send_frame((const void *)&LF, 1);
			*p = 0x00;
			goto err_ret;
		} else if (*p == 0x03) {
			*p = 0x00;
			len = -1;
			goto err_ret;
		}
		p++;
		len++;
	}

err_ret:
	EI(imask);

	return len;
}
