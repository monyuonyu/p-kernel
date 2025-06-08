/*
 * p-kernel libc implementation
 * 制御文字判定実装 (iscntrl.c)
 * 
 * このファイルはiscntrl関数を実装します。
 * ASCII制御文字かどうかを判定します。
 */

#include "ctype.h"

/**
 * @brief 制御文字判定
 * @param c 判定する文字
 * @return 制御文字の場合は非0、それ以外は0
 * @note ASCII制御文字(0x00-0x1F, 0x7F)を判定
 */
int iscntrl(int c)
{
	/* ASCII制御文字: 0x00-0x1F(0-31)とDEL(0x7F/127) */
	return (c >= 0 && c <= 31) || c == 127;
}