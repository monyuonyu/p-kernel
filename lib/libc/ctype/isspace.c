/*
 * p-kernel libc implementation
 * 空白文字判定実装 (isspace.c)
 * 
 * このファイルはisspace関数を実装します。
 * 文字が空白文字（スペース、タブ、改行等）かどうかを判定します。
 */

#include "ctype.h"

/**
 * @brief 空白文字判定
 * @param c 判定する文字
 * @return 空白文字の場合は非0、それ以外は0
 * @note スペース、タブ、改行、垂直タブ、フォームフィード、キャリッジリターンを判定
 */
int isspace(int c)
{
	/* 標準的な空白文字を判定: SP, TAB, LF, VT, FF, CR */
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}