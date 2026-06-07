/*
 * p-kernel libc implementation
 * 句読点文字判定実装 (ispunct.c)
 * 
 * このファイルはispunct関数を実装します。
 * 文字が句読点文字（印刷可能で英数字以外）かどうかを判定します。
 */

#include "ctype.h"

/**
 * @brief 句読点文字判定
 * @param c 判定する文字
 * @return 句読点文字の場合は非0、それ以外は0
 * @note 印刷可能文字でかつ英数字ではない文字を判定
 */
int ispunct(int c)
{
	/* 印刷可能文字かつ英数字ではない文字を句読点と判定 */
	return isgraph(c) && !isalnum(c);
}