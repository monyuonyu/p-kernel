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

/*
 *	string.c
 *	T-Kernel standard library 
 */

#include <tk/tkernel.h>
#include "kernel.h"

/*** binary operation ***/
/* memset : fill memory area */
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

/* memcpy : copy memory */
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

/* strlen : get text string length */
SZ knl_strlen( const char *s )
{
	register char *cp;

	cp = (char *)s;
	while (*cp) {
		++cp;
	}
	return (SZ)(cp - s);
}

/* strcpy : copy text string */
char* knl_strcpy( char *dst, const char *src )
{
	register char *cp;

	cp = dst;
	do {
		*cp++ = *src;
	} while (*src++);

	return dst;
}

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

/* strcmp : perform text string comparison */
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

/* strcat : perform text string concatenation */
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
