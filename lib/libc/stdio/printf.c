/*
 * p-kernel libc implementation
 * フォーマット出力実装 (printf.c)
 * 
 * このファイルはprintf系関数の実装を提供します。
 * %d, %i, %u, %x, %X, %c, %s, %p のフォーマット指定子をサポートします。
 */

#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "stddef.h"

/**
 * @brief 数値を指定された基数で文字列に変換
 * @param buf 出力バッファ
 * @param num 変換する数値
 * @param base 基数（2-16）
 * @param uppercase 大文字使用フラグ（16進数用）
 * @return 変換された文字数
 */
static int format_number(char* buf, unsigned long num, int base, int uppercase)
{
	char digits[] = "0123456789abcdef";  /* 小文字の数字文字 */
	char DIGITS[] = "0123456789ABCDEF";  /* 大文字の数字文字 */
	char* charset = uppercase ? DIGITS : digits;
	char temp[32];  /* 一時バッファ（最大32桁） */
	int i = 0;
	int len = 0;
	
	/* 0の特別処理 */
	if (num == 0) {
		buf[0] = '0';
		return 1;
	}
	
	/* 数値を逆順で一時バッファに格納 */
	while (num > 0) {
		temp[i++] = charset[num % base];
		num /= base;
	}
	
	/* 逆順に出力バッファにコピー */
	while (i > 0) {
		buf[len++] = temp[--i];
	}
	
	return len;
}

/**
 * @brief 可変引数フォーマット文字列出力（サイズ制限付き）
 * @param str 出力先文字列バッファ
 * @param size バッファサイズ
 * @param format フォーマット文字列
 * @param ap 可変引数リスト
 * @return 出力された文字数
 */
int vsnprintf(char* str, size_t size, const char* format, va_list ap)
{
	size_t pos = 0;          /* 現在の出力位置 */
	const char* p = format;  /* フォーマット文字列の現在位置 */
	
	/* フォーマット文字列を1文字ずつ処理 */
	while (*p && pos < size - 1) {
		/* 通常文字の場合はそのまま出力 */
		if (*p != '%') {
			str[pos++] = *p++;
			continue;
		}
		
		/* '%'が見つかった場合 */
		p++;
		if (*p == '%') {
			/* '%%' -> '%' */
			str[pos++] = '%';
			p++;
			continue;
		}
		
		switch (*p) {
			case 'd':
			case 'i': {
				int val = va_arg(ap, int);
				if (val < 0) {
					str[pos++] = '-';
					val = -val;
				}
				char num_buf[16];
				int len = format_number(num_buf, val, 10, 0);
				{
				int i;
				for (i = 0; i < len && pos < size - 1; i++)
					str[pos++] = num_buf[i];
				}
				break;
			}
			case 'u': {
				unsigned int val = va_arg(ap, unsigned int);
				char num_buf[16];
				int len = format_number(num_buf, val, 10, 0);
				{
				int i;
				for (i = 0; i < len && pos < size - 1; i++)
					str[pos++] = num_buf[i];
				}
				break;
			}
			case 'x': {
				unsigned int val = va_arg(ap, unsigned int);
				char num_buf[16];
				int len = format_number(num_buf, val, 16, 0);
				{
				int i;
				for (i = 0; i < len && pos < size - 1; i++)
					str[pos++] = num_buf[i];
				}
				break;
			}
			case 'X': {
				unsigned int val = va_arg(ap, unsigned int);
				char num_buf[16];
				int len = format_number(num_buf, val, 16, 1);
				{
				int i;
				for (i = 0; i < len && pos < size - 1; i++)
					str[pos++] = num_buf[i];
				}
				break;
			}
			case 'c': {
				char c = (char)va_arg(ap, int);
				str[pos++] = c;
				break;
			}
			case 's': {
				char* s = va_arg(ap, char*);
				if (!s) s = "(null)";
				while (*s && pos < size - 1)
					str[pos++] = *s++;
				break;
			}
			case 'p': {
				void* ptr = va_arg(ap, void*);
				str[pos++] = '0';
				str[pos++] = 'x';
				char num_buf[16];
				int len = format_number(num_buf, (unsigned long)ptr, 16, 0);
				{
				int i;
				for (i = 0; i < len && pos < size - 1; i++)
					str[pos++] = num_buf[i];
				}
				break;
			}
		}
		p++;
	}
	
	str[pos] = '\0';
	return pos;
}

int vsprintf(char* str, const char* format, va_list ap)
{
	return vsnprintf(str, SIZE_MAX, format, ap);
}

int sprintf(char* str, const char* format, ...)
{
	va_list ap;
	va_start(ap, format);
	int result = vsprintf(str, format, ap);
	va_end(ap);
	return result;
}

int snprintf(char* str, size_t size, const char* format, ...)
{
	va_list ap;
	va_start(ap, format);
	int result = vsnprintf(str, size, format, ap);
	va_end(ap);
	return result;
}