/*
 * p-kernel libc implementation
 * プログラム正常終了実装 (exit.c)
 * 
 * このファイルはexit関数を実装します。
 * プログラムを正常終了させます。
 */

#include "stdlib.h"

/**
 * @brief プログラムの正常終了
 * @param status 終了ステータスコード（組み込み環境では使用されない）
 * @note この関数は返らない、無限ループで停止する
 * @warning 組み込み環境ではシステムコールの代わりに停止する
 */
void exit(int status)
{
	(void)status;  /* 組み込み環境ではステータスコードを無視 */
	
	/* 無限ループでプログラムを停止 */
	while (1) {
		__asm__ volatile("nop");  /* CPUをアイドル状態にする */
	}
}