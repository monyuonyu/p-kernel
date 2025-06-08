/*
 * p-kernel libc implementation
 * 英数字判定実装 (isalnum.c)
 * 
 * このファイルはisalnum関数を実装します。
 * 文字が英字または数字かどうかを判定します。
 */

#include "ctype.h"

/**
 * @brief 英数字判定
 * @param c 判定する文字
 * @return 英数字の場合は非0、それ以外は0
 * @note isalpha()とisdigit()の論理和で実装
 */
int isalnum(int c)
{
	return isalpha(c) || isdigit(c);  /* 英字または数字の場合にtrue */
}