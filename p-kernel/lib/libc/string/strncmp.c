/*
 * p-kernel libc implementation
 * 文字列比較実装 (strncmp.c)
 *
 * このファイルはstrncmp関数を実装します。
 * 文字列を指定された長さだけ比較します。
 */

#include "string.h"

/**
 * @brief 文字列を指定長だけ比較
 * @param s1 比較対象文字列1
 * @param s2 比較対象文字列2
 * @param n 比較する最大文字数
 * @return 比較結果 (s1 > s2: 正の値, s1 < s2: 負の値, s1 == s2: 0)
 * @note 大文字小文字を区別して比較
 */
int strncmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0)
        return 0;

    while (n-- != 0 && *s1 == *s2)
    {
        if (n == 0 || *s1 == '\0')
            break;
        s1++;
        s2++;
    }

    return (*(unsigned char *) s1) - (*(unsigned char *) s2);
}
