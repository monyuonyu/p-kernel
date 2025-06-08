/*
 * p-kernel libc implementation
 * 双曲線関数実装 (hyperbolic.c)
 *
 * このファイルは双曲線関数(sinh, cosh, tanh)を実装します。
 * 指数関数を使用して双曲線関数を計算します。
 */

#include "math.h"

/**
 * @brief 双曲線正弦関数
 * @param x 入力値
 * @return xの双曲線正弦値
 */
double sinh(double x)
{
	return (exp(x) - exp(-x)) / 2.0;
}

/**
 * @brief 双曲線余弦関数
 * @param x 入力値
 * @return xの双曲線余弦値
 */
double cosh(double x)
{
	return (exp(x) + exp(-x)) / 2.0;
}

/**
 * @brief 双曲線正接関数
 * @param x 入力値
 * @return xの双曲線正接値
 */
double tanh(double x)
{
	double exp_x = exp(x);
	double exp_neg_x = exp(-x);
	return (exp_x - exp_neg_x) / (exp_x + exp_neg_x);
}

/* float版双曲線関数群 - double版をキャストして実装 */
float sinhf(float x) { return (float)sinh((double)x); }  /* float版双曲線正弦 */
float coshf(float x) { return (float)cosh((double)x); }  /* float版双曲線余弦 */
float tanhf(float x) { return (float)tanh((double)x); }  /* float版双曲線正接 */