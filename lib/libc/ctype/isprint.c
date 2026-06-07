/*
 * p-kernel libc implementation
 * 印刷可能文字判定実装 (isprint.c)
 * 
 * このファイルはisprint関数を実装します。
 * 文字が印刷可能文字（空白文字を含む）かどうかを判定します。
 */

#include "ctype.h"

/**
 * @brief 印刷可能文字判定（空白文字を含む）
 * @param c 判定する文字
 * @return 印刷可能文字の場合は非0、それ以外は0
 * @note ASCII印刷可能文字(0x20-0x7E)を判定、スペースも含む
 */
int isprint(int c)
{
	/* 印刷可能文字の範囲: 0x20-0x7E(32-126)、スペースを含む */
	return c >= 32 && c <= 126;
}