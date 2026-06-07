/*
 * p-kernel libc implementation
 * 文字列連結実装 (strcat.c)
 *
 * このファイルはstrcat関数を実装します。
 * 文字列を連結します。
 */

#include "string.h"

/**
 * @brief 文字列の連結
 * @param dst 連結先文字列バッファ
 * @param src 連結元文字列
 * @return dstのポインタ
 * @note dstのNULL終端文字を上書きしてsrcを連結
 * @warning dstは十分なサイズが必要
 */
char* strcat(char* dst, const char* src)
{
	char* d = dst;  /* 連結先ポインタ */
	
	/* dstの終端まで移動 */
	while (*d)
		d++;
	
	/* srcをdstにコピー */
	while ((*d++ = *src++))
		;
	
	return dst;  /* 連結後の文字列を返す */
}