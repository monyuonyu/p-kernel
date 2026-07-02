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
 * @file	mutex.h
 * @brief	ミューテックス機能のカーネル内部定義
 *
 * ミューテックス管理ブロック（MTXCB）の定義と、待ちタスクの有無・
 * 優先度の参照および優先度再計算に用いるマクロを提供します。
 */

#ifndef _MUTEX_H_
#define _MUTEX_H_

typedef struct mutex_control_block	MTXCB;

/*
 * ミューテックス管理ブロック
 */
struct mutex_control_block {
	QUEUE	wait_queue;	/* ミューテックス待ちキュー */
	ID	mtxid;		/* ミューテックス ID */
	void	*exinf;		/* 拡張情報 */
	ATR	mtxatr;		/* ミューテックス属性 */
	UB	ceilpri;	/* ミューテックスの上限優先度 */
	TCB	*mtxtsk;	/* ロック中のタスク */
	MTXCB	*mtxlist;	/* ロック中ミューテックスのリスト */
#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
};

IMPORT MTXCB knl_mtxcb_table[];	/* ミューテックス管理ブロックテーブル */
IMPORT QUEUE knl_free_mtxcb;	/* 未使用管理ブロックのキュー（FreeQue） */

#define get_mtxcb(id)	( &knl_mtxcb_table[INDEX_MTX(id)] )


/*
 * ミューテックス 'mtxcb' にロック待ちのタスクがあれば TRUE
 */
#define mtx_waited(mtxcb)	( !isQueEmpty(&(mtxcb)->wait_queue) )

/*
 * ミューテックス 'mtxcb' のロック待ちタスクの最高優先度を返す
 */
#define mtx_head_pri(mtxcb)	( ((TCB*)(mtxcb)->wait_queue.next)->priority )

/*
 * ロック中タスクの優先度の再計算（TA_INHERIT 専用）
 */
#define reset_priority(tcb)	knl_release_mutex((tcb), NULL)


IMPORT void knl_release_mutex( TCB *tcb, MTXCB *relmtxcb );

#endif /* _MUTEX_H_ */
