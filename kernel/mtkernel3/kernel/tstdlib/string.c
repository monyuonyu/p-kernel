/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.00
 *
 *    Copyright (C) 2006-2019 by Ken Sakamura.
 *    This software is distributed under the T-License 2.1.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2019/12/11.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	string.c
 * @brief	T-Kernel 標準ライブラリ（メモリ・文字列操作）
 *
 * カーネル内部で使用するメモリ操作関数（knl_memset / knl_memcpy）と
 * 文字列操作関数（knl_strlen / knl_strcpy / knl_strncpy / knl_strcmp /
 * knl_strcat）を提供します。標準 C ライブラリに依存しないための
 * 独自実装です。
 */

#include <tk/tkernel.h>
#include "kernel.h"

/*** メモリ操作 ***/
/**
 * @brief	メモリ領域の埋め立て
 *
 * s から n バイトの領域を値 c（下位 1 バイト）で埋めます。
 * 8 バイト以上の場合はワード境界まで整列後、ワード単位で書き込んで
 * 高速化します。
 *
 * @param s	埋め立て対象領域の先頭アドレス
 * @param c	埋める値（下位 1 バイトのみ使用）
 * @param n	埋めるバイト数
 *
 * @return s をそのまま返します。
 */
void* knl_memset( void *s, int c, SZ n )
{
	register unsigned char *cp, cval;
	register unsigned long *lp, lval;

	cp = (unsigned char *)s;
	cval = (unsigned char)c;

	if (n < 8) {
		while (n-- > 0) {
			*cp++ = cval;
		}
		return s;
	}

	/* p-kernel 変更（LP64 対応）: 元のコードはワード長を 4 バイト
	 * 固定と仮定していたが、unsigned long は LP64 ホストでは 8 バイト。
	 * 「8 バイト書いて n を 4 減らす」ことになり、要求サイズの約 2 倍を
	 * 塗り潰して呼び出し元のスタックを破壊していた。整列・埋め値・
	 * 減算をすべて sizeof(unsigned long) 基準に統一する。
	 * （32bit MCU では従来と同一の動作） */
	while ((long)cp % (long)sizeof(unsigned long)) {
		--n;
		*cp++ = cval;
	}

	lp = (unsigned long *)cp;
	lval = (unsigned long)cval |
		(unsigned long)cval << 8 |
		(unsigned long)cval << 16 |
		(unsigned long)cval << 24;
	if ( sizeof(unsigned long) > 4 ) {
		/* 64bit long: 上位 32bit にも埋め値を複製
		 * （<<16<<16 の 2 段シフトは 32bit long での UB 回避） */
		lval |= lval << 16 << 16;
	}

	while (n >= (SZ)sizeof(unsigned long)) {
		*lp++ = lval;
		n -= (SZ)sizeof(unsigned long);
	}

	cp = (unsigned char *)lp;
	while (n) {
		*cp++ = cval;
		--n;
	}

	return s;
}

/**
 * @brief	メモリ領域のコピー
 *
 * src から dst へ n バイトをコピーします。
 *
 * @param dst	コピー先の先頭アドレス
 * @param src	コピー元の先頭アドレス
 * @param n	コピーするバイト数
 *
 * @return dst をそのまま返します。
 *
 * @note バイト単位の前方コピーのため、領域が重なる場合の動作は
 *	保証されません。
 */
void* knl_memcpy( void *dst, const void *src, SZ n )
{
	register unsigned char *cdst, *csrc;

	cdst = (unsigned char *)dst;
	csrc = (unsigned char *)src;
	while (n-- > 0) {
		*cdst++ = *csrc++;
	}

	return dst;
}

/**
 * @brief	文字列長の取得
 *
 * @param s	対象文字列（'\0' 終端）
 *
 * @return 終端の '\0' を含まない文字列長。
 */
SZ knl_strlen( const char *s )
{
	register char *cp;

	cp = (char *)s;
	while (*cp) {
		++cp;
	}
	return (SZ)(cp - s);
}

/**
 * @brief	文字列のコピー
 *
 * src の文字列を終端の '\0' を含めて dst へコピーします。
 *
 * @param dst	コピー先バッファ
 * @param src	コピー元文字列（'\0' 終端）
 *
 * @return dst をそのまま返します。
 */
char* knl_strcpy( char *dst, const char *src )
{
	register char *cp;

	cp = dst;
	do {
		*cp++ = *src;
	} while (*src++);

	return dst;
}

/**
 * @brief	最大長を指定した文字列のコピー
 *
 * src の文字列を最大 n 文字まで dst へコピーします。src が n 文字より
 * 短い場合、残りは '\0' で埋めます。
 *
 * @param dst	コピー先バッファ
 * @param src	コピー元文字列（'\0' 終端）
 * @param n	コピーする最大文字数
 *
 * @return dst をそのまま返します。
 *
 * @note src の長さが n 以上の場合、dst は '\0' 終端されません。
 */
char* knl_strncpy( char *dst, const char *src, SZ n )
{
	register char *cp;

	cp = dst;
	do {
		if (n-- <= 0) {
			return dst;
		}
		*cp++ = *src;
	} while (*src++);

	while (n-- > 0) {
		*cp++ = 0;
	}

	return dst;
}

/**
 * @brief	文字列の比較
 *
 * s1 と s2 を先頭から 1 文字ずつ比較します。
 *
 * @param s1	比較する文字列 1
 * @param s2	比較する文字列 2
 *
 * @return 両者が等しければ 0。最初に異なった位置の文字を unsigned char
 *	として比較し、s1 の方が大きければ正、小さければ負の値。
 */
int knl_strcmp( const char *s1, const char *s2 )
{
	register int result;

	while (*s1) {
		result = (unsigned char)*s1++ - (unsigned char)*s2++;
		if (result) {
			return result;
		}
	}

	return (unsigned char)*s1 - (unsigned char)*s2;
}

/**
 * @brief	文字列の連結
 *
 * dst の文字列の末尾に src の文字列を連結し、'\0' で終端します。
 *
 * @param dst	連結先文字列（'\0' 終端。連結後の長さ分の領域が必要）
 * @param src	連結する文字列（'\0' 終端）
 *
 * @return dst をそのまま返します。
 */
char* knl_strcat( char *dst, const char *src )
{
	register char *cp;

	cp = dst;
	while (*cp) {
		++cp;
	}

	while (*src) {
		*cp++ = *src++;
	}
	*cp = '\0';

	return dst;
}
