/*
 * p-kernel libc implementation
 * long型除算実装 (ldiv.c)
 * 
 * このファイルはldiv関数を実装します。
 * long型整数の除算を行い、商と余りを同時に返します。
 */

#include "stdlib.h"

/**
 * @brief long型整数除算（商と余りを返す）
 * @param numer 被除数
 * @param denom 除数
 * @return ldiv_t構造体（商と余りを含む）
 * @note 効率的な除算でlong型の商と余りを同時に取得
 */
ldiv_t ldiv(long int numer, long int denom)
{
	ldiv_t result;  /* 結果を格納する構造体 */
	
	/* 商と余りを計算 */
	result.quot = numer / denom;  /* 商 (quotient) */
	result.rem = numer % denom;   /* 余り (remainder) */
	
	return result;
}