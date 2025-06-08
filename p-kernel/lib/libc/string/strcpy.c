/*
 * p-kernel libc implementation
 * 文字列コピー実装 (strcpy.c)
 * 
 * このファイルはstrcpy関数を実装します。
 * NULL終端文字列を安全にコピーします。
 */

#include "string.h"

/**
 * @brief 文字列のコピー
 * @param dst コピー先の文字列バッファ
 * @param src コピー元の文字列
 * @return dst のポインタ
 * @note srcのNULL終端文字も含めてコピーする
 * @warning dstは十分なサイズを持つ必要がある
 */
char* strcpy(char* dst, const char* src)
{
	char* d = dst;  /* 元のdstポインタを保存 */
	
	/* NULL終端文字を含めてコピー */
	while ((*d++ = *src++))
		;  /* 代入とポインタ進行を同時実行 */
	
	return dst;  /* コピー先の開始アドレスを返す */
}