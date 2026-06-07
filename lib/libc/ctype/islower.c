/*
 * p-kernel libc implementation
 * 小文字判定実装 (islower.c)
 * 
 * このファイルはislower関数を実装します。
 * 文字がASCII小文字(a-z)かどうかを判定します。
 */

#include "ctype.h"

/**
 * @brief 小文字判定
 * @param c 判定する文字
 * @return 小文字の場合は非0、それ以外は0
 * @note ASCII小文字(a-z: 0x61-0x7A)を判定
 */
int islower(int c)
{
	/* ASCII小文字の範囲チェック (a-z: 0x61-0x7A) */
	return c >= 'a' && c <= 'z';
}