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
 * @file	task.h
 * @brief	タスク定義
 *
 * タスク状態の内部表現（TSTAT）、優先度の内部/外部表現変換マクロ、
 * TCB 取得マクロ、およびタスク実行制御ルーチンの宣言を提供します。
 */

#ifndef _TASK_
#define _TASK_

/*
 * タスク状態の内部表現
 *	'state & TS_WAIT' により待ち状態かどうかを判定できます。
 *	'state & TS_SUSPEND' により強制待ち状態かどうかを判定できます。
 */
typedef enum {
	TS_NONEXIST	= 0,	/* 未登録状態 */
	TS_READY	= 1,	/* 実行状態または実行可能状態 */
	TS_WAIT		= 2,	/* 待ち状態 */
	TS_SUSPEND	= 4,	/* 強制待ち状態 */
	TS_WAITSUS	= 6,	/* 二重待ち状態（待ち＋強制待ち） */
	TS_DORMANT	= 8	/* 休止状態 */
} TSTAT;

/**
 * @brief タスクが生存しているかの判定
 *
 * タスクが生存状態（未登録状態・休止状態以外）であれば TRUE を
 * 返します。
 *
 * @param state タスク状態（内部表現）
 * @return 生存していれば TRUE、そうでなければ FALSE
 */
Inline BOOL knl_task_alive( TSTAT state )
{
	return ( (state & (TS_READY|TS_WAIT|TS_SUSPEND)) != 0 );
}


/*
 * タスク優先度の内部/外部表現変換マクロ
 */
#define int_priority(x)		( (INT)((x) - MIN_TSKPRI) )
#define ext_tskpri(x)		( (PRI)((x) + MIN_TSKPRI) )


/*
 * タスク制御情報
 */
IMPORT TCB	knl_tcb_table[];	/* タスク制御ブロック */
IMPORT QUEUE	knl_free_tcb;	/* 未使用 TCB のキュー */

/*
 * タスク ID から TCB を取得するマクロ
 *	get_tcb_self は TSK_SELF 指定時に実行中タスクの TCB を返す。
 */
#define get_tcb(id)		( &knl_tcb_table[INDEX_TSK(id)] )
#define get_tcb_self(id)	( ( (id) == TSK_SELF )? knl_ctxtsk: get_tcb(id) )

/**
 * @brief タスクを休止状態（DORMANT）へ遷移
 *
 * 休止状態でリセットすべき TCB の各変数を初期化し、タスク起動用の
 * コンテキストを設定します。
 *
 * @param tcb 対象タスクの TCB
 */
IMPORT void knl_make_dormant( TCB *tcb );

/**
 * @brief タスクを実行可能状態（READY）へ遷移
 *
 * タスクを実行可能キューへ挿入します。挿入したタスクが最高優先度
 * となった場合は 'knl_schedtsk' を更新します。
 *
 * @param tcb 対象タスクの TCB
 */
IMPORT void knl_make_ready( TCB *tcb );

/**
 * @brief タスクを実行可能状態から外す
 *
 * タスクを実行可能キューから削除します。呼び出し時、タスクは
 * 実行可能状態でなければなりません。本関数から戻った後、
 * 呼び出し側で 'tcb->state' を非実行状態（待ち状態、強制待ち状態、
 * または休止状態）に変更してください。
 *
 * @param tcb 対象タスクの TCB
 */
IMPORT void knl_make_non_ready( TCB *tcb );

/**
 * @brief タスク優先度の変更
 *
 * 'tcb' タスクの優先度を 'priority' に変更し、それに伴い必要な
 * タスク状態遷移を発生させます。
 *
 * @param tcb      対象タスクの TCB
 * @param priority 新しい優先度（内部表現）
 */
IMPORT void knl_change_task_priority( TCB *tcb, INT priority );

/**
 * @brief 実行可能キューの回転
 *
 * knl_rotate_ready_queue は優先度 'priority' の実行可能キューを
 * 回転します。knl_rotate_ready_queue_run は実行可能キュー中の
 * 最高優先度タスクを含む待ち行列を回転します。
 *
 * @param priority 回転対象の優先度（内部表現）
 */
IMPORT void knl_rotate_ready_queue( INT priority );
IMPORT void knl_rotate_ready_queue_run( void );


#include "ready_queue.h"

/**
 * @brief 実行すべきタスクの再選定
 *
 * 実行可能キューの先頭タスクを 'knl_schedtsk' に設定します。
 */
Inline void knl_reschedule( void )
{
	TCB	*toptsk;

	toptsk = knl_ready_queue_top(&knl_ready_queue);
	if ( knl_schedtsk != toptsk ) {
		knl_schedtsk = toptsk;
	}
}

#endif /* _TASK_ */
