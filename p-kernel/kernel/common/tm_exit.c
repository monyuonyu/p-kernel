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

/**
 * @file tm_exit.c
 * @brief T-Monitor 終了処理
 * 
 * T-Monitorの終了処理機能を実装する。
 * 現在はスタブ実装で、無限ループする。
 */
#include <typedef.h>

/**
 * @brief T-Monitor終了処理
 * @param mode 終了モード
 */
void tm_exit( INT mode )
{
	for(;;) {
		;
	}
}
