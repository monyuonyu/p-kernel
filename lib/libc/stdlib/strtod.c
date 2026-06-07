/*
 * p-kernel libc implementation
 * 文字列からdouble変換実装 (strtod.c)
 *
 * このファイルはstrtod関数を実装します。
 * 文字列をdouble型浮動小数点数に変換します。
 */

#include "stdlib.h"
#include "ctype.h"

/**
 * @brief 文字列をdouble型浮動小数点数に変換
 * @param str 変換する文字列
 * @param endptr 変換終了位置を格納するポインタ
 * @return 変換されたdouble値
 * @note 空白、符号、整数部、小数部、指数部を処理
 */
double strtod(const char* str, char** endptr)
{
	const char* start = str;  /* 元の文字列ポインタを保存 */
	double result = 0.0;      /* 整数部の結果 */
	double fraction = 0.0;    /* 小数部の値 */
	int sign = 1;             /* 符号 (1:正, -1:負) */
	int exp_sign = 1;         /* 指数の符号 (1:正, -1:負) */
	int exp = 0;              /* 指数の値 */
	double divisor = 1.0;     /* 小数部の除数 */
	
	/* 先頭の空白文字をスキップ */
	while (isspace(*str))
		str++;
	
	/* 符号の処理 */
	if (*str == '-') {
		sign = -1;
		str++;
	} else if (*str == '+') {
		str++;
	}
	
	/* 整数部の処理 */
	int has_digits = 0;  /* 数字が存在したかフラグ */
	while (isdigit(*str)) {
		result = result * 10.0 + (*str - '0');
		str++;
		has_digits = 1;
	}
	
	/* 小数部の処理 */
	if (*str == '.') {
		str++;
		while (isdigit(*str)) {
			fraction = fraction * 10.0 + (*str - '0');
			divisor *= 10.0;
			str++;
			has_digits = 1;
		}
	}
	
	/* 数字が1つもない場合は変換失敗 */
	if (!has_digits) {
		if (endptr)
			*endptr = (char*)start;
		return 0.0;
	}
	
	/* 整数部と小数部を結合 */
	result = result + fraction / divisor;
	
	/* 指数部の処理 */
	if (*str == 'e' || *str == 'E') {
		str++;
		if (*str == '-') {
			exp_sign = -1;
			str++;
		} else if (*str == '+') {
			str++;
		}
		
		/* 指数値の取得 */
		while (isdigit(*str)) {
			exp = exp * 10 + (*str - '0');
			str++;
		}
		
		/* 指数の適用 */
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
	
	/* 変換終了位置を設定 */
	if (endptr)
		*endptr = (char*)str;
	
	/* 符号を適用して結果を返す */
	return sign * result;
}