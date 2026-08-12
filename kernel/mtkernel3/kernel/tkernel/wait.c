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
 * @file	wait.c
 * @brief	タスクの待ち状態を操作する同期用共通ルーチン
 *
 * 待ち解除（正常・エラー・タイムアウト）、待ち状態への遷移、
 * および同期・通信オブジェクト共通の汎用制御ブロック（GCB）に対する
 * 待ちキュー操作を提供します。
 */

#include "kernel.h"
#include "wait.h"
#ifdef KCC_DIAG
#include <tm/tmonitor.h>	/* KILL-CHURN-CRASH diagnostic: tm_putstring */
#endif

/**
 * @brief タスクの待ち状態を正常に解除します（エラーコード E_OK）。
 *
 * タスクをタイマキューと待ちキューから外して状態を更新し、
 * 待ちに入っていた API の戻り値として E_OK を設定します。
 *
 * @param tcb 待ち解除するタスクの TCB
 */
EXPORT void knl_wait_release_ok( TCB *tcb )
{
	knl_wait_release(tcb);
	*tcb->wercd = E_OK;
}

/**
 * @brief タスクの待ち状態を正常に解除し、指定のエラーコードを返します。
 *
 * knl_wait_release_ok() と同様の正常な待ち解除ですが、待ちに入っていた
 * API の戻り値として ercd を設定します。ercd >= 0 であることが前提です。
 *
 * @param tcb  待ち解除するタスクの TCB
 * @param ercd 待ち解除されたタスクへ渡す値（ercd >= 0）
 */
EXPORT void knl_wait_release_ok_ercd( TCB *tcb, ER ercd )
{
	knl_wait_release(tcb);
	*tcb->wercd = ercd;
}

/**
 * @brief タスクの待ち状態を異常終了として解除します。
 *
 * タスクをタイマキューと待ちキューから外して状態を更新し、
 * 待ち要因ごとの待ち解除フック（rel_wai_hook）があれば呼び出したうえで、
 * 待ちに入っていた API の戻り値として ercd を設定します。
 * 強制待ち解除（tk_rel_wai 等）に使用し、ercd < 0 であることが前提です。
 *
 * @param tcb  待ち解除するタスクの TCB
 * @param ercd 待ち解除されたタスクへ渡すエラーコード（ercd < 0）
 */
EXPORT void knl_wait_release_ng( TCB *tcb, ER ercd )
{
	knl_wait_release(tcb);
	if ( tcb->wspec->rel_wai_hook != NULL ) {
		(*tcb->wspec->rel_wai_hook)(tcb);
	}
	*tcb->wercd = ercd;
}

/**
 * @brief タイムアウトによりタスクの待ち状態を解除します。
 *
 * タイマイベントのコールバックとして呼ばれるため、タイマキューからの
 * 削除は行わず、待ちキューから外して状態を更新します。
 * 待ち要因ごとの待ち解除フック（rel_wai_hook）があれば呼び出します。
 * 待ちに入っていた API の戻り値（E_TMOUT）は待ち開始時に設定済みです。
 *
 * @param tcb タイムアウトしたタスクの TCB
 */
EXPORT void knl_wait_release_tmout( TCB *tcb )
{
#ifdef KCC_DIAG
	/* KILL-CHURN-CRASH diagnostic — OFF in shipped builds (opt-in only).
	 *
	 * A live wait-timer must never fire on a task that is no longer in a
	 * timed wait.  The condition MIRRORS the cure below ((state & TS_WAIT)
	 * == 0) on purpose: an earlier revision tested only TS_NONEXIST /
	 * TS_DORMANT, which CANNOT see the case that actually happens — the
	 * slot is recycled and the new occupant is already TS_READY by the time
	 * the stale timer lands (the crash's preceding console line is
	 * "[elf] task started").  A diagnostic narrower than the cure is a net
	 * that the fish swims past.  The TCB pool (knl_tcb_table) is a STATIC
	 * array, so ASAN/valgrind cannot see this UAF — the memory is always
	 * mapped and the hazard is LOGICAL reuse; this is the equivalent loud
	 * diagnostic.
	 *
	 * HONEST BOUND (measured, do not re-derive): tm_putstring from THIS
	 * callback (timer IRQ context) does NOT reach the captured serial log —
	 * an audit build that printed here emitted 0 lines over 42 boots while
	 * an otherwise identical build that only hung was hit 2/30.  So a
	 * SILENT log is NOT evidence the branch was never taken.  The hang is
	 * the load-bearing signal; the string is a best-effort courtesy.
	 *
	 * 3.0 port note: micro T-Kernel 2.0's <tmonitor.h> exposed tm_monitor()
	 * to drop into the monitor; the μT-Kernel 3.0 tm/tmonitor.h has NO
	 * tm_monitor (see kernel/mtkernel3/include/tm/tmonitor.h), so the halt
	 * is expressed with the primitives 3.0 actually has: print, then spin
	 * with interrupts disabled.  Same observable effect (the machine stops
	 * with the state on the console), no new API invented. */
	if ( (tcb->state & TS_WAIT) == 0 ) {
		tm_putstring((UB *)"[kill-churn] CAUGHT: wait-timer fired on a task not in a timed wait, state=");
		{
			/* NUL-terminated: tm_putstring scans to '\0'
			 * (lib/libtm/libtm.c:120).  A 4-byte array filled with 4
			 * bytes made it read off the end of this frame. */
			char b[5];
			b[0] = (char)('0' + ((tcb->state >> 4) & 0xf));
			b[1] = (char)('0' + (tcb->state & 0xf));
			b[2] = '\r';
			b[3] = '\n';
			b[4] = '\0';
			tm_putstring((UB *)b);
		}
		DISABLE_INTERRUPT;
		for ( ;; ) { }
	}
#endif
	/* KILL-CHURN-CRASH cure — stale-timer-on-recycle guard.
	 *
	 * A wait-timer callback must only fire on a task that is STILL in a
	 * timed wait.  Under foreign kill/heal churn of a ring3 daemon the
	 * victim's TCB (a slot in the STATIC knl_tcb_table) can be freed by
	 * tk_del_tsk and RECYCLED into a brand-new task before its own,
	 * embedded, wtmeb's pending timer is serviced.  The callback then runs
	 * on the NEW occupant, whose tcb->tskque links a DIFFERENT object
	 * queue (or, freshly recycled, a garbage prev): the unconditional
	 * QueRemove below stores through a wild pointer — the ring0 #PF
	 * (err=0x2 / CS=0x08) observed right after "[elf] task started".
	 *
	 * The wtmeb unlink in knl_make_dormant / knl_del_tsk is correct timer
	 * hygiene but does not close the intrinsic teardown race: the timer
	 * IRQ can dequeue this event in the same tick the slot is recycled.
	 * So guard the ACTION, not just the queue: a genuine timeout always
	 * finds the task with the TS_WAIT bit set (knl_make_wait sets it; only
	 * release clears it).  If TS_WAIT is absent the task is no longer
	 * waiting on anything we own — the timer is stale; drop it.  No
	 * legitimate wakeup is lost: TS_WAITSUS (= TS_WAIT|TS_SUSPEND) still
	 * carries the bit, so a suspended timed wait is not missed.
	 *
	 * (Restored from the pre-f50c30a0 kernel/common/wait.c, where this was
	 * commit 2dbacd66.  f50c30a0 replaced kernel/common with the μT-Kernel
	 * 3.0 vendor core, which never had it.) */
	if ( (tcb->state & TS_WAIT) == 0 ) {
		return;
	}
	QueRemove(&tcb->tskque);
	QueInit(&tcb->tskque);
	knl_make_non_wait(tcb);
	if ( tcb->wspec->rel_wai_hook != NULL ) {
		(*tcb->wspec->rel_wai_hook)(tcb);
	}
}

/**
 * @brief 実行中タスクを待ち状態に遷移させ、タイマイベントキューに接続します。
 *
 * 通常 knl_ctxtsk は実行状態（RUN）ですが、システムコール実行中に割込みが
 * 発生し、割込みハンドラ内のシステムコールによって他の状態に変わることが
 * あります。ただし待ち状態（WAIT）になることはありません。
 * タイムアウト時には knl_wait_release_tmout() が呼び出されます。
 *
 * @param tmout タイムアウト時間（ms）。TMO_FEVR(-1) で永久待ち
 * @param atr   待ち対象オブジェクトの属性（本実装では未使用）
 *
 * @note "include/tk/typedef.h" にて
 *       typedef W TMO; / typedef UW RELTIM; / #define TMO_FEVR (-1)
 */
EXPORT void knl_make_wait( TMO tmout, ATR atr )
{
	switch ( knl_ctxtsk->state ) {
	  case TS_READY:
		knl_make_non_ready(knl_ctxtsk);
		knl_ctxtsk->state = TS_WAIT;
		break;
	  case TS_SUSPEND:
		knl_ctxtsk->state = TS_WAITSUS;
		break;
	}
	knl_timer_insert(&knl_ctxtsk->wtmeb, tmout, (CBACK)knl_wait_release_tmout, knl_ctxtsk);
}

/**
 * @brief 実行中タスクを待ち状態に遷移させます（相対時間 RELTIM 指定版）。
 *
 * knl_make_wait() と同様ですが、タイムアウト時間を符号なしの相対時間
 * RELTIM で指定します（永久待ちの指定はできません）。
 *
 * @param tmout タイムアウト時間（相対時間、ms）
 * @param atr   待ち対象オブジェクトの属性（本実装では未使用）
 */
EXPORT void knl_make_wait_reltim( RELTIM tmout, ATR atr )
{
	switch ( knl_ctxtsk->state ) {
	  case TS_READY:
		knl_make_non_ready(knl_ctxtsk);
		knl_ctxtsk->state = TS_WAIT;
		break;
	  case TS_SUSPEND:
		knl_ctxtsk->state = TS_WAITSUS;
		break;
	}
	knl_timer_insert_reltim(&knl_ctxtsk->wtmeb, tmout, (CBACK)knl_wait_release_tmout, knl_ctxtsk);
}

/**
 * @brief 待ちキューに接続された全タスクを E_DLT エラーで待ち解除します。
 *
 * 同期・通信オブジェクトの削除時に使用します。
 *
 * @param wait_queue 対象オブジェクトの待ちキュー
 */
EXPORT void knl_wait_delete( QUEUE *wait_queue )
{
	TCB	*tcb;

	while ( !isQueEmpty(wait_queue) ) {
		tcb = (TCB*)wait_queue->next;
		knl_wait_release(tcb);
		*tcb->wercd = E_DLT;
	}
}

/**
 * @brief 待ちキュー先頭タスクの ID を取得します。
 *
 * @param wait_queue 対象オブジェクトの待ちキュー
 * @return 先頭タスクのタスク ID。待ちキューが空の場合は 0
 */
EXPORT ID knl_wait_tskid( QUEUE *wait_queue )
{
	if ( isQueEmpty(wait_queue) ) {
		return 0;
	}

	return ((TCB*)wait_queue->next)->tskid;
}

/**
 * @brief 実行中タスクを待ち状態にし、タイマキューとオブジェクトの待ちキューに接続します。
 *
 * 待ちエラーコードとして E_TMOUT をあらかじめ設定し、knl_ctxtsk の
 * wid にオブジェクト ID を設定します。オブジェクト属性に TA_TPRI が
 * 指定されていれば優先度順、そうでなければ FIFO 順で待ちキューに
 * つなぎます。tmout が TMO_POL の場合は待ち状態にせず、E_TMOUT の
 * 設定のみ行います（ポーリング）。
 *
 * @param gcb   対象オブジェクトの汎用制御ブロック（GCB）
 * @param tmout タイムアウト時間（ms）。TMO_POL でポーリング、TMO_FEVR で永久待ち
 */
EXPORT void knl_gcb_make_wait( GCB *gcb, TMO tmout )
{
	*knl_ctxtsk->wercd = E_TMOUT;
	if ( tmout != TMO_POL ) {
		knl_ctxtsk->wid = gcb->objid;
		knl_make_wait(tmout, gcb->objatr);
		if ( (gcb->objatr & TA_TPRI) != 0 ) {
			knl_queue_insert_tpri(knl_ctxtsk, &gcb->wait_queue);
		} else {
			QueInsert(&knl_ctxtsk->tskque, &gcb->wait_queue);
		}
	}
}

/**
 * @brief タスク優先度変更時に、待ちキュー内でのタスク位置を調整します。
 *
 * タスクをいったん待ちキューから外し、新しい優先度に従って
 * 優先度順の位置につなぎ直します。オブジェクト属性に TA_TPRI が
 * 指定されている場合にのみ呼び出されます。
 *
 * @param gcb 対象オブジェクトの汎用制御ブロック（GCB）
 * @param tcb 優先度が変更されたタスクの TCB
 */
EXPORT void knl_gcb_change_priority( GCB *gcb, TCB *tcb )
{
	QueRemove(&tcb->tskque);
	knl_queue_insert_tpri(tcb, &gcb->wait_queue);
}

/**
 * @brief tcb を含めたと仮定した場合の待ちキュー先頭タスクを求めます。
 *
 * 待ちキューが空なら tcb を返します。TA_TPRI 指定がなければ
 * 待ちキューの先頭タスクを返し、TA_TPRI 指定があれば待ちキュー先頭
 * タスクと tcb のうち優先度の高い方を返します。
 * （tcb を実際に待ちキューへ挿入することはありません。）
 *
 * @param gcb 対象オブジェクトの汎用制御ブロック（GCB）
 * @param tcb 比較対象のタスクの TCB
 * @return 先頭となるタスクの TCB
 */
EXPORT TCB* knl_gcb_top_of_wait_queue( GCB *gcb, TCB *tcb )
{
	TCB	*q;

	if ( isQueEmpty(&gcb->wait_queue) ) {
		return tcb;
	}

	q = (TCB*)gcb->wait_queue.next;
	if ( (gcb->objatr & TA_TPRI) == 0 ) {
		return q;
	}

	return ( tcb->priority < q->priority )? tcb: q;
}
