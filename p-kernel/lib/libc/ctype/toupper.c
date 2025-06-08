/*
 * p-kernel libc implementation
 * 大文字変換実装 (toupper.c)
 * 
 * このファイルはtoupper関数を実装します。
 * ASCII文字の小文字を大文字に変換します。
 */

#include "ctype.h"

/**
 * @brief 小文字を大文字に変換
 * @param c 変換する文字
 * @return 大文字に変換された文字、変換不要な場合はそのまま
 * @note ASCII範囲内の小文字(a-z)のみを大文字(A-Z)に変換
 */
int toupper(int c)
{
	/* ASCII小文字の範囲チェック (a-z: 0x61-0x7A) */
	if(c >= 'a' && c <= 'z')
		return c - 0x20;  /* 0x20(32)を減算して大文字に変換 */

	return c;  /* 小文字以外はそのまま返す */
}
