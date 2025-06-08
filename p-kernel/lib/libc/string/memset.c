/*
 * p-kernel libc implementation
 * メモリ設定実装 (memset.c)
 *
 * このファイルはmemset関数を実装します。
 * メモリ領域を指定値で埋めます。
 */

#include "string.h"

/**
 * @brief メモリ領域を指定値で埋める
 * @param s 設定対象のメモリ領域
 * @param c 設定する値 (intとして渡されるがunsigned charに変換)
 * @param n 設定するバイト数
 * @return sのポインタ
 * @note 1バイトずつ指定値をメモリに書き込む
 */
void *memset(void *s, int c, size_t n)
{
	register unsigned char* _s = (unsigned char *)s;  /* メモリポインタ */
	register unsigned char _c = (unsigned char)c;     /* 設定値 */

	/* メモリ領域を指定値で埋める */
	int cnt;
	for(cnt = 0; cnt < n; cnt++)
	{
		*_s++ = _c;
	}

	return s;  /* 設定したメモリ領域のポインタを返す */
}
