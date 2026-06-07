/*
 * p-kernel libc implementation
 * 文字列比較実装 (strcmp.c)
 *
 * このファイルはstrcmp関数を実装します。
 * 2つの文字列を辞書順で比較します。
 */

#include "string.h"

/**
 * @brief 文字列の比較
 * @param s1 比較対象文字列1
 * @param s2 比較対象文字列2
 * @return 比較結果 (s1 > s2: 正の値, s1 < s2: 負の値, s1 == s2: 0)
 * @note 大文字小文字を区別して比較
 */
int strcmp(const char *s1, const char *s2)
{
    /* 文字が一致している間ループ */
    while (*s1 != '\0' && *s1 == *s2)
    {
      s1++;
      s2++;
    }
    
    /* 最初に異なる文字の差を返す */
    return (*(unsigned char *) s1) - (*(unsigned char *) s2);
}
