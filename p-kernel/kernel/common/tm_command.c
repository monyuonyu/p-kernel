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
 * @file tm_command.c
 * @brief T-Monitor コマンド処理
 * 
 * T-Monitorのコマンド処理機能を実装する。
 * 現在はスタブ実装で、無限ループする。
 */
#include <typedef.h>

/**
 * @brief コマンド処理
 * @param buff コマンドバッファ
 * @return 処理結果（現在は未実装）
 */
INT tm_command ( UB *buff )
{
	for(;;) {
		;
	}
}
