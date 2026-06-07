/*
 * p-kernel libc implementation
 * 擬似乱数生成実装 (rand.c)
 * 
 * このファイルはrandとsrand関数を実装します。
 * 線形合同法を使用した擬似乱数生成を行います。
 */

#include "stdlib.h"

/* 乱数生成器の内部状態 */
static unsigned long int next = 1;

/**
 * @brief 擬似乱数生成
 * @return 0からRAND_MAXまでの擬似乱数
 * @note 線形合同法 (LCG) を使用した乱数生成
 */
int rand(void)
{
	/* 線形合同法: next = (a * next + c) mod m */
	next = next * 1103515245 + 12345;  /* a=1103515245, c=12345 */
	
	/* 上位ビットを除去してRAND_MAX範囲に制限 */
	return (unsigned int)(next / 65536) % (RAND_MAX + 1);
}

/**
 * @brief 乱数生成器のシード設定
 * @param seed 初期値（シード）
 * @note 同じシードで初期化すると同じ乱数列が生成される
 */
void srand(unsigned int seed)
{
	next = seed;  /* 内部状態をシード値で初期化 */
}