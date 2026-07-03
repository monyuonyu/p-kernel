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
 * @file	mempool.h
 * @brief	可変長メモリプールの内部定義
 *
 * 可変長メモリプール管理ブロック（MPLCB）と最大空き領域サイズの取得
 * など、mempool.c の内部で使用する定義を提供します。
 */

#ifndef _MEMPOOL_H_
#define _MEMPOOL_H_

/*
 * 可変長メモリプール管理ブロック
 *	'areaque' はメモリブロックをアドレスの昇順につなぐ
 *	'freeque' は空きブロックをサイズの昇順につなぐ
 *
 *  一部のメンバは IMACB にキャストして使用されるため、
 *  メンバの並び順を変更してはならない。
 */
typedef struct memorypool_control_block {
	QUEUE	wait_queue;	/* メモリプール待ちキュー */
	ID	mplid;		/* 可変長メモリプールID */
	void	*exinf;		/* 拡張情報 */
	ATR	mplatr;		/* メモリプール属性 */
	W	mplsz;		/* メモリプール全体のサイズ */
	QUEUE	areaque;	/* 全ブロックをつなぐキュー */
	QUEUE	freeque;	/* 空きブロックをつなぐキュー */
	QUEUE	areaque_end;	/* areaque の最終要素 */
	void	*mempool;	/* メモリプールの先頭アドレス */
#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} MPLCB;

IMPORT MPLCB knl_mplcb_table[];	/* 可変長メモリプール管理ブロックテーブル */
IMPORT QUEUE knl_free_mplcb;	/* 未使用管理ブロックのキュー（FreeQue） */

#define get_mplcb(id)	( &knl_mplcb_table[INDEX_MPL(id)] )


/**
 * @brief 最大空き領域サイズの取得
 *
 * freeque はサイズ昇順のため、末尾要素のサイズが最大空き領域
 * サイズになります。
 *
 * @param mplcb	メモリプール管理ブロック
 * @return 最大空き領域のサイズ（空きがなければ 0）
 */
Inline W knl_MaxFreeSize( MPLCB *mplcb )
{
	if ( isQueEmpty(&mplcb->freeque) ) {
		return 0;
	}
	return FreeSize(mplcb->freeque.prev);
}

/*
 * 待ちタスクへのメモリブロック割り当て（mempool.c）
 */
IMPORT void knl_mpl_wakeup( MPLCB *mplcb );

#endif /* _MEMPOOL_H_ */
