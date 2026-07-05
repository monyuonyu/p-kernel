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
 * @file	memory.h
 * @brief	カーネル内動的メモリ管理の定義
 *
 * メモリ確保管理情報 IMACB と、AreaQue / FreeQue を操作する
 * マクロおよび関数プロトタイプを定義します。
 */

#ifndef _MEMORY_H_
#define _MEMORY_H_

#include "limits.h"

/*
 * メモリ確保管理情報
 *
 *  MPLCB からのキャストで参照されるため、
 *  メンバの並び順を変更してはならない。
 */
typedef struct {
	W		memsz;

	/* AreaQue: 確保ページを分割した各領域を連結するキュー。
	   ページ内はアドレスの昇順に整列する。
	   ページ間では整列しない。 */
	QUEUE		areaque;
	/* FreeQue: 確保ページ内の未使用領域を連結するキュー。
	   空きサイズの小さい順に整列する。 */
	QUEUE		freeque;
} IMACB;

/*
 * "&areaque" の位置を 2 バイト境界に整列させるための補正
 *
 * p-kernel 変更（LP64 対応）: ポインタ演算のキャストを UW（32bit）から
 * unsigned long へ変更。LP64 ホストでは UW キャストがポインタ上位
 * 32bit を切り捨てるため。32bit MCU では従来と同一。
 */
#define AlignIMACB(imacb)	( (IMACB*)((unsigned long)(imacb) & ~0x00000001UL) )

/*
 * 分割の最小単位
 *	メモリは ROUNDSZ 単位で確保されるため、
 *	アドレスの下位 1 ビットは常に 0 となる。
 *	AreaQue はこの下位 1 ビットをフラグとして使用する。
 */
#define ROUNDSZ		( sizeof(QUEUE) )	/* 8 バイト */
#define ROUND(sz)	( ((UW)(sz) + (UW)(ROUNDSZ-1)) & ~(UW)(ROUNDSZ-1) )

/* 最小フラグメントサイズ */
#define MIN_FRAGMENT	( sizeof(QUEUE) * 2 )

/*
 * 確保可能な最大サイズ（パラメータチェック用）
 */
#define	MAX_ALLOCATE	( INT_MAX & ~(ROUNDSZ-1) )

/**
 * @brief 確保可能なサイズへの調整
 *
 * サイズを最小フラグメントサイズ以上に切り上げ、
 * ROUNDSZ 単位に丸めます。
 *
 * @param sz 要求サイズ（バイト数）
 * @return 調整後のサイズ
 */
Inline W roundSize( W sz )
{
	if ( sz < (W)MIN_FRAGMENT ) {
		sz = (W)MIN_FRAGMENT;
	}
	return (W)(((UW)sz + (UW)(ROUNDSZ-1)) & ~(UW)(ROUNDSZ-1));
}


/*
 * AreaQue の 'prev' の下位ビットを使用するフラグ
 */
#define AREA_USE	0x00000001UL	/* 使用中 */
#define AREA_MASK	0x00000001UL

/* p-kernel 変更（LP64 対応）: ポインタを経由するビット操作は
 * unsigned long で行う（UW では上位 32bit が失われる）。
 *
 * さらに Windows ネイティブ（LLP64）対応: LLP64 では unsigned long が
 * 32bit のためポインタ上位 32bit が失われる（LP64 の UW と同じ罠が
 * 再来する）。ポインタ幅の符号無し整数 KNL_UPTR を経由する。
 *   - LP64（Linux / bare-metal x86_64・aarch64）: unsigned long == 64bit
 *     で KNL_UPTR も同一 → 生成コードはバイト一致。
 *   - LLP64（Windows x86_64）: unsigned long long == 64bit を使う。
 *   - 32bit MCU: いずれも 32bit で従来どおり。
 * マスク定数も ~(KNL_UPTR) で 64bit 化する（~AREA_MASK が 32bit だと
 * 64bit ポインタの上位を落とすため）。 */
#ifdef _WIN32
typedef unsigned long long KNL_UPTR;
#else
typedef unsigned long KNL_UPTR;
#endif

#define setAreaFlag(q, f)   ( (q)->prev = (QUEUE*)((KNL_UPTR)(q)->prev |  (KNL_UPTR)(f)) )
#define clrAreaFlag(q, f)   ( (q)->prev = (QUEUE*)((KNL_UPTR)(q)->prev & ~(KNL_UPTR)(f)) )
#define chkAreaFlag(q, f)   ( ((KNL_UPTR)(q)->prev & (KNL_UPTR)(f)) != 0 )

#define Mask(x)		( (QUEUE*)((KNL_UPTR)(x) & ~(KNL_UPTR)AREA_MASK) )
#define Assign(x, y)	( (x) = (QUEUE*)(((KNL_UPTR)(x) & (KNL_UPTR)AREA_MASK) | (KNL_UPTR)(y)) )
/*
 * 領域サイズ
 */
#define AreaSize(aq)	( (VB*)(aq)->next - (VB*)((aq) + 1) )
#define FreeSize(fq)	( (W)((fq) + 1)->prev )


IMPORT QUEUE* knl_searchFreeArea( IMACB *imacb, W blksz );
IMPORT void knl_appendFreeArea( IMACB *imacb, QUEUE *aq );
IMPORT void knl_removeFreeQue( QUEUE *fq );
IMPORT void knl_insertAreaQue( QUEUE *que, QUEUE *ent );
IMPORT void knl_removeAreaQue( QUEUE *aq );

IMPORT IMACB *knl_imacb;

#endif /* _MEMORY_H_ */
