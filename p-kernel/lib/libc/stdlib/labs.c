/*
 * p-kernel libc implementation
 * long型絶対値実装 (labs.c)
 * 
 * このファイルはlabs関数を実装します。
 * long型整数の絶対値を返します。
 */

#include "stdlib.h"

/**
 * @brief long型整数の絶対値
 * @param x 入力値
 * @return x のlong型絶対値
 * @note 負の値の場合は符号を反転、正の値はそのまま
 */
long int labs(long int x)
{
	/* 負の値の場合は符号を反転、正の値はそのまま返す */
	return x < 0 ? -x : x;
}