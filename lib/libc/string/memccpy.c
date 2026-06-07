/*
 * p-kernel libc implementation
 * メモリコピー実装 (memccpy.c)
 *
 * このファイルはmemccpy関数を実装します。
 * メモリ領域を指定バイト数コピーします。
 */

#include "string.h"

/**
 * @brief メモリ領域のコピー
 * @param s1 コピー先メモリ領域
 * @param s2 コピー元メモリ領域
 * @param n コピーするバイト数
 * @return s1のポインタ
 * @note メモリ領域が重複している場合の動作は未定義
 */
void *memcpy(void * s1, const void * s2, size_t n)
{
	register unsigned char* _s1 = (unsigned char *)s1;  /* コピー先ポインタ */
	register unsigned char* _s2 = (unsigned char *)s2;  /* コピー元ポインタ */

	/* 1バイトずつコピー */
	int cnt;
	for(cnt = 0; cnt < n; cnt++)
	{
		*_s1++ = *_s2++;
	}

	return s1;  /* コピー先のポインタを返す */
}
