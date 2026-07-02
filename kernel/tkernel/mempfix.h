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
 * @file	mempfix.h
 * @brief	固定長メモリプールの内部定義
 *
 * 固定長メモリプール管理ブロック（MPFCB）とフリーブロックリスト、
 * ブロックサイズ調整用マクロなど、mempfix.c の内部で使用する定義を
 * 提供します。
 */

#ifndef _MEMPFIX_H_
#define _MEMPFIX_H_

/*
 * 固定長メモリプール管理ブロック
 */
typedef struct free_list {
	struct free_list *next;	/* 次のフリーブロック */
} FREEL;

typedef struct fix_memorypool_control_block {
	QUEUE	wait_queue;	/* メモリプール待ちキュー */
	ID	mpfid;		/* 固定長メモリプールID */
	void	*exinf;		/* 拡張情報 */
	ATR	mpfatr;		/* メモリプール属性 */
	W	mpfcnt;		/* メモリプール全体のブロック数 */
	W	blfsz;		/* 固定長メモリブロックサイズ */
	W	mpfsz;		/* メモリプール全体のサイズ */
	W	frbcnt;		/* 空きブロック数 */
	void	*mempool;	/* メモリプールの先頭アドレス */
	void	*unused;		/* 未使用領域の先頭アドレス */
	FREEL	*freelist;	/* フリーブロックリスト */
	OBJLOCK	lock;		/* オブジェクト排他アクセス用ロック */
#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} MPFCB;

IMPORT MPFCB knl_mpfcb_table[];	/* 固定長メモリプール管理ブロックテーブル */
IMPORT QUEUE knl_free_mpfcb;	/* 未使用管理ブロックのキュー（FreeQue） */

#define get_mpfcb(id)	( &knl_mpfcb_table[INDEX_MPF(id)] )


#define MINSIZE		( sizeof(FREEL) )
#define MINSZ(sz)	( ((UW)(sz) + (UW)(MINSIZE-1)) & ~(UW)(MINSIZE-1) )

/**
 * @brief メモリプール領域の終端アドレスの取得
 * @param mpfcb	メモリプール管理ブロック
 * @return プール領域の終端アドレス（最終ブロックの次のアドレス）
 */
Inline void *knl_mempool_end( MPFCB *mpfcb )
{
	return (VB*)mpfcb->mempool + mpfcb->mpfsz;
}



#endif /* _MEMPFIX_H_ */
