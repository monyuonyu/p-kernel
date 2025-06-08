/*
 * p-kernel libc implementation
 * 大文字判定実装 (isupper.c)
 * 
 * このファイルはisupper関数を実装します。
 * 文字がASCII大文字(A-Z)かどうかを判定します。
 */

#include "ctype.h"

/**
 * @brief 大文字判定
 * @param c 判定する文字
 * @return 大文字の場合は非0、それ以外は0
 * @note ASCII大文字(A-Z: 0x41-0x5A)を判定
 */
int isupper(int c)
{
	/* ASCII大文字の範囲チェック (A-Z: 0x41-0x5A) */
	return c >= 'A' && c <= 'Z';
}