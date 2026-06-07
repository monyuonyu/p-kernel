/*
 * p-kernel libc implementation
 * 絶対値計算実装 (fabs.c)
 *
 * このファイルは浮動小数点数の絶対値を計算する関数を実装します。
 * 負の値を正の値に変換します。
 */

#include "math.h"

/**
 * @brief 倍精度浮動小数点数の絶対値
 * @param x 入力値
 * @return xの絶対値
 */
double fabs(double x)
{
	return x < 0.0 ? -x : x;
}

/**
 * @brief 単精度浮動小数点数の絶対値
 * @param x 入力値
 * @return xの絶対値
 */
float fabsf(float x)
{
	return x < 0.0f ? -x : x;
}