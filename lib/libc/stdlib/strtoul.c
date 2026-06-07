/*
 * p-kernel libc implementation
 * 文字列からunsigned long変換実装 (strtoul.c)
 *
 * このファイルはstrtoul関数を実装します。
 * 文字列をunsigned long型整数に変換します。
 */

#include "stdlib.h"
#include "ctype.h"

/**
 * @brief 文字列をunsigned long型整数に変換
 * @param str 変換する文字列
 * @param endptr 変換終了位置を格納するポインタ
 * @param base 基数 (2-36、0の場合は自動判定)
 * @return 変換されたunsigned long値
 * @note 基数の自動判定: 0xで始まれば16進数、0で始まれば8進数、それ以外は10進数
 */
unsigned long int strtoul(const char* str, char** endptr, int base)
{
	const char* start = str;  /* 元の文字列ポインタを保存 */
	unsigned long int result = 0;  /* 変換結果 */
	int digit;  /* 現在の桁の値 */
	
	/* 先頭の空白文字をスキップ */
	while (isspace(*str))
		str++;
	
	/* 基数(base)の自動判定 */
	if (base == 0) {
		if (*str == '0') {
			str++;
			if (*str == 'x' || *str == 'X') {
				str++;
				base = 16;  /* 0xで始まる場合は16進数 */
			} else {
				base = 8;   /* 0で始まる場合は8進数 */
				str--;
			}
		} else {
			base = 10;  /* それ以外は10進数 */
		}
	} else if (base == 16) {
		/* 16進数で0x接頭辞がある場合はスキップ */
		if (*str == '0' && (str[1] == 'x' || str[1] == 'X'))
			str += 2;
	}
	
	/* 基数の範囲チェック */
	if (base < 2 || base > 36) {
		if (endptr)
			*endptr = (char*)start;
		return 0;
	}
	
	/* 文字列を走査して数値に変換 */
	while (*str) {
		/* 数字文字を数値に変換 */
		if (isdigit(*str))
			digit = *str - '0';
		/* アルファベット文字を数値に変換 (a-z/A-Z -> 10-35) */
		else if (isalpha(*str))
			digit = tolower(*str) - 'a' + 10;
		else
			break;  /* 数字でもアルファベットでもない文字で終了 */
		
		/* 基数以上の値は無効 */
		if (digit >= base)
			break;
		
		/* 結果を更新 */
		result = result * base + digit;
		str++;
	}
	
	/* 変換終了位置を設定 */
	if (endptr)
		*endptr = (char*)str;
	
	return result;
}