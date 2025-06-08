/*
 * p-kernel libc implementation
 * 文字集合検索実装 (strcspn.c)
 *
 * このファイルはstrcspn関数を実装します。
 * 文字列から指定文字集合に含まれる文字を検索します。
 */

#include "string.h"

/**
 * @brief 文字集合に含まれる文字を検索
 * @param s1 検索対象文字列
 * @param s2 検索文字集合
 * @return 見つかった位置までの文字数 (見つからない場合は文字列長)
 * @note 文字集合(s2)に含まれる最初の文字の位置を返す
 */
size_t strcspn(const char* s1, const char* s2)
{
	const char* p1 = s1;  /* 検索対象文字列のポインタ */
	const char* p2;       /* 文字集合のポインタ */
	
	/* 文字列を1文字ずつチェック */
	while (*p1) {
		/* 文字集合をチェック */
		for (p2 = s2; *p2; p2++) {
			if (*p1 == *p2)
				return p1 - s1;  /* 一致した文字の位置を返す */
		}
		p1++;
	}
	
	/* 一致する文字が見つからなかった場合 */
	return p1 - s1;  /* 文字列全体の長さを返す */
}