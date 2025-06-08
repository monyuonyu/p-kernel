/*
 * p-kernel libc implementation
 * 浮動小数点剰余演算実装 (fmod.c)
 *
 * このファイルは浮動小数点数の剰余演算(fmod)を実装します。
 * 除算の余りを計算します。
 */

#include "math.h"

/**
 * @brief 倍精度浮動小数点剰余演算
 * @param x 被除数
 * @param y 除数
 * @return xをyで割った余り
 * @note yが0の場合は0を返す
 */
double fmod(double x, double y)
{
	if (y == 0.0) return 0.0;
	
	double quotient = x / y;
	double integer_part = floor(quotient);
	
	return x - integer_part * y;
}

/**
 * @brief 単精度浮動小数点剰余演算
 * @param x 被除数
 * @param y 除数
 * @return xをyで割った余り
 */
float fmodf(float x, float y)
{
	return (float)fmod((double)x, (double)y);
}