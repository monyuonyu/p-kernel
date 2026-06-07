/*
 * p-kernel libc implementation
 * 印刷可能文字判定実装（空白以外）(isgraph.c)
 * 
 * このファイルはisgraph関数を実装します。
 * 文字が印刷可能でかつ空白文字ではないかどうかを判定します。
 */

#include "ctype.h"

/**
 * @brief 印刷可能文字判定（空白以外）
 * @param c 判定する文字
 * @return 印刷可能文字（空白以外）の場合は非0、それ以外は0
 * @note ASCII印刷可能文字からスペースを除いた範囲(0x21-0x7E)
 */
int isgraph(int c)
{
	/* 印刷可能文字からスペース(0x20)を除いた範囲: 0x21-0x7E(33-126) */
	return c >= 33 && c <= 126;
}