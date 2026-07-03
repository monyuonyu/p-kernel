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
 * @file	semaphore.h
 * @brief	セマフォ機能のカーネル内部定義
 *
 * セマフォ制御ブロック（SEMCB）の定義と、制御ブロック取得マクロを
 * 提供します。
 */

#ifndef _SEMAPHORE_H_
#define _SEMAPHORE_H_

/*
 * セマフォ制御ブロック
 */
typedef struct semaphore_control_block {
	QUEUE	wait_queue;	/* セマフォ待ちキュー */
	ID	semid;		/* セマフォID */
	void	*exinf;		/* 拡張情報 */
	ATR	sematr;		/* セマフォ属性 */
	INT	semcnt;		/* 現在の資源数 */
	INT	maxsem;		/* 最大資源数 */
#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} SEMCB;

IMPORT SEMCB knl_semcb_table[];	/* セマフォ制御ブロックテーブル */
IMPORT QUEUE knl_free_semcb;	/* 未使用制御ブロックのキュー（FreeQue） */

#define get_semcb(id)	( &knl_semcb_table[INDEX_SEM(id)] )


#endif /* _SEMAPHORE_H_ */
