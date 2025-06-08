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

IMPORT void sio_recv_frame( UB* buf, INT size );

/**
 * @file tm_getchar.c
 * @brief T-Monitor 文字入力機能
 * 
 * T-Monitorの文字入力機能を実装する。
 * シリアルポートから一文字を受信する。
 */

/**
 * @brief 一文字入力
 * @param wait 待機フラグ（ポーリングは未サポート）
 * @return 入力された文字コード
 */
EXPORT INT tm_getchar( INT wait )
{
	UB	p;
	UINT	imask;

	DI(imask);

	sio_recv_frame(&p, 1);

	EI(imask);

	return (INT)p;
}
