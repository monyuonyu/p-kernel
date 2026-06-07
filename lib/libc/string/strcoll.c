/*
 * p-kernel libc implementation
 * ロケール対応文字列比較実装 (strcoll.c)
 *
 * このファイルはstrcoll関数を実装します。
 * ロケールを考慮した文字列比較を行います。
 * 現在は簡易実装でstrcmpと同じ動作です。
 */

#include "string.h"

/**
 * @brief ロケールを考慮した文字列比較
 * @param s1 比較対象文字列1
 * @param s2 比較対象文字列2
 * @return 比較結果 (s1 > s2: 正の値, s1 < s2: 負の値, s1 == s2: 0)
 * @note 現在はstrcmpと同じ実装 (ロケール対応は未実装)
 */
int strcoll(const char* s1, const char* s2)
{
	return strcmp(s1, s2);  /* 現在は単純な文字列比較 */
}