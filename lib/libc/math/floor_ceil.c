/*
 * p-kernel libc implementation
 * 床関数・天井関数実装 (floor_ceil.c)
 *
 * このファイルは床関数(floor)と天井関数(ceil)を実装します。
 * 浮動小数点数を整数方向に丸める関数群です。
 */

#include "math.h"

/**
 * @brief 倍精度床関数
 * @param x 入力値
 * @return x以下の最大整数
 */
double floor(double x)
{
	if (x >= 0.0) {
		return (double)((long)x);
	} else {
		long i = (long)x;
		return (x == (double)i) ? x : (double)(i - 1);
	}
}

/**
 * @brief 倍精度天井関数
 * @param x 入力値
 * @return x以上の最小整数
 */
double ceil(double x)
{
	if (x >= 0.0) {
		long i = (long)x;
		return (x == (double)i) ? x : (double)(i + 1);
	} else {
		return (double)((long)x);
	}
}

/**
 * @brief 単精度床関数
 * @param x 入力値
 * @return x以下の最大整数
 */
float floorf(float x)
{
	if (x >= 0.0f) {
		return (float)((long)x);
	} else {
		long i = (long)x;
		return (x == (float)i) ? x : (float)(i - 1);
	}
}

/**
 * @brief 単精度天井関数
 * @param x 入力値
 * @return x以上の最小整数
 */
float ceilf(float x)
{
	if (x >= 0.0f) {
		long i = (long)x;
		return (x == (float)i) ? x : (float)(i + 1);
	} else {
		return (float)((long)x);
	}
}