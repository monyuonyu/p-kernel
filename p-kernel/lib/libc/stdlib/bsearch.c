/*
 * p-kernel libc implementation
 * 二分探索実装 (bsearch.c)
 *
 * このファイルは二分探索(binary search)関数を実装します。
 * ソート済み配列から効率的に要素を検索します。
 */

#include "stdlib.h"

/**
 * @brief 二分探索
 * @param key 検索する要素へのポインタ
 * @param base 検索対象の配列
 * @param nmemb 配列の要素数
 * @param size 各要素のサイズ
 * @param compar 比較関数
 * @return 見つかった要素へのポインタ、見つからない場合はNULL
 * @note 配列はcompar関数でソート済みである必要がある
 */
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*))
{
	const char* cbase = (const char*)base;  /* 要素アクセスのためのポインタ */
	size_t left = 0;                        /* 検索範囲の左端 */
	size_t right = nmemb;                   /* 検索範囲の右端 */
	
	/* 二分探索ループ */
	while (left < right) {
		size_t mid = left + (right - left) / 2;  /* 中央位置計算 */
		const void* mid_elem = cbase + mid * size;  /* 中央要素へのポインタ */
		int cmp = compar(key, mid_elem);  /* 比較関数で判定 */
		
		if (cmp == 0)
			return (void*)mid_elem;  /* 一致した要素を返す */
		else if (cmp < 0)
			right = mid;  /* 左半分を検索 */
		else
			left = mid + 1;  /* 右半分を検索 */
	}
	
	return NULL;  /* 要素が見つからなかった */
}