/*
 * p-kernel libc implementation
 * 文字検索実装 (strchr.c)
 *
 * このファイルはstrchr関数を実装します。
 * 文字列から指定文字を検索します。
 */

#include "string.h"

/**
 * @brief 文字列から文字を検索
 * @param s 検索対象文字列
 * @param c 検索する文字 (intとして渡されるがcharに変換)
 * @return 見つかった位置のポインタ、見つからない場合はNULL
 * @note NULL終端文字('\0')も検索可能
 */
char* strchr(const char* s, int c)
{
	/* 文字列を走査して文字を検索 */
	while (*s) {
		if (*s == c)
			return (char*)s;  /* 文字が見つかった */
		s++;
	}
	
	/* NULL終端文字を検索する場合 */
	if (c == '\0')
		return (char*)s;
	
	return NULL;  /* 文字が見つからなかった */
}