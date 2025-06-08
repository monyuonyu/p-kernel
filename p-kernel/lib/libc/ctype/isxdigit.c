/*
 * p-kernel libc implementation
 * 16進数字判定実装 (isxdigit.c)
 * 
 * このファイルはisxdigit関数を実装します。
 * 文字が16進数字(0-9, A-F, a-f)かどうかを判定します。
 */

#include "ctype.h"

/**
 * @brief 16進数字判定
 * @param c 判定する文字
 * @return 16進数字の場合は非0、それ以外は0
 * @note 数字(0-9)、大文字(A-F)、小文字(a-f)を判定
 */
int isxdigit(int c)
{
	/* 数字(0-9)または大文字(A-F)または小文字(a-f)の判定 */
	return isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}