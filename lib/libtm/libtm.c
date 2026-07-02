/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.03
 *
 *    Copyright (C) 2006-2021 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2021/03/31.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	libtm.c
 * @brief	T-Monitor 互換呼び出しライブラリ
 *
 * コンソール入出力を行う T-Monitor 互換の基本関数
 * （1 文字・1 行単位の入出力）を実装します。
 * 各関数は割込み禁止状態（DI/EI）で通信ポートに直接アクセスします。
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#if USE_TMONITOR
#include "libtm.h"

/**
 * @brief	T-Monitor 互換ライブラリの初期化
 *
 * 通信ポートのシステム依存部の初期化（tm_com_init()）を行います。
 */
EXPORT void libtm_init(void)
{
	tm_com_init();
}

/**
 * @brief	コンソールからの 1 文字入力
 * @param wait	待ち指定（本実装では wait != 0 の待ちありのみサポート。
 *		ポーリング（wait == 0）は未サポート）
 * @return 入力した文字の文字コード
 * @note 1 文字受信するまでブロックします。受信中は割込みを禁止します。
 */
EXPORT INT tm_getchar( INT wait )
{
	UB	p;
	UINT	imask;

	DI(imask);
	tm_rcv_dat(&p, 1);
	EI(imask);

	return (INT)p;
}

/**
 * @brief	コンソールからの 1 行入力
 * @param buff	入力文字列の格納先バッファ（終端に '\0' を付加）
 * @return 入力した文字列の長さ。Ctrl-C（ETX）で中断された場合は -1
 * @note 入力文字はエコーバックします。CR の入力で行の終わりとみなし、
 *	LF を追加でエコーします。特殊キーは未サポートです。
 */
EXPORT INT tm_getline( UB *buff )
{
	UB* p = buff;
	int len = 0;
	static const char LF = CHR_LF;
	INT imask;

	DI(imask);
	while (1) {
		tm_rcv_dat(p, 1);
		tm_snd_dat(p, 1); /* エコーバック */
		if (*p == CHR_CR) {
			tm_snd_dat((const UB*)&LF, 1);
			break;
		} else if (*p == CHR_ETX) {
			len = -1;
			break;
		}
		p++; len++;
	}
	*p = 0x00;
	EI(imask);

	return len;
}

/**
 * @brief	コンソールへの 1 文字出力
 * @param c	出力する文字
 * @return 常に 0
 * @note LF は CR + LF に変換して出力します。Ctrl-C による中断は
 *	未サポートです。
 */
EXPORT INT tm_putchar( INT c )
{
	static const char CR = CHR_CR;
	UB buf = (UB)c;
	INT imask;

	DI(imask);
	if (buf == CHR_LF) {
		tm_snd_dat((const UB*)&CR, 1);
	}
	tm_snd_dat(&buf, 1);
	EI(imask);

	return 0;
}

/**
 * @brief	コンソールへの文字列出力
 * @param buff	出力する文字列（'\0' 終端）
 * @return 常に 0
 * @note 1 文字ずつ tm_putchar() で出力します（LF は CR + LF に変換）。
 *	Ctrl-C による中断は未サポートです。
 */
EXPORT INT tm_putstring( const UB *buff )
{
	const UB* p = buff;
	INT imask;

	DI(imask);
	while ( *p != (UB)'\0' ) {
		tm_putchar(*p++);
	}
	EI(imask);

	return 0;
}

#endif /* USE_TMONITOR */