/*
 * p-kernel libc implementation
 * 文字列長取得実装 (strlen.c)
 * 
 * このファイルはstrlen関数を実装します。
 * NULL終端文字列の長さを効率的に計算します。
 */

#include "string.h"

/**
 * @brief 文字列の長さを取得
 * @param s 長さを測定する文字列
 * @return 文字列の長さ（NULL終端文字を除く）
 * @note NULLポインタを渡した場合の動作は未定義
 */
size_t strlen(const char* s)
{
	size_t len = 0;  /* 文字数カウンタ */
	
	/* NULL終端文字が現れるまでループ */
	while (*s++)
		len++;
	
	return len;
}