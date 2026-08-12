/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.06A
 *
 *    Copyright (C) 2006-2023 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2023/03.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	timer.c
 * @brief	システムタイマ制御
 *
 * ソフトウェアクロック（システム稼働時間）の管理、タイマイベントキューへの
 * イベント登録、およびシステムタイマ割込みハンドラを提供します。
 */

#include "kernel.h"
#include "task.h"		/* p-kernel 拡張: knl_rotate_ready_queue_run() の宣言 */
#include "timer.h"
#include "../sysdepend/sys_timer.h"

/*
 * 現在時刻（ソフトウェアクロック）
 *	'knl_current_time' は OS 起動からの総稼働時間を示す。
 *	'knl_real_time_ofs' は現在時刻と OS クロック（knl_current_time）
 *	との差を示す。'tk_set_tim()' 等で時刻を設定するときは
 *	'knl_current_time' を変更せず、'knl_current_time' と設定時刻との
 *	差を 'knl_real_time_ofs' に設定する。
 *	したがって 'knl_current_time' は時刻変更の影響を受けず、
 *	単調に増加する。
 */
Noinit(EXPORT LSYSTIM	knl_current_time);	/* システム稼働時間 */
Noinit(EXPORT LSYSTIM	knl_real_time_ofs);	/* 実時刻 - システム稼働時間 */

/*
 * タイマイベントキュー
 */
Noinit(EXPORT QUEUE	knl_timer_queue);

/**
 * @brief システムタイマを開始します。
 *
 * ソフトウェアクロックとタイマイベントキューを初期化し、
 * ハードウェアタイマ割込みを開始します。
 *
 * @return 常に E_OK
 */
EXPORT ER knl_timer_startup( void )
{
	knl_current_time = knl_real_time_ofs = uitoll(0);
	QueInit(&knl_timer_queue);

	/* タイマ割込みの開始 */
	knl_start_hw_timer();

	return E_OK;
}

#if USE_SHUTDOWN
/**
 * @brief システムタイマを停止します。
 *
 * ハードウェアタイマの動作を終了させます。
 */
EXPORT void knl_timer_shutdown( void )
{
	knl_terminate_hw_timer();
}
#endif /* USE_SHUTDOWN */


/**
 * @brief タイマイベントをタイマイベントキューに挿入します。
 *
 * イベント発生時刻の昇順となる位置に挿入します。ABSTIM の周回
 * （ラップアラウンド）を考慮し、現在時刻を基準としたオフセットで
 * 大小を比較します。
 *
 * @param event 挿入するタイマイベントブロック（time 設定済み）
 */
LOCAL void knl_enqueue_tmeb( TMEB *event )
{
	QUEUE	*q;
	ABSTIM	ofs = lltoul(knl_current_time) - ABSTIM_DIFF_MIN;
	/* KILL-CHURN-CRASH hardening (DEFENSIVE — a corrupted timer queue is
	 * not a proven 3.0 state).  If the queue's links are ever cyclic this
	 * walk never reaches the &knl_timer_queue sentinel and the timer IRQ
	 * wedges the machine with interrupts disabled — an unrecoverable hang
	 * that hides its own cause.  Bounding the walk degrades that into a
	 * mis-ordered insert, which is survivable and debuggable. */
	int cnt = 0;

	for ( q = knl_timer_queue.next; q != &knl_timer_queue; q = q->next ) {
		if ( ++cnt > 10000 ) {
			/* timer queue corrupted - break to avoid infinite loop */
			break;
		}
		if ( (ABSTIM)(event->time - ofs) < (ABSTIM)((((TMEB*)q)->time) - ofs) ) {
			break;
		}
	}
	QueInsert(&event->queue, q);
}

/**
 * @brief タイムアウトイベントを設定します。
 *
 * タイムアウト時間 'tmout' の経過後に起動されるよう、タイマイベント
 * 'event' をタイマキューに登録します。タイムアウト時にはコールバック
 * 関数 'callback' が引数 'arg' で呼び出されます。
 * 'tmout' が TMO_FEVR の場合はタイマキューに登録せず、後で
 * knl_timer_delete() が呼ばれた場合に備えてキュー領域のみ初期化します。
 *
 * @param event    タイマイベントブロック
 * @param tmout    タイムアウト時間（ms）。TMO_FEVR(-1) で登録なし
 * @param callback タイムアウト時に呼び出されるコールバック関数
 * @param arg      コールバック関数に渡す引数
 *
 * @note "include/tk/typedef.h" にて
 *       typedef W TMO; / typedef UW RELTIM; / #define TMO_FEVR (-1)
 */
EXPORT void knl_timer_insert( TMEB *event, TMO tmout, CBACK callback, void *arg )
{
	event->callback = callback;
	event->arg = arg;

	if ( tmout == TMO_FEVR ) {
		QueInit(&event->queue);
	} else {
		/* 'tmout' で指定された待ち時間以上の待ちを保証するため、
		   待ち時間に TIMER_PERIOD を加算する */
		event->time = lltoul(knl_current_time) + tmout + TIMER_PERIOD;
		knl_enqueue_tmeb(event);
	}
}

/**
 * @brief タイムアウトイベントを設定します（相対時間 RELTIM 指定版）。
 *
 * knl_timer_insert() と同様ですが、タイムアウト時間を符号なしの
 * 相対時間 RELTIM で指定します（永久待ちの指定はできません）。
 *
 * @param event    タイマイベントブロック
 * @param tmout    タイムアウト時間（相対時間、ms）
 * @param callback タイムアウト時に呼び出されるコールバック関数
 * @param arg      コールバック関数に渡す引数
 */
EXPORT void knl_timer_insert_reltim( TMEB *event, RELTIM tmout, CBACK callback, void *arg )
{
	event->callback = callback;
	event->arg = arg;

	/* 'tmout' で指定された待ち時間以上の待ちを保証するため、
	   待ち時間に TIMER_PERIOD を加算する */
	event->time = lltoul(knl_current_time) + tmout + TIMER_PERIOD;
	knl_enqueue_tmeb(event);
}

/**
 * @brief 時刻指定イベントを設定します。
 *
 * （絶対）時刻 'time' に起動されるよう、タイマイベント 'evt' を
 * タイマキューに登録します。'time' は実時刻ではなく
 * システム稼働時間です。
 *
 * @param evt   タイマイベントブロック
 * @param time  起動時刻（システム稼働時間ベースの絶対時刻）
 * @param cback 時刻到達時に呼び出されるコールバック関数
 * @param arg   コールバック関数に渡す引数
 */
EXPORT void knl_timer_insert_abs( TMEB *evt, ABSTIM time, CBACK cback, void *arg )
{
	evt->callback = cback;
	evt->arg = arg;
	evt->time = time;
	knl_enqueue_tmeb(evt);
}

/* ------------------------------------------------------------------------ */

/**
 * @brief システムタイマ割込みハンドラ
 *
 * ハードウェアタイマにより TIMER_PERIOD ミリ秒間隔で起動されます。
 * ソフトウェアクロックを更新し、発生時刻に達したタイマイベントの
 * コールバックを順に実行します。デバッガサポート機能が有効な場合は、
 * 実行中タスクの実行時間（システムモード／ユーザモード）も集計します。
 */

EXPORT void knl_timer_handler( void )
{
	TMEB	*event;
	ABSTIM	cur;

	knl_clear_hw_timer_interrupt();		/* タイマ割込みのクリア */

	BEGIN_CRITICAL_SECTION;
	knl_current_time = ll_add(knl_current_time, uitoll(TIMER_PERIOD));
	cur = lltoul(knl_current_time);

#if USE_DBGSPT && defined(USE_FUNC_TD_INF_TSK)
	if ( knl_ctxtsk != NULL ) {
		/* 実行中タスク */
		if ( knl_ctxtsk->sysmode > 0 ) {
			knl_ctxtsk->stime += TIMER_PERIOD;
		} else {
			knl_ctxtsk->utime += TIMER_PERIOD;
		}
	}
#endif

	/* 発生時刻を過ぎたイベントの実行 */
	/* KILL-CHURN-CRASH hardening (DEFENSIVE, same class as the bound in
	 * knl_enqueue_tmeb): this drain loop runs inside the timer IRQ with
	 * interrupts disabled.  If a damaged queue ever makes it non-
	 * terminating the system hangs silently; a bound turns that into a
	 * deferred event.  1000 is far above any legitimate number of timer
	 * events expiring in a single tick. */
	int timer_loop_cnt = 0;
	while ( !isQueEmpty(&knl_timer_queue) ) {
		event = (TMEB*)knl_timer_queue.next;

		if ( !knl_abstim_reached(cur, event->time) ) {
			break;
		}

		if ( ++timer_loop_cnt > 1000 ) {
			/* timer queue corrupted - break to avoid infinite loop */
			break;
		}

		QueRemove(&event->queue);
		/* KILL-CHURN-CRASH hardening (DEFENSIVE): return the dequeued
		 * node to the self-linked state so a later knl_timer_delete on
		 * the same TMEB (e.g. from knl_wait_release running inside the
		 * callback below) is a no-op instead of a write through stale
		 * neighbour pointers. */
		QueInit(&event->queue);
		if ( event->callback != NULL ) {
			(*event->callback)(event->arg);
		}
	}

	/* p-kernel 拡張: ラウンドロビンのタイムスライス管理
	 *	実行中タスクが SCHED_RR の場合、tick ごとに残りスライスを
	 *	減算し、使い切ったら実行可能キューの同一優先度内で回転
	 *	させる。実際のタスク切替は END_CRITICAL_SECTION の
	 *	ディスパッチ判定で起きる。 */
	if ( knl_ctxtsk != NULL && knl_ctxtsk->sched_policy == SCHED_RR ) {
		if ( knl_ctxtsk->remaining_slice > 0 ) {
			knl_ctxtsk->remaining_slice--;
		}
		if ( knl_ctxtsk->remaining_slice == 0 ) {
			knl_ctxtsk->remaining_slice = knl_ctxtsk->time_slice;
			knl_rotate_ready_queue_run();
		}
	}

	END_CRITICAL_SECTION;

	knl_end_of_hw_timer_interrupt();		/* タイマ割込みの終了処理 */
}
