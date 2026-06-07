/*
 * p-kernel libc implementation
 * メモリ移動実装 (memmove.c)
 *
 * このファイルはmemmove関数を実装します。
 * メモリ領域を安全に移動します（領域重複を考慮）。
 */

#include "string.h"

/**
 * @brief メモリ領域の移動
 * @param dst 移動先メモリ領域
 * @param src 移動元メモリ領域
 * @param n 移動するバイト数
 * @return dstのポインタ
 * @note 領域が重複している場合でも安全にコピー
 */
void* memmove(void* dst, const void* src, size_t n)
{
	char* d = (char*)dst;          /* 移動先ポインタ */
	const char* s = (const char*)src;  /* 移動元ポインタ */
	
	/* 移動方向を決定（前方から or 後方から） */
	if (d < s) {
		/* 前方からコピー（移動先が移動元より前） */
		while (n--)
			*d++ = *s++;
	} else {
		/* 後方からコピー（移動先が移動元より後） */
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	
	return dst;  /* 移動先のポインタを返す */
}