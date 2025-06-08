/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 2.0 Software Package
 *
 *    Copyright (C) 2006-2014 by Ken Sakamura.
 *    This software is distributed under the T-License 2.0.
 *----------------------------------------------------------------------
 *
 *    Released by T-Engine Forum(http://www.t-engine.org/) at 2014/09/01.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file bitop.c
 * @brief ビット操作ライブラリ
 * 
 * このファイルは、T-Kernelの共通標準ライブラリの一部として、
 * ビット単位での操作を効率的に行うための関数群を提供します。
 * 
 * 主な機能：
 * - 指定位置のビットのセット/クリア
 * - ビット列の検索（1が設定されているビットの位置を検索）
 * 
 * これらの関数は、タスク管理やスケジューリング、メモリ管理などの
 * カーネル内部処理で広く使用されます。
 */

/** [BEGIN Common Definitions] */
#include <basic.h>
#include <tkernel.h>
#include "utk_config.h"

/*** macros ***/
/* bit operation macro */
#if BIGENDIAN
#define _BIT_SET_N(n) ( (UB)0x80 >> ((n) & 7) )
#define _BIT_SHIFT(n) ( (UB)n >> 1 )
#else
#define _BIT_SET_N(n) ( (UB)0x01 << ((n) & 7) )
#define _BIT_SHIFT(n) ( (UB)n << 1 )
#endif
/** [END Common Definitions] */


/*** bit operation ***/
#ifdef USE_FUNC_TSTDLIB_BITCLR
/**
 * @brief 指定位置のビットをクリア（0に設定）する
 * @param base ビット列の開始アドレス
 * @param offset クリアするビットの位置（0から開始）
 * 
 * 指定されたビット位置のビットを0にクリアします。
 * offsetが負の値の場合は何も行いません。
 */
void
knl_tstdlib_bitclr( void *base, W offset )
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
 * @brief 指定位置のビットをセット（1に設定）する
 * @param base ビット列の開始アドレス
 * @param offset セットするビットの位置（0から開始）
 * 
 * 指定されたビット位置のビットを1にセットします。
 * offsetが負の値の場合は何も行いません。
 */
void
knl_tstdlib_bitset( void *base, W offset )
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
 * @brief ビット列から1が設定されているビットを検索する
 * @param base ビット列の開始アドレス
 * @param offset 検索開始位置（0から開始）
 * @param width 検索範囲の幅（ビット数）
 * @return 最初に見つかった1ビットの位置、見つからない場合は-1
 * 
 * 指定されたビット列の範囲内で、1が設定されている最初のビットの
 * 位置を検索します。効率的な検索のため、バイト単位でのスキップを行います。
 */
W
knl_tstdlib_bitsearch1( void *base, W offset, W width )
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
		if (*cp) {		/* includes 1 --> search bit of 1 */
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
		} else {		/* all bits are 0 --> 1 Byte skip */
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
