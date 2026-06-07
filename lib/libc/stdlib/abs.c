/*
 * p-kernel libc implementation
 * 整数絶対値実装 (abs.c)
 * 
 * このファイルはabs関数を実装します。
 * 整数の絶対値を返します。
 */

#include "stdlib.h"

/**
 * @brief 整数の絶対値
 * @param x 入力値
 * @return x の絶対値
 * @note 負の値の場合は符号を反転、正の値はそのまま
 */
int abs(int x)
{
	/* 負の値の場合は符号を反転、正の値はそのまま返す */
	return x < 0 ? -x : x;
}