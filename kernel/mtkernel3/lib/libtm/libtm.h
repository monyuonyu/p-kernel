/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.04
 *
 *    Copyright (C) 2006-2021 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2021/05/17.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	libtm.h
 * @brief	T-Monitor 互換ライブラリの内部定義
 *
 * 制御文字コードと、通信ポートのシステム依存部が提供する
 * 初期化・送受信関数を宣言します。
 */

#include <config_tm.h>

#define	CHR_CR		(0x0D)		/* 復帰（Carriage Return） */
#define	CHR_ETX		(0x03)		/* テキスト終了（End of TeXt、Ctrl-C） */
#define	CHR_LF		(0x0A)		/* 改行（Line Feed） */

IMPORT void tm_com_init(void);

IMPORT void tm_snd_dat( const UB* buf, INT size );
IMPORT void tm_rcv_dat( UB* buf, INT size );

