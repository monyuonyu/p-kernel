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
 * @file tm_getline.c
 * @brief T-Monitor 行入力機能
 * 
 * T-Monitorの行入力機能を実装する。
 * シリアルポートから一行を入力し、エコーバックを行う。
 */

/**
 * @brief 一行入力（特殊キーは未サポート）
 * @param buff 入力バッファ
 * @return 入力文字数（Ctrl-Cの場合は-1）
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
