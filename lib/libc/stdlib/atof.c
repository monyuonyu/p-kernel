/*
 * p-kernel libc implementation
 * 浮動小数点数変換実装 (atof.c)
 * 
 * このファイルはatof関数を実装します。
 * 文字列をdouble型の浮動小数点数に変換します。
 */

#include "stdlib.h"
#include "ctype.h"

/**
 * @brief 文字列をdouble型浮動小数点数に変換
 * @param str 変換する文字列
 * @return 変換されたdouble型浮動小数点数
 * @note 空白、符号、整数部、小数部、指数部を処理
 */
double atof(const char* str)
{
	double result = 0.0;
	double fraction = 0.0;
	int sign = 1;
	int exp_sign = 1;
	int exp = 0;
	double divisor = 1.0;
	
	while (isspace(*str))
		str++;
	
	if (*str == '-') {
		sign = -1;
		str++;
	} else if (*str == '+') {
		str++;
	}
	
	while (isdigit(*str)) {
		result = result * 10.0 + (*str - '0');
		str++;
	}
	
	if (*str == '.') {
		str++;
		while (isdigit(*str)) {
			fraction = fraction * 10.0 + (*str - '0');
			divisor *= 10.0;
			str++;
		}
	}
	
	result = result + fraction / divisor;
	
	if (*str == 'e' || *str == 'E') {
		str++;
		if (*str == '-') {
			exp_sign = -1;
			str++;
		} else if (*str == '+') {
			str++;
		}
		
		while (isdigit(*str)) {
			exp = exp * 10 + (*str - '0');
			str++;
		}
		
		{
		int i;
		for (i = 0; i < exp; i++) {
			if (exp_sign == 1)
				result *= 10.0;
			else
				result /= 10.0;
		}
		}
	}
	
	return sign * result;
}