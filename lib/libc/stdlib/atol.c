/*
 * p-kernel libc implementation
 * long型数値変換実装 (atol.c)
 * 
 * このファイルはatol関数を実装します。
 * 文字列をlong型の整数に変換します。
 */

#include "stdlib.h"

/**
 * @brief 文字列をlong型整数に変換
 * @param str 変換する文字列
 * @return 変換されたlong型整数値
 * @note 空白文字をスキップし、符号を考慮した変換を行う
 */
long atol(const char* str)
{
	long result = 0;  /* 変換結果 */
	int sign = 1;     /* 符号（1:正、-1:負） */
	
	/* 先頭の空白文字をスキップ */
	while (*str == ' ' || *str == '\t')
		str++;
	
	/* 符号の処理 */
	if (*str == '-') {
		sign = -1;  /* 負の符号 */
		str++;
	} else if (*str == '+') {
		str++;      /* 正の符号（省略可能） */
	}
	
	/* 数字文字を順次変換 */
	while (*str >= '0' && *str <= '9') {
		result = result * 10 + (*str - '0');  /* 10進数として累積 */
		str++;
	}
	
	/* 符号を適用して結果を返す */
	return result * sign;
}