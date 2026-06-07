/*
 * p-kernel libc implementation
 * メモリコピー実装 (memcpy.c)
 * 
 * このファイルはmemcpy関数を実装します。
 * メモリ領域が重複しない場合の高速コピーを行います。
 */

#include "string.h"

/**
 * @brief メモリコピー
 * @param dst コピー先のメモリ領域
 * @param src コピー元のメモリ領域
 * @param n コピーするバイト数
 * @return dst のポインタ
 * @note src と dst が重複している場合の動作は未定義です
 * @warning 重複するメモリ領域の場合はmemmove()を使用してください
 */
void* memcpy(void* dst, const void* src, size_t n)
{
	char* d = (char*)dst;          /* コピー先ポインタ */
	const char* s = (const char*)src; /* コピー元ポインタ */
	
	/* 1バイトずつコピー */
	while (n--)
		*d++ = *s++;
	
	return dst;  /* コピー先のポインタを返す */
}