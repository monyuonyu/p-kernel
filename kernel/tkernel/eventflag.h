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
 * @file	eventflag.h
 * @brief	イベントフラグ機能のカーネル内部定義
 *
 * イベントフラグ管理ブロック（FLGCB）の定義と、待ち解除条件を
 * 判定するインライン関数を提供します。
 */

#ifndef _EVENTFLAG_H_
#define _EVENTFLAG_H_

/*
 * イベントフラグ管理ブロック
 */
typedef struct eventflag_control_block {
	QUEUE	wait_queue;	/* イベントフラグ待ちキュー */
	ID	flgid;		/* イベントフラグ ID */
	void	*exinf;		/* 拡張情報 */
	ATR	flgatr;		/* イベントフラグ属性 */
	UINT	flgptn;		/* イベントフラグの現在パターン */
#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} FLGCB;

IMPORT FLGCB knl_flgcb_table[];	/* イベントフラグ管理ブロックテーブル */
IMPORT QUEUE knl_free_flgcb;	/* 未使用管理ブロックのキュー（FreeQue） */

#define get_flgcb(id)	( &knl_flgcb_table[INDEX_FLG(id)] )


/**
 * @brief イベントフラグ待ち解除条件の判定
 *
 * 現在のフラグパターンが待ちパターン waiptn を満たしているかを
 * 判定します。wfmode に TWF_ORW が指定されていれば OR 待ち
 * （いずれかのビットが一致）、それ以外は AND 待ち
 * （すべてのビットが一致）として評価します。
 *
 * @param flgcb	対象イベントフラグの管理ブロック
 * @param waiptn	待ちパターン
 * @param wfmode	待ちモード（TWF_ANDW / TWF_ORW）
 * @return 条件を満たしていれば TRUE、満たしていなければ FALSE
 */
Inline BOOL knl_eventflag_cond( FLGCB *flgcb, UINT waiptn, UINT wfmode )
{
	if ( (wfmode & TWF_ORW) != 0 ) {
		return ( (flgcb->flgptn & waiptn) != 0 );
	} else {
		return ( (flgcb->flgptn & waiptn) == waiptn );
	}
}


#endif /* _EVENTFLAG_H_ */
