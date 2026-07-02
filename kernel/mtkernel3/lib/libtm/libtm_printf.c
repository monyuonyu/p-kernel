/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.03
 *
 *    Copyright (C) 2006-2021 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2021/03/31.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	libtm_printf.c
 * @brief	T-Monitor 互換の printf() / sprintf() 呼び出し
 *
 * 機能を限定した書式付き出力を実装します。
 *	- 浮動小数点、long long などの指定子は未サポート。
 *		変換指定:	 a, A, e, E, f, F, g, G, n
 *		サイズ修飾子:  hh, ll, j, z, t, L
 *	- 出力文字列長に制限はありません。
 *	- スタック使用量を最小化しています。
 *		利用可能なスタックサイズに応じて、TM_OUTBUF_SZ を
 *		適切な値に定義してください。
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#if USE_TMONITOR
#include "libtm.h"

#if USE_TM_PRINTF
#include <stdarg.h>

/* 出力関数の定義 */
typedef	struct {
	H	len;		/* 合計出力長 */
	H	cnt;		/* バッファ内の文字数 */
	UB	*bufp;		/* tm_sprintf 用のバッファポインタ */
} OutPar;
typedef	void	(*OutFn)( UB *str, INT len, OutPar *par );

/**
 * @brief	整数値の文字列変換
 *
 * val を指定の基数で文字列に変換し、バッファ終端 ep の手前へ
 * 後ろ詰めで書き込みます。
 * @param ep	バッファの終端ポインタ（この位置の手前に書き込む）
 * @param val	変換する値
 * @param base	基数（下位 6 ビット）と大文字指定フラグ（0x40）
 * @return 変換後の文字列の先頭ポインタ
 */
LOCAL	UB	*outint( UB *ep, UW val, UB base )
{
LOCAL const UB  digits[32] = "0123456789abcdef0123456789ABCDEF";
	UB	caps;

	caps = (base & 0x40) >> 2;		/* 'a' または 'A' */
	for (base &= 0x3F; val >= base; val /= base) {
		*--ep = digits[(val % base) + caps];
	}
	*--ep = digits[val + caps];
	return ep;				/* バッファの先頭ポインタ */
}

/**
 * @brief	書式付き出力の共通処理（機能限定版）
 *
 * 書式文字列 fmt を解釈し、出力関数 ostr を通じて出力します。
 * @param ostr	出力関数（コンソール出力またはバッファ出力）
 * @param par	出力先パラメータ
 * @param fmt	書式文字列
 * @param ap	可変長引数リスト
 */
LOCAL	void	tm_vsprintf( OutFn ostr, OutPar *par, const UB *fmt, va_list ap )
{
#define	MAX_DIGITS	14
	UW		v;
	H		wid, prec, n;
	UB		*fms, *cbs, *cbe, cbuf[MAX_DIGITS];
	UB		c, base, flg, sign, qual;

/* flg のビット定義 */
#define	F_LEFT		0x01
#define	F_PLUS		0x02
#define	F_SPACE		0x04
#define	F_PREFIX	0x08
#define	F_ZERO		0x10

	for (fms = NULL; (c = *fmt++) != '\0'; ) {

		if (c != '%') {	/* 固定文字列 */
			if (fms == NULL) fms = (UB*)fmt - 1;
			continue;
		}

		/* 固定文字列の出力 */
		if (fms != NULL) {
			(*ostr)(fms, fmt - fms - 1, par);
			fms = NULL;
		}

		/* フラグの取得 */
		for (flg = 0; ; ) {
			switch (c = *fmt++) {
			case '-': flg |= F_LEFT;	continue;
			case '+': flg |= F_PLUS;	continue;
			case ' ': flg |= F_SPACE;	continue;
			case '#': flg |= F_PREFIX;	continue;
			case '0': flg |= F_ZERO;	continue;
			}
			break;
		}

		/* フィールド幅の取得 */
		if (c == '*') {
			wid = va_arg(ap, INT);
			if (wid < 0) {
				wid = -wid;
				flg |= F_LEFT;
			}
			c = *fmt++;
		} else {
			for (wid = 0; c >= '0' && c <= '9'; c = *fmt++)
				wid = wid * 10 + c - '0';
		}

		/* 精度の取得 */
		prec = -1;
		if (c == '.') {
			c = *fmt++;
			if (c == '*') {
				prec = va_arg(ap, INT);
				if (prec < 0) prec = 0;
				c = *fmt++;
			} else {
				for (prec = 0;c >= '0' && c <= '9';c = *fmt++)
					prec = prec * 10 + c - '0';
			}
			flg &= ~F_ZERO;		/* ゼロ埋めなし */
		}

		/* サイズ修飾子の取得 */
		qual = 0;
		if (c == 'h' || c == 'l') {
			qual = c;
			c = *fmt++;
		}

		/* 変換指定ごとの処理 */
		base = 10;
		sign = 0;
		cbe = &cbuf[MAX_DIGITS];	/* バッファの終端ポインタ */

		switch (c) {
		case 'i':
		case 'd':
		case 'u':
		case 'X':
		case 'x':
		case 'o':
			if (qual == 'l') {
				v = va_arg(ap, UW);
			} else {
				v = va_arg(ap, UINT);
				if (qual == 'h') {
					v = (c == 'i' || c == 'd') ?
						(H)v :(UH)v;
				}
			}
			switch (c) {
			case 'i':
			case 'd':
				if ((W)v < 0) {
					v = - (W)v;
					sign = '-';
				} else if ((flg & F_PLUS) != 0) {
					sign = '+';
				} else if ((flg & F_SPACE) != 0) {
					sign = ' ';
				} else {
					break;
				}
				wid--;		/* 符号の分 */
			case 'u':
				break;
			case 'X':
				base += 0x40;	/* base = 16 + 0x40 */
			case 'x':
				base += 8;	/* base = 16 */
			case 'o':
				base -= 2;	/* base = 8 */
				if ((flg & F_PREFIX) != 0 && v != 0) {
					wid -= (base == 8) ? 1 : 2;
					base |= 0x80;
				}
				break;
			}
			/* 注: v == 0 かつ prec == 0 のときは何も出力しない */
			cbs = (v == 0 && prec == 0) ?
						cbe : outint(cbe, v, base);
			break;
		case 'p':
			v = (UW)va_arg(ap, void *);
			if (v != 0) {
				base = 16 | 0x80;
				wid -= 2;
			}
			cbs = outint(cbe, v, base);
			break;
		case 's':
			cbe = cbs = va_arg(ap, UB *);
			if (prec < 0) {
				while (*cbe != '\0') cbe++;
			} else {
				while (--prec >= 0 && *cbe != '\0') cbe++;
			}
			break;
		case 'c':
			cbs = cbe;
			*--cbs = (UB)va_arg(ap, INT);
			prec = 0;
			break;
		case '\0':
			fmt--;
			continue;
		default:
			/* 固定文字列として出力 */
			fms = (UB*)fmt - 1;
			continue;
		}

		n = cbe - cbs;				/* 項目の長さ */
		if ((prec -= n) > 0) n += prec;
		wid -= n;				/* パディングの長さ */

		/* 前詰めの空白の出力 */
		if ((flg & (F_LEFT | F_ZERO)) == 0 ) {
			while (--wid >= 0) (*ostr)((UB*)" ", 1, par);
		}

		/* 符号の出力 */
		if (sign != 0) {
			(*ostr)(&sign, 1, par);
		}

		/* プレフィックス "0x"・"0X"・"0" の出力 */
		if ((base & 0x80) != 0) {
			(*ostr)((UB*)"0", 1, par);
			if ((base & 0x10) != 0) {
				(*ostr)((base & 0x40) ? (UB*)"X" : (UB*)"x", 1, par);
			}
		}

		/* 精度またはゼロ埋めのための先行ゼロの出力 */
		if ((n = prec) <= 0) {
			if ((flg & (F_LEFT | F_ZERO)) == F_ZERO ) {
				n = wid;
				wid = 0;
			}
		}
		while (--n >= 0) (*ostr)((UB*)"0", 1, par);

		/* 項目文字列の出力 */
		(*ostr)(cbs, cbe - cbs, par);

		/* 後続の空白の出力 */
		while (--wid >= 0) (*ostr)((UB*)" ", 1, par);
	}

	/* 最後の固定文字列の出力 */
	if (fms != NULL) {
		(*ostr)(fms, fmt - fms - 1, par);
	}
#if	TM_OUTBUF_SZ > 0
	/* 出力のフラッシュ */
	(*ostr)(NULL, 0, par);
#endif
}

/**
 * @brief	コンソールへの出力関数
 *
 * TM_OUTBUF_SZ == 0 のときは 1 文字ずつ直接出力し、
 * それ以外はバッファ経由で出力します（str == NULL でフラッシュ）。
 * @param str	出力する文字列（NULL でフラッシュ指示）
 * @param len	出力する長さ
 * @param par	出力先パラメータ
 */
LOCAL	void	out_cons( UB *str, INT len,  OutPar *par )
{
#if	TM_OUTBUF_SZ == 0
	/* コンソールへ直接出力 */
	par->len += len;
	while (--len >= 0) tm_putchar(*str++);
#else
	/* バッファ経由でコンソールへ出力 */
	if (str == NULL) {	/* フラッシュ */
		if (par->cnt > 0) {
			par->bufp[par->cnt] = '\0';
			tm_putstring(par->bufp);
			par->cnt = 0;
		}
	} else {
		par->len += len;
		while (--len >= 0) {
			if (par->cnt >= TM_OUTBUF_SZ - 1) {
				par->bufp[par->cnt] = '\0';
				tm_putstring(par->bufp);
				par->cnt = 0;
			}
			par->bufp[par->cnt++] = *str++;
		}
	}
#endif
}

/**
 * @brief	コンソールへの書式付き出力（T-Monitor 互換）
 * @param format	書式文字列
 * @return 出力した文字数
 */
EXPORT INT	tm_printf( const UB *format, ... )
{
	va_list	ap;

#if	TM_OUTBUF_SZ == 0
	H	len = 0;

	va_start(ap, format);
	tm_vsprintf(out_cons, (OutPar*)&len, format, ap);
	va_end(ap);
	return len;
#else
	UB	obuf[TM_OUTBUF_SZ];
	OutPar	par;

	par.len = par.cnt = 0;
	par.bufp = obuf;
	va_start(ap, format);
	tm_vsprintf(out_cons, (OutPar*)&par, format, ap);
	va_end(ap);
	return par.len;
#endif
}

/**
 * @brief	バッファへの出力関数
 * @param str	出力する文字列
 * @param len	出力する長さ
 * @param par	出力先パラメータ（bufp の指す位置へ書き込む）
 */
LOCAL	void	out_buf( UB *str, INT len, OutPar *par )
{
	par->len += len;
	while (--len >= 0) *(par->bufp)++ = *str++;
}

/**
 * @brief	文字列バッファへの書式付き出力（T-Monitor 互換）
 * @param str		出力先バッファ（終端に '\0' を付加）
 * @param format	書式文字列
 * @return 出力した文字数（終端の '\0' を除く）
 */
EXPORT INT	tm_sprintf( UB *str, const UB *format, ... )
{
	OutPar	par;
	va_list	ap;

	par.len = 0;
	par.bufp = str;
	va_start(ap, format);
	tm_vsprintf(out_buf, &par, format, ap);
	va_end(ap);
	str[par.len] = '\0';
	return par.len;
}

#else
/*
 * USE_TM_PRINTF が無効の場合のスタブ（常に -1 を返す）
 */
EXPORT INT	tm_printf( const UB *format, ... )
{
	return (-1);
}

EXPORT INT	tm_sprintf( UB *str, const UB *format, ... )
{
	return (-1);
}

#endif /* USE_TM_PRINTF */
#endif /* USE_TMONITOR */