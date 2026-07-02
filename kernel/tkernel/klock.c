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
 * @file	klock.c
 * @brief	カーネルロック（オブジェクトロック）
 *
 * カーネル内部オブジェクトの排他制御を行うロック機構の実装です。
 * ロックを獲得したタスクは最高実行優先度として扱われます。
 * ロックのネスト（多重獲得）はできません。
 */

#include "kernel.h"
#include "klock.h"
#include "ready_queue.h"

/**
 * @brief オブジェクトロックの獲得
 *
 * ロックが空いていれば自タスクがロックを獲得し、実行可能キュー
 * （ready queue）上で最高優先扱いとなります。他タスクがロック中の
 * 場合は、自タスクを実行可能キューから外してロック待ちキューに
 * つなぎ、他タスクへディスパッチします。
 *
 * @param loc 対象のオブジェクトロック
 * @note クリティカルセクション内からは呼び出せません。
 * @note 待ち解除後もロックを獲得できていない場合があるため、
 *       獲得できるまで再試行します。
 */
EXPORT void knl_LockOBJ( OBJLOCK *loc )
{
	BOOL	klocked;

  retry:
	BEGIN_CRITICAL_SECTION;
	klocked = knl_ctxtsk->klocked;
	if ( !klocked ) {
		if ( loc->wtskq.next == NULL ) {
			/* ロックを獲得 */
			QueInit(&loc->wtskq);

			knl_ctxtsk->klocked = klocked = TRUE;
			knl_ready_queue.klocktsk = knl_ctxtsk;
		} else {
			/* ロック待ちに移行 */
			knl_ready_queue_delete(&knl_ready_queue, knl_ctxtsk);
			knl_ctxtsk->klockwait = TRUE;
			QueInsert(&knl_ctxtsk->tskque, &loc->wtskq);

			knl_schedtsk = knl_ready_queue_top(&knl_ready_queue);
		}
	}
	END_CRITICAL_SECTION;
	/* ロックを獲得できないまま待ちが解除される場合があるため、
	   未獲得なら再試行する */
	if ( !klocked ) {
		goto retry;
	}
}

/**
 * @brief オブジェクトロックの解放
 *
 * 自タスクが保持するロックを解放します。ロック待ちタスクが
 * あれば先頭のタスクにロックを引き渡し、実行可能キューの先頭に
 * 挿入して待ちを解除します。最後に最高優先度タスクを次の実行
 * タスク（knl_schedtsk）として選び直します。
 *
 * @param loc 対象のオブジェクトロック
 * @note クリティカルセクション内から呼び出しても構いません。
 */
EXPORT void knl_UnlockOBJ( OBJLOCK *loc )
{
	TCB	*tcb;

	BEGIN_CRITICAL_SECTION;
	knl_ctxtsk->klocked = FALSE;
	knl_ready_queue.klocktsk = NULL;

	tcb = (TCB*)QueRemoveNext(&loc->wtskq);
	if ( tcb == NULL ) {
		/* ロックを空き状態に戻す */
		loc->wtskq.next = NULL;
	} else {
		/* ロック待ちタスクの待ち解除 */
		tcb->klockwait = FALSE;
		tcb->klocked = TRUE;
		knl_ready_queue_insert_top(&knl_ready_queue, tcb);
	}

	knl_schedtsk = knl_ready_queue_top(&knl_ready_queue);
	END_CRITICAL_SECTION;
}
