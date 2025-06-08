/*
 * p-kernel libc implementation
 * 三角関数実装 (trigonometry.c)
 * 
 * このファイルは三角関数（sin, cos, tan）と逆三角関数（asin, acos, atan）を実装します。
 * テイラー級数近似による高精度計算を行います。
 */

#include "math.h"

/* 数学定数の定義 */
#define PI 3.14159265358979323846      /* 円周率 */
#define PI_2 (PI / 2.0)               /* π/2 */

/**
 * @brief テイラー級数による正弦関数の近似計算
 * @param x 角度（ラジアン、-π/2 ≤ x ≤ π/2）
 * @return sin(x) の近似値
 * @note テイラー級数: sin(x) = x - x³/3! + x⁵/5! - x⁷/7! + ...
 */
static double polynomial_sin(double x)
{
	double x2 = x * x;  /* x² */
	/* テイラー級数の最初の4項による近似計算 */
	return x * (1.0 - x2 * (1.0/6.0 - x2 * (1.0/120.0 - x2 * (1.0/5040.0))));
}

/**
 * @brief テイラー級数による余弦関数の近似計算
 * @param x 角度（ラジアン、-π/2 ≤ x ≤ π/2）
 * @return cos(x) の近似値
 * @note テイラー級数: cos(x) = 1 - x²/2! + x⁴/4! - x⁶/6! + ...
 */
static double polynomial_cos(double x)
{
	double x2 = x * x;  /* x² */
	/* テイラー級数の最初の4項による近似計算 */
	return 1.0 - x2 * (1.0/2.0 - x2 * (1.0/24.0 - x2 * (1.0/720.0)));
}

/**
 * @brief 正弦関数（サイン）
 * @param x 角度（ラジアン）
 * @return x の正弦値
 * @note 入力値を[-π, π]の範囲に正規化してから計算
 */
double sin(double x)
{
	/* 入力値を 2π の範囲に正規化 */
	x = fmod(x, 2.0 * PI);
	if (x > PI) x -= 2.0 * PI;
	if (x < -PI) x += 2.0 * PI;
	
	/* 範囲に応じて適切な近似関数を選択 */
	if (x > PI_2) return polynomial_cos(x - PI_2);     /* 第1象限 */
	if (x < -PI_2) return -polynomial_cos(x + PI_2);   /* 第3象限 */
	
	/* [-π/2, π/2] の範囲では直接テイラー級数を使用 */
	return polynomial_sin(x);
}

/**
 * @brief 余弦関数（コサイン）
 * @param x 角度（ラジアン）
 * @return x の余弦値
 * @note cos(x) = sin(x + π/2) の性質を利用
 */
double cos(double x)
{
	return sin(x + PI_2);
}

/**
 * @brief 正接関数（タンジェント）
 * @param x 角度（ラジアン）
 * @return x の正接値
 * @note tan(x) = sin(x) / cos(x)、ゼロ除算のチェックあり
 */
double tan(double x)
{
	double cos_x = cos(x);
	/* ゼロ除算を防ぐためのチェック */
	if (fabs(cos_x) < 1e-15) return 0.0;
	return sin(x) / cos_x;
}

/**
 * @brief 逆正弦関数（アークサイン）
 * @param x 入力値（-1.0 ≤ x ≤ 1.0）
 * @return x の逆正弦（ラジアン、-π/2 ≤ 戻り値 ≤ π/2）
 * @note テイラー級数による近似計算
 */
double asin(double x)
{
	/* 定義域のチェック */
	if (x < -1.0 || x > 1.0) return 0.0;
	/* 特別値の処理 */
	if (fabs(x) == 1.0) return x * PI_2;
	
	/* テイラー級数: asin(x) = x + x³/6 + 3x⁵/40 + ... */
	return x + x*x*x/6.0 + 3.0*x*x*x*x*x/40.0;
}

/**
 * @brief 逆余弦関数（アークコサイン）
 * @param x 入力値（-1.0 ≤ x ≤ 1.0）
 * @return x の逆余弦（ラジアン、0 ≤ 戻り値 ≤ π）
 * @note acos(x) = π/2 - asin(x) の性質を利用
 */
double acos(double x)
{
	return PI_2 - asin(x);
}

/**
 * @brief 逆正接関数（アークタンジェント）
 * @param x 入力値
 * @return x の逆正接（ラジアン、-π/2 < 戻り値 < π/2）
 * @note 大きな値に対しては逆数を使用して精度を改善
 */
double atan(double x)
{
	/* |x| > 1 の場合は逆数を使用 */
	if (fabs(x) > 1.0) return PI_2 - atan(1.0/x);
	/* テイラー級数: atan(x) = x - x³/3 + x⁵/5 - ... */
	return x - x*x*x/3.0 + x*x*x*x*x/5.0;
}

/**
 * @brief 2引数逆正接関数
 * @param y y座標
 * @param x x座標
 * @return 点(x,y)の偏角（ラジアン、-π ≤ 戻り値 ≤ π）
 * @note 各象限を考慮した適切な角度を返す
 */
double atan2(double y, double x)
{
	/* 第1象限と第4象限 */
	if (x > 0.0) return atan(y/x);
	/* 第2象限 */
	if (x < 0.0 && y >= 0.0) return atan(y/x) + PI;
	/* 第3象限 */
	if (x < 0.0 && y < 0.0) return atan(y/x) - PI;
	/* y軸正の方向 */
	if (x == 0.0 && y > 0.0) return PI_2;
	/* y軸負の方向 */
	if (x == 0.0 && y < 0.0) return -PI_2;
	/* 原点 */
	return 0.0;
}

/* float版の三角関数群 - double版をキャストして実装 */
float sinf(float x) { return (float)sin((double)x); }          /* float版正弦 */
float cosf(float x) { return (float)cos((double)x); }          /* float版余弦 */
float tanf(float x) { return (float)tan((double)x); }          /* float版正接 */
float asinf(float x) { return (float)asin((double)x); }        /* float版逆正弦 */
float acosf(float x) { return (float)acos((double)x); }        /* float版逆余弦 */
float atanf(float x) { return (float)atan((double)x); }        /* float版逆正接 */
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); } /* float版2引数逆正接 */