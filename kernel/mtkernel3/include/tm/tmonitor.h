/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.00
 *
 *    Copyright (C) 2006-2019 by Ken Sakamura.
 *    This software is distributed under the T-License 2.1.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2019/12/11.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	tmonitor.h
 * @brief	T-Monitor 互換呼び出し
 *
 * コンソール入出力を行う T-Monitor 互換のサービス関数
 * （文字・行・書式付き出力）を宣言します。
 */

#ifndef __TM_TMONITOR_H__
#define __TM_TMONITOR_H__

#include <sys/machine.h>
#include <tk/typedef.h>


IMPORT void libtm_init(void);

/*
 * モニタサービス関数
 */
IMPORT INT  tm_getchar( INT wait );
IMPORT INT  tm_putchar( INT c );
IMPORT INT  tm_getline( UB *buff );
IMPORT INT  tm_putstring( const UB *buff );
IMPORT INT  tm_printf( const UB *format, ... );
IMPORT INT  tm_sprintf( UB *str, const UB *format, ... );

#endif /* __TM_TMONITOR_H__ */
