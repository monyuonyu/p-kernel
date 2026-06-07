/*
 * p-kernel libc implementation
 * 文字列連結実装 (strncat.c)
 *
 * このファイルはstrncat関数を実装します。
 * 文字列を指定された長さだけ連結します。
 */

#include "string.h"

/**
 * @brief 文字列を指定長だけ連結
 * @param dst 連結先文字列バッファ
 * @param src 連結元文字列
 * @param n 連結する最大文字数
 * @return dstのポインタ
 * @note srcのNULL終端文字を含めて最大n文字を連結
 * @warning dstは十分なサイズが必要
 */
char* strncat(char* dst, const char* src, size_t n)
{
	char* d = dst;
	
	while (*d)
		d++;
	
	while (n-- && *src)
		*d++ = *src++;
	
	*d = '\0';
	return dst;
}