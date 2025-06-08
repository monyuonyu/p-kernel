/*
 * p-kernel libc implementation
 * 平方根計算実装 (sqrt.c)
 *
 * このファイルは平方根計算関数を実装します。
 * ニュートン法を使用して高精度な平方根を計算します。
 */

#include "math.h"

/**
 * @brief 倍精度浮動小数点数の平方根
 * @param x 入力値（x >= 0）
 * @return xの平方根
 * @note xが負の場合は0を返す
 */
double sqrt(double x)
{
	if (x < 0.0)
		return 0.0;
	
	if (x == 0.0)
		return 0.0;
	
	double guess = x / 2.0;
	double prev_guess = 0.0;
	
	while (fabs(guess - prev_guess) > 1e-10) {
		prev_guess = guess;
		guess = (guess + x / guess) / 2.0;
	}
	
	return guess;
}

/**
 * @brief 単精度浮動小数点数の平方根
 * @param x 入力値（x >= 0）
 * @return xの平方根
 * @note xが負の場合は0を返す
 */
float sqrtf(float x)
{
	if (x < 0.0f)
		return 0.0f;
	
	if (x == 0.0f)
		return 0.0f;
	
	float guess = x / 2.0f;
	float prev_guess = 0.0f;
	
	while (fabsf(guess - prev_guess) > 1e-6f) {
		prev_guess = guess;
		guess = (guess + x / guess) / 2.0f;
	}
	
	return guess;
}