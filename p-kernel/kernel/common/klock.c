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
 * @file klock.c
 * @brief カーネルロック機能
 * 
 * このファイルは、T-Kernelのカーネル内部で使用される
 * オブジェクトレベルの排他制御機能を実装します。
 * 
 * 主な機能：
 * - オブジェクトロック（OBJLOCKベース）
 * - ロック待ちタスクの管理
 * - ロック継承によるプリオリティインバージョン対策
 * - クリティカルセクション内での安全なロック操作
 * 
 * カーネルロックの特徴：
 * - タスクがロック取得中は最高優先度で実行
 * - ロック待ちタスクは専用キューで管理
 * - ロック解放時の即座のタスク切り替え
 * - 再帰的ロック要求への対応
 */

#include "kernel.h"
#include "task.h"
#include "ready_queue.h"

/**
 * @brief オブジェクトロック取得
 * @param loc ロックオブジェクトへのポインタ
 * 
 * 指定されたオブジェクトのロックを取得します。ロックが既に
 * 他のタスクによって取得されている場合は、ロックが解放される
 * まで待機します。
 * 
 * @note クリティカルセクションからは呼び出さないでください
 * @note ロック取得中のタスクは最高優先度で実行されます
 * @note 待機が解除されてもロックが取得できない場合は再試行します
 */
EXPORT void knl_LockOBJ( OBJLOCK *loc )
{
	BOOL	klocked;

  retry:
	BEGIN_CRITICAL_SECTION;
	klocked = knl_ctxtsk->klocked;
	if ( !klocked ) {
		if ( loc->wtskq.next == NULL ) {
			/* Lock */
			QueInit(&loc->wtskq);

			knl_ctxtsk->klocked = klocked = TRUE;
			knl_ready_queue.klocktsk = knl_ctxtsk;
		} else {
			/* Ready for lock */
			knl_ready_queue_delete(&knl_ready_queue, knl_ctxtsk);
			knl_ctxtsk->klockwait = TRUE;
			QueInsert(&knl_ctxtsk->tskque, &loc->wtskq);

			knl_schedtsk = knl_ready_queue_top(&knl_ready_queue);
			knl_dispatch_request();
		}
	}
	END_CRITICAL_SECTION;
	/* Since wait could be freed without getting lock, 
	   need to re-try if lock is not got */
	if ( !klocked ) {
		goto retry;
	}
}

/**
 * @brief オブジェクトロック解放
 * @param loc ロックオブジェクトへのポインタ
 * 
 * 指定されたオブジェクトのロックを解放します。ロック待ちの
 * タスクがある場合は、そのタスクにロックを渡して実行可能状態に
 * 戻します。
 * 
 * @note クリティカルセクションからも安全に呼び出せます
 * @note ロック待ちタスクがある場合は即座にタスク切り替えが発生します
 * @note ロック解放により現在のタスクの最高優先度は取り消されます
 */
EXPORT void knl_UnlockOBJ( OBJLOCK *loc )
{
	TCB	*tcb;

	BEGIN_CRITICAL_SECTION;
	knl_ctxtsk->klocked = FALSE;
	knl_ready_queue.klocktsk = NULL;

	tcb = (TCB*)QueRemoveNext(&loc->wtskq);
	if ( tcb == NULL ) {
		/* Free lock */
		loc->wtskq.next = NULL;
	} else {
		/* Wake lock wait task */
		tcb->klockwait = FALSE;
		tcb->klocked = TRUE;
		knl_ready_queue_insert_top(&knl_ready_queue, tcb);
	}

	knl_schedtsk = knl_ready_queue_top(&knl_ready_queue);
	if ( knl_ctxtsk != knl_schedtsk ) {
		knl_dispatch_request();
	}
	END_CRITICAL_SECTION;
}
