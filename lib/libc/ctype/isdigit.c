/*
 * p-kernel libc implementation
 * 数字判定実装 (isdigit.c)
 * 
 * このファイルはisdigit関数を実装します。
 * 文字がASCII数字(0-9)かどうかを判定します。
 */

#include "ctype.h"

/**
 * @brief 数字判定
 * @param c 判定する文字
 * @return 数字の場合は非0、それ以外は0
 * @note ASCII数字(0-9: 0x30-0x39)を判定
 */
int isdigit(int c)
{
	/* ASCII数字の範囲チェック (0-9: 0x30-0x39) */
	return c >= '0' && c <= '9';
}