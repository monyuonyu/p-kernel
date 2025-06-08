/*
 * p-kernel libc implementation
 * 指数・対数関数実装 (exponential.c)
 * 
 * このファイルは指数関数、対数関数、べき乗関数を実装します。
 * テイラー級数展開や繰り返し計算による高精度計算を行います。
 */

#include "math.h"

/* 数学定数の定義 */
#define E 2.71828182845904523536      /* ネイピア数 */

/**
 * @brief 指数関数（e^x）
 * @param x 指数
 * @return e の x 乗
 * @note テイラー級数展開: e^x = 1 + x + x²/2! + x³/3! + ...
 */
double exp(double x)
{
	/* 特別値の処理 */
	if (x == 0.0) return 1.0;
	/* 負の値は逆数を使用して計算 */
	if (x < 0.0) return 1.0 / exp(-x);
	
	double result = 1.0;  /* 結果の累積 */
	double term = 1.0;    /* 各項の値 */
	
	/* テイラー級数の20項までを計算 */
	{
	int i;
	for (i = 1; i < 20; i++) {
		term *= x / i;  /* term = x^i / i! */
		result += term;
	}
	}
	
	return result;
}

/**
 * @brief 自然対数（ln(x)）
 * @param x 入力値（x > 0）
 * @return x の自然対数
 * @note 繰り返し関数: ln(x) = 2 * Σ(y^(2n+1)/(2n+1)), y = (x-1)/(x+1)
 */
double log(double x)
{
	/* 定義域のチェック */
	if (x <= 0.0) return 0.0;
	/* 特別値の処理 */
	if (x == 1.0) return 0.0;
	
	double result = 0.0;
	double y = (x - 1.0) / (x + 1.0);  /* 変数変換で収束を改善 */
	double y2 = y * y;                 /* y² */
	double term = y;                   /* 各項の値 */
	
	/* 繰り返し関数の20項までを計算 */
	{
	int i;
	for (i = 1; i < 20; i += 2) {
		result += term / i;  /* y^i / i を累積 */
		term *= y2;          /* 次の項へ */
	}
	}
	
	return 2.0 * result;
}

/**
 * @brief 常用対数（log10(x)）
 * @param x 入力値（x > 0）
 * @return x の常用対数（底10の対数）
 * @note log10(x) = ln(x) / ln(10) を利用
 */
double log10(double x)
{
	return log(x) / log(10.0);
}

/**
 * @brief べき乗関数（x^y）
 * @param x 底
 * @param y 指数
 * @return x の y 乗
 * @note x^y = e^(y * ln(|x|)) を利用、特別値とエラーケースを処理
 */
double pow(double x, double y)
{
	/* 特別値の処理 */
	if (y == 0.0) return 1.0;          /* 任意の数の0乗は1 */
	if (x == 0.0) return 0.0;          /* 0の任意乗は0 */
	if (y == 1.0) return x;            /* 任意の数の1乗はその数自身 */
	/* 負の底に対して非整数指数はエラー */
	if (x < 0.0 && y != floor(y)) return 0.0;
	
	/* x^y = e^(y * ln(|x|)) を使用 */
	return exp(y * log(fabs(x)));
}

/**
 * @brief 浮動小数点数の分解（仮数部と指数部に分離）
 * @param value 分解する値
 * @param exp 指数部を格納するポインタ
 * @return 仮数部（0.5 ≤ |戻り値| < 1.0）
 * @note value = 仮数部 × 2^指数部 の形に分解
 */
double frexp(double value, int* exp)
{
	/* 0の特別処理 */
	if (value == 0.0) {
		*exp = 0;
		return 0.0;
	}
	
	*exp = 0;
	double abs_value = fabs(value);
	
	/* 1.0以上の場合は2で割り続ける */
	while (abs_value >= 1.0) {
		abs_value /= 2.0;
		(*exp)++;
	}
	
	/* 0.5未満の場合は2をかけ続ける */
	while (abs_value < 0.5) {
		abs_value *= 2.0;
		(*exp)--;
	}
	
	/* 元の符号を保持 */
	return value < 0.0 ? -abs_value : abs_value;
}

/**
 * @brief 指数部の設定（x × 2^exp）
 * @param x 仮数部
 * @param exp 指数部
 * @return x × 2^exp
 * @note シフト演算の代わりに乘除算で実装
 */
double ldexp(double x, int exp)
{
	/* 正の指数の場合は2をかける */
	while (exp > 0) {
		x *= 2.0;
		exp--;
	}
	/* 負の指数の場合は2で割る */
	while (exp < 0) {
		x /= 2.0;
		exp++;
	}
	return x;
}

/**
 * @brief 浮動小数点数の整数部と小数部への分離
 * @param value 分離する値
 * @param iptr 整数部を格納するポインタ
 * @return 小数部
 * @note value = 整数部 + 小数部 の形に分離
 */
double modf(double value, double* iptr)
{
	*iptr = floor(value);  /* 整数部は床関数で取得 */
	return value - *iptr;  /* 小数部は元の値から整数部を引いたもの */
}

/* float版の指数・対数関数群 - double版をキャストして実装 */
float expf(float x) { return (float)exp((double)x); }          /* float版指数関数 */
float logf(float x) { return (float)log((double)x); }          /* float版自然対数 */
float log10f(float x) { return (float)log10((double)x); }      /* float版常用対数 */
float powf(float x, float y) { return (float)pow((double)x, (double)y); } /* float版べき乗 */
float frexpf(float value, int* exp) { return (float)frexp((double)value, exp); } /* float版数値分解 */
float ldexpf(float x, int exp) { return (float)ldexp((double)x, exp); } /* float版指数部設定 */
/**
 * @brief float版整数部・小数部分離
 * @param value 分離する値
 * @param iptr 整数部を格納するポインタ
 * @return 小数部
 */
float modff(float value, float* iptr) {
	double diptr;  /* double版の一時変数 */
	float result = (float)modf((double)value, &diptr);
	*iptr = (float)diptr;  /* doubleからfloatに変換 */
	return result;
}