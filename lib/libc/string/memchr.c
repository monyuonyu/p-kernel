/*
 * p-kernel libc implementation
 * メモリ文字検索実装 (memchr.c)
 *
 * このファイルはmemchr関数を実装します。
 * メモリ領域から指定文字を検索します。
 */

#include "string.h"

/**
 * @brief メモリ領域から文字を検索
 * @param s 検索対象のメモリ領域
 * @param c 検索する文字 (intとして渡されるがunsigned charに変換)
 * @param n 検索するバイト数
 * @return 見つかった位置のポインタ、見つからない場合はNULL
 */
void* memchr(const void* s, int c, size_t n)
{
	const unsigned char* p = (const unsigned char*)s;  /* 検索ポインタ */
	unsigned char ch = (unsigned char)c;               /* 検索文字 */
	
	/* メモリ領域を1バイトずつ検索 */
	while (n--) {
		if (*p == ch)
			return (void*)p;  /* 文字が見つかった */
		p++;
	}
	
	return NULL;  /* 文字が見つからなかった */
}