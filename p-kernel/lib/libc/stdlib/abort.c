/*
 * p-kernel libc implementation
 * プログラム異常終了実装 (abort.c)
 * 
 * このファイルはabort関数を実装します。
 * プログラムを異常終了させます。
 */

#include "stdlib.h"

/**
 * @brief プログラムの異常終了
 * @note この関数は返らない、無限ループで停止する
 * @warning 緊急停止用の関数、通常のプログラム終了ではない
 */
void abort(void)
{
	/* 無限ループでプログラムを停止 */
	while (1) {
		__asm__ volatile("nop");  /* CPUをアイドル状態にする */
	}
}