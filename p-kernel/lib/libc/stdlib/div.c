/*
 * p-kernel libc implementation
 * 整数除算実装 (div.c)
 * 
 * このファイルはdiv関数を実装します。
 * 整数の除算を行い、商と余りを同時に返します。
 */

#include "stdlib.h"

/**
 * @brief 整数除算（商と余りを返す）
 * @param numer 被除数
 * @param denom 除数
 * @return div_t構造体（商と余りを含む）
 * @note 効率的な除算で商と余りを同時に取得
 */
div_t div(int numer, int denom)
{
	div_t result;  /* 結果を格納する構造体 */
	
	/* 商と余りを計算 */
	result.quot = numer / denom;  /* 商 (quotient) */
	result.rem = numer % denom;   /* 余り (remainder) */
	
	return result;
}