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
 * unsigned long で行う（UW では上位 32bit が失われる） */
#define setAreaFlag(q, f)   ( (q)->prev = (QUEUE*)((unsigned long)(q)->prev |  (unsigned long)(f)) )
#define clrAreaFlag(q, f)   ( (q)->prev = (QUEUE*)((unsigned long)(q)->prev & ~(unsigned long)(f)) )
#define chkAreaFlag(q, f)   ( ((unsigned long)(q)->prev & (unsigned long)(f)) != 0 )

#define Mask(x)		( (QUEUE*)((unsigned long)(x) & ~AREA_MASK) )
#define Assign(x, y)	( (x) = (QUEUE*)(((unsigned long)(x) & AREA_MASK) | (unsigned long)(y)) )
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
