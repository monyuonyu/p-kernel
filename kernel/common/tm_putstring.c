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

/**
 * @file tm_putstring.c
 * @brief T-Monitor 文字列出力機能
 * 
 * T-Monitorの文字列出力機能を実装する。
 * シリアルポートへ文字列を出力する。
 */

/**
 * @brief 文字列出力（Ctrl-Cは未サポート）
 * @param buff 出力する文字列
 * @return 0（固定値）
 */
EXPORT INT tm_putstring( UB *buff )
{
	const UB* p = buff;
	static const char CR = 0x0d;
	INT imask;

	DI(imask);

	while (*p != '\0') {
		if (*p == 0x0a) { /* LF */
			sio_send_frame((const void *)&CR, 1);
			sio_send_frame((const void *)p, 1);
		} else {
			sio_send_frame((const void *)p, 1);
		}
		p++;
	}

	EI(imask);

	return 0;
}
