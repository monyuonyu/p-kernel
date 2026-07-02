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
 * @file	wait.h
 * @brief	同期用共通ルーチンの定義
 *
 * タスクの待ち解除・待ち状態遷移のルーチン群と、同期・通信オブジェクト
 * 共通の汎用制御ブロック（GCB）およびそれに対する操作を定義します。
 */

#ifndef _WAIT_
#define _WAIT_

#include <sys/queue.h>
#include "timer.h"
#include "task.h"

/*
 * タスクの待ち状態の解除
 *	タスクをタイマキューと待ちキューから外し、タスク状態を更新する。
 *	'knl_wait_release_ok' は待ち解除したタスクへ E_OK を渡す。
 *	'knl_wait_release_ok_ercd' は 'knl_wait_release_ok' と同じ正常な
 *	待ち解除だが、待ち解除したタスクへ 'ercd' を渡す。
 *	ercd >= 0 であることが必要。
 *	'knl_wait_release_ng' は待ち解除したタスクへ 'ercd' を渡す。
 *	強制的な待ち解除に使用する。ercd < 0 であることが必要。
 *	'knl_wait_release_tmout' はタイマキューからの削除を行わない。
 *	タイムアウト処理に使用する。
 */
IMPORT void knl_wait_release_ok( TCB *tcb );
IMPORT void knl_wait_release_ok_ercd( TCB *tcb, ER ercd );
IMPORT void knl_wait_release_ng( TCB *tcb, ER ercd );
IMPORT void knl_wait_release_tmout( TCB *tcb );

/**
 * @brief タスクの待ち状態を取り消します。
 *
 * タスクをタイマキューと待ちキューから外します。
 * タスク状態の更新は行いません。
 *
 * @param tcb 対象タスクの TCB
 */
Inline void knl_wait_cancel( TCB *tcb )
{
	knl_timer_delete(&tcb->wtmeb);
	QueRemove(&tcb->tskque);
}

/*
 * 実行中タスクを待ち状態に遷移させ、タイマイベントキューに接続する。
 */
IMPORT void knl_make_wait( TMO tmout, ATR atr );
IMPORT void knl_make_wait_reltim( RELTIM tmout, ATR atr );

/*
 * 待ちキューに接続された全タスクの待ちを解除し、E_DLT エラーを設定する。
 * 同期・通信オブジェクトの削除時に使用する。
 */
IMPORT void knl_wait_delete( QUEUE *wait_queue );

/*
 * 待ちキュー先頭タスクの ID を取得する。
 */
IMPORT ID knl_wait_tskid( QUEUE *wait_queue );

/**
 * @brief タスクを優先度順の待ちキューに接続します。
 *
 * キューを先頭から走査し、tcb の優先度より低い（値の大きい）
 * 最初のタスクの直前に挿入します。同一優先度では FIFO 順になります。
 *
 * @param tcb   接続するタスクの TCB
 * @param queue 接続先の待ちキュー
 */
Inline void knl_queue_insert_tpri( TCB *tcb, QUEUE *queue )
{
	QUEUE *q;
	QUEUE *start, *end;
	UB val;
	W offset;

	start = end = queue;
	val = tcb->priority;
	offset = offsetof(TCB, priority);

	for ( q = start->next; q != end; q = q->next ) {
		if ( *(UB*)((VB*)q + offset) > val ) {
			break;
		}
	}

	QueInsert(&tcb->tskque, q);
}

/*
 * 制御ブロックの共通部
 *	タスク同期・通信オブジェクトでは、制御ブロックの先頭部分が
 *	共通になっている。以下はその共通部を操作する共通ルーチンである。
 *	共通部を GCB（generic control block）型として定義する。
 *	オブジェクトが複数の待ちキューを持ち、2 番目以降の待ちキューを
 *	操作する場合は、これらのルーチンは使用できない。
 *	オブジェクト属性の TA_TPRI・TA_NODISWAI ビットを他の用途に
 *	使用している場合も、これらのビットを参照するため使用できない。
 */
typedef struct generic_control_block {
	QUEUE	wait_queue;	/* 待ちキュー */
	ID	objid;		/* オブジェクト ID */
	void	*exinf;		/* 拡張情報 */
	ATR	objatr;		/* オブジェクト属性 */
	/* これ以降に別のフィールドを持ってもよいが、 */
	/* 汎用操作ルーチンでは使用しない。 */
} GCB ;

/*
 * 実行中タスクを待ち状態に遷移させ、タイマイベントキューと
 * オブジェクトの待ちキューに接続する。あわせて knl_ctxtsk の
 * 'wid' を設定する。
 */
IMPORT void knl_gcb_make_wait( GCB *gcb, TMO tmout );

/*
 * タスク優先度変更時に、待ちキュー内でのタスク位置を調整する。
 * オブジェクト属性に TA_TPRI が指定されていない場合は何もしない。
 */
IMPORT void knl_gcb_change_priority( GCB *gcb, TCB *tcb );

/*
 * "tcb" を含めたと仮定した場合の待ちキュー先頭タスクを求める。
 * （"tcb" を待ちキューへ挿入することはない。）
 */
IMPORT TCB* knl_gcb_top_of_wait_queue( GCB *gcb, TCB *tcb );

/**
 * @brief 待ち解除に伴いタスク状態を更新します。
 *
 * 実行可能状態になる場合は実行可能キュー（ready queue）に接続します。
 * 二重待ち（強制待ち＋待ち）の場合は強制待ち状態（TS_SUSPEND）に
 * します。タスクが待ち状態（二重待ちを含む）のときに呼び出してください。
 *
 * @param tcb 対象タスクの TCB
 */
Inline void knl_make_non_wait( TCB *tcb )
{
	if ( tcb->state == TS_WAIT ) {
		knl_make_ready(tcb);
	} else {
		tcb->state = TS_SUSPEND;
	}
}

/**
 * @brief タスクの待ち状態を解除します。
 *
 * タスクをタイマキューと待ちキューから外し、タスク状態を更新します。
 *
 * @param tcb 待ち解除するタスクの TCB
 */
Inline void knl_wait_release( TCB *tcb )
{
	knl_timer_delete(&tcb->wtmeb);
	QueRemove(&tcb->tskque);
	knl_make_non_wait(tcb);
}

#endif /* _WAIT_ */
