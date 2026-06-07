/*
 * p-kernel libc implementation
 * クイックソート実装 (qsort.c)
 *
 * このファイルはqsort関数を実装します。
 * 再帰的なクイックソートアルゴリズムを使用して配列をソートします。
 */

#include "stdlib.h"
#include "string.h"

/**
 * @brief 要素の入れ替え
 * @param a 要素1へのポインタ
 * @param b 要素2へのポインタ
 * @param size 要素のサイズ
 */
static void swap(void* a, void* b, size_t size)
{
	char* ca = (char*)a;
	char* cb = (char*)b;
	char temp;
	
	/* 1バイトずつ入れ替え */
	{
	size_t i;
	for (i = 0; i < size; i++) {
		temp = ca[i];
		ca[i] = cb[i];
		cb[i] = temp;
	}
	}
}

/**
 * @brief パーティション分割
 * @param base 配列の先頭ポインタ
 * @param nmemb 配列の要素数
 * @param size 各要素のサイズ
 * @param compar 比較関数
 * @return ピボット要素のポインタ
 */
static void* partition(void* base, size_t nmemb, size_t size,
                      int (*compar)(const void*, const void*))
{
	char* cbase = (char*)base;
	char* pivot = cbase + (nmemb - 1) * size;  /* 最後の要素をピボットに選択 */
	size_t i = 0;  /* 分割位置 */
	
	/* ピボットより小さい要素を左側に移動 */
	{
	size_t j;
	for (j = 0; j < nmemb - 1; j++) {
		if (compar(cbase + j * size, pivot) <= 0) {
			swap(cbase + i * size, cbase + j * size, size);
			i++;
		}
	}
	}
	
	/* ピボットを正しい位置に移動 */
	swap(cbase + i * size, pivot, size);
	return cbase + i * size;
}

/**
 * @brief クイックソート
 * @param base ソートする配列の先頭ポインタ
 * @param nmemb 配列の要素数
 * @param size 各要素のサイズ
 * @param compar 比較関数
 * @note 再帰的にソートを行う
 */
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*))
{
	if (nmemb <= 1)
		return;  /* 要素数が1以下ならソート不要 */
	
	char* cbase = (char*)base;
	char* pivot = (char*)partition(base, nmemb, size, compar);
	size_t pivot_idx = (pivot - cbase) / size;
	
	/* 再帰的に左右の部分配列をソート */
	qsort(base, pivot_idx, size, compar);
	qsort(pivot + size, nmemb - pivot_idx - 1, size, compar);
}