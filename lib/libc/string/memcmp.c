/*
 * p-kernel libc implementation
 * メモリ比較実装 (memcmp.c)
 *
 * このファイルはmemcmp関数を実装します。
 * 2つのメモリ領域をバイト単位で比較します。
 */

#include "string.h"

/**
 * @brief メモリ領域の比較
 * @param m1 比較対象メモリ領域1
 * @param m2 比較対象メモリ領域2
 * @param n 比較するバイト数
 * @return 比較結果 (m1 > m2: 正の値, m1 < m2: 負の値, m1 == m2: 0)
 * @note 1バイトずつ比較し、最初に異なるバイトで差を返す
 */
int memcmp(const void *m1, const void *m2, size_t n)
{
    unsigned char *s1 = (unsigned char *) m1;  /* 比較対象1 */
    unsigned char *s2 = (unsigned char *) m2;  /* 比較対象2 */

    /* バイト単位で比較 */
    while (n--)
    {
        if (*s1 != *s2)
        {
            return *s1 - *s2;  /* 差異があった場合はその差を返す */
        }
        s1++;
        s2++;
    }
    return 0;  /* 全てのバイトが一致 */
}

