/*
 * p-kernel libc implementation
 * 英字判定実装 (isalpha.c)
 * 
 * このファイルはisalpha関数を実装します。
 * 文字がASCII英字（A-Z, a-z）かどうかを判定します。
 */

#include "ctype.h"

/**
 * @brief 英字判定
 * @param c 判定する文字
 * @return 英字の場合は非0、それ以外は0
 * @note ASCII範囲内の大文字(A-Z)と小文字(a-z)を判定
 */
int isalpha(int c)
{
	/* 大文字(A-Z: 0x41-0x5A)または小文字(a-z: 0x61-0x7A)の範囲チェック */
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}