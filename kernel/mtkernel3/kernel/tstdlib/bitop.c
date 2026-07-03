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
 * @file	bitop.c
 * @brief	T-Kernel 標準ライブラリ（ビット操作）
 *
 * カーネル内部で使用するビット列操作関数（ビットのセット・クリア・
 * 1 ビットの探索）を提供します。ビット番号とバイト内ビット位置の
 * 対応はエンディアンに依存し、マクロで吸収しています。
 */

#include <tk/tkernel.h>

/*** マクロ ***/
/* ビット操作マクロ */
#if BIGENDIAN
#define _BIT_SET_N(n) ( (UB)0x80 >> ((n) & 7) )
#define _BIT_SHIFT(n) ( (UB)n >> 1 )
#else
#define _BIT_SET_N(n) ( (UB)0x01 << ((n) & 7) )
#define _BIT_SHIFT(n) ( (UB)n << 1 )
#endif


/*** ビット操作 ***/
#ifdef USE_FUNC_TSTDLIB_BITCLR
/**
 * @brief	指定ビットのクリア
 *
 * base から offset ビット目のビットを 0 にします。
 *
 * @param base		ビット列の先頭アドレス
 * @param offset	クリアするビットの位置（0 起点）
 *
 * @note offset が負の場合は何もせずに戻ります。
 */
void
knl_bitclr( void *base, W offset )
{
	register UB *cp, mask;

	if (offset < 0) {
		return;
	}

	cp = (UB*)base;
	cp += offset / 8;

	mask = _BIT_SET_N(offset);

	*cp &= ~mask;
}
#endif /* USE_FUNC_TSTDLIB_BITCLR */

#ifdef USE_FUNC_TSTDLIB_BITSET
/**
 * @brief	指定ビットのセット
 *
 * base から offset ビット目のビットを 1 にします。
 *
 * @param base		ビット列の先頭アドレス
 * @param offset	セットするビットの位置（0 起点）
 *
 * @note offset が負の場合は何もせずに戻ります。
 */
void
knl_bitset( void *base, W offset )
{
	register UB *cp, mask;

	if (offset < 0) {
		return;
	}

	cp = (UB*)base;
	cp += offset / 8;

	mask = _BIT_SET_N(offset);

	*cp |= mask;
}
#endif /* USE_FUNC_TSTDLIB_BITSET */

#ifdef USE_FUNC_TSTDLIB_BITSEARCH1
/**
 * @brief	ビット列中の 1 のビットの探索
 *
 * base から offset ビット目を起点に width ビットの範囲を走査し、
 * 最初に見つかった 1 のビットの位置を返します。
 * 全ビットが 0 のバイトは 1 バイト単位で読み飛ばします。
 *
 * @param base		ビット列の先頭アドレス
 * @param offset	探索開始ビット位置（0 起点）
 * @param width		探索するビット数
 *
 * @return 見つかったビットの探索開始位置からの相対位置（0 起点）。
 *		見つからない場合、または offset か width が負の場合は -1。
 */
W
knl_bitsearch1( void *base, W offset, W width )
{
	register UB *cp, mask;
	register W position;

	if ((offset < 0) || (width < 0)) {
		return -1;
	}

	cp = (UB*)base;
	cp += offset / 8;

	position = 0;
	mask = _BIT_SET_N(offset);

	while (position < width) {
		if (*cp) {		/* 1 を含む → 1 のビットを探索 */
			while (1) {
				if (*cp & mask) {
					if (position < width) {
						return position;
					} else {
						return -1;
					}
				}
				mask = _BIT_SHIFT(mask);
				++position;
			}
		} else {		/* 全ビットが 0 → 1 バイト読み飛ばし */
			if (position) {
				position += 8;
			} else {
				position = 8 - (offset & 7);
				mask = _BIT_SET_N(0);
			}
			cp++;
		}
	}

	return -1;
}
#endif /* USE_FUNC_TSTDLIB_BITSEARCH1 */
