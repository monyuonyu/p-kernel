/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.01
 *
 *    Copyright (C) 2006-2020 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2020/05/29.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	task.c
 * @brief	タスク制御
 *
 * タスク実行制御の中核となる共通ルーチンを提供します。
 * TCB テーブルの初期化、休止状態への遷移、実行可能キュー
 * （ready queue）への登録・削除、タスク優先度の変更、
 * 実行可能キューの回転などを行います。
 */

#include "kernel.h"
#include "task.h"
#include "ready_queue.h"
#include "wait.h"
#include "check.h"

#include "../sysdepend/cpu_task.h"

/*
 * タスクディスパッチ禁止状態
 */
Noinit(EXPORT INT	knl_dispatch_disabled);	/* DDS_XXX（task.h 参照） */

/*
 * タスク実行制御
 */
Noinit(EXPORT TCB	*knl_ctxtsk);	/* 実行中のタスク */
Noinit(EXPORT TCB	*knl_schedtsk);	/* 実行すべきタスク */

Noinit(EXPORT RDYQUE	knl_ready_queue);	/* 実行可能キュー */

/*
 * タスク制御情報
 */
Noinit(EXPORT TCB	knl_tcb_table[NUM_TSKID]);	/* タスク制御ブロック */
Noinit(EXPORT QUEUE	knl_free_tcb);	/* 未使用 TCB のキュー */

/**
 * @brief TCB の初期化
 *
 * カーネル起動時に呼ばれ、タスク実行制御情報（knl_ctxtsk、
 * knl_schedtsk、実行可能キュー、ディスパッチ禁止状態）を初期化し、
 * すべての TCB にタスク ID を割り当てて未使用キュー
 * （knl_free_tcb）へ登録します。
 *
 * @retval E_OK		正常終了
 * @retval E_SYS	タスク数の構成値（NUM_TSKID）が不正
 */
EXPORT ER knl_task_initialize( void )
{
	INT	i;
	TCB	*tcb;
	ID	tskid;

	/* システム構成値の確認 */
	if ( NUM_TSKID < 1 ) {
		return E_SYS;
	}

	/* タスク実行制御情報の初期化 */
	knl_ctxtsk = knl_schedtsk = NULL;
	knl_ready_queue_initialize(&knl_ready_queue);
	knl_dispatch_disabled = DDS_ENABLE;

	/* 全 TCB を未使用キューへ登録 */
	QueInit(&knl_free_tcb);
	for ( tcb = knl_tcb_table, i = 0; i < NUM_TSKID; tcb++, i++ ) {
		tskid = ID_TSK(i);
		tcb->tskid = tskid;
		tcb->state = TS_NONEXIST;
#if USE_LEGACY_API && USE_RENDEZVOUS
		tcb->wrdvno = tskid;
#endif

		/* KILL-CHURN-CRASH hardening: self-link every TCB's wait-timer
		 * event block ONCE, here.  knl_tcb_table is Noinit(...) — i.e.
		 * `.noinit (NOLOAD)` (sys/machine.h:101 + the linker scripts), NOT
		 * .bss — so its contents are NOT zero-guaranteed: a cold QEMU boot
		 * happens to give NULL/NULL, a warm reboot guarantees nothing.  That
		 * makes this QueInit MORE necessary, not less.  The defensive
		 * knl_timer_delete
		 * (= QueRemove) now placed in knl_make_dormant and knl_del_tsk
		 * relies on a never-armed node being self-referential, because
		 * QueRemove's no-op guard is `next != entry` — a NULL/NULL node
		 * fails that guard and faults dereferencing NULL->next.  This
		 * single QueInit makes every later delete safe and idempotent,
		 * and is the prerequisite for the timer hygiene in L5/L6. */
		QueInit(&tcb->wtmeb.queue);
		QueInsert(&tcb->tskque, &knl_free_tcb);
	}

	return E_OK;
}

/**
 * @brief タスクを休止状態（DORMANT）へ遷移
 *
 * 休止状態でリセットすべき TCB の各変数（優先度・動作モード・
 * 起床要求数・強制待ち要求数など）を初期化し、タスク起動用の
 * コンテキストを設定します。タスク生成時および終了時に呼ばれます。
 *
 * @param tcb 対象タスクの TCB
 */
EXPORT void knl_make_dormant( TCB *tcb )
{
	/* 休止状態でリセットすべき変数の初期化 */
	tcb->state	= TS_DORMANT;
	tcb->priority	= tcb->bpriority = tcb->ipriority;
	tcb->sysmode	= tcb->isysmode;
	tcb->wupcnt	= 0;
	tcb->suscnt	= 0;

	tcb->klockwait	= FALSE;
	tcb->klocked	= FALSE;

#if USE_DBGSPT && defined(USE_FUNC_TD_INF_TSK)
	tcb->stime	= 0;
	tcb->utime	= 0;
#endif

	tcb->wercd = NULL;

	/* KILL-CHURN-CRASH hardening — timer hygiene at the teardown choke
	 * point.  While armed (tk_wai_sem / tk_dly_tsk with a timeout ->
	 * knl_make_wait_reltim) the TCB-embedded wtmeb is a LIVE node in the
	 * kernel-wide timer queue whose callback is knl_wait_release_tmout(
	 * tcb).  knl_ter_tsk() only cancels it on its TS_WAIT branch, so a
	 * task killed from a non-WAIT instant could leave a stale wtmeb
	 * linked.  knl_make_dormant is reached by EVERY teardown
	 * (tk_ter_tsk, tk_ext_tsk, and task creation), so unlinking here
	 * covers foreign kill/heal churn.
	 *
	 * knl_timer_delete (QueRemove + QueInit) — NOT a bare QueInit, which
	 * would ORPHAN a still-linked node and corrupt its neighbours.  Safe
	 * and idempotent because knl_task_initialize self-links every wtmeb
	 * once, so a never-armed node satisfies QueRemove's `next != entry`
	 * no-op guard.
	 *
	 * HONEST STATUS: this is timer-QUEUE hygiene, not the cure.  The cure
	 * for the observed ring0 #PF is the TS_WAIT early-return at the
	 * callback action site in knl_wait_release_tmout (wait.c). */
	knl_timer_delete(&tcb->wtmeb);

#if USE_MUTEX == 1
	tcb->mtxlist	= NULL;
#endif

	/* タスク起動用コンテキストの設定 */
	knl_setup_context(tcb);
}

/* ------------------------------------------------------------------------ */

/**
 * @brief タスクを実行可能状態（READY）へ遷移
 *
 * タスク状態を TS_READY に更新して実行可能キューへ挿入します。
 * 挿入したタスクが最高優先度となった場合は 'knl_schedtsk' を
 * 更新し、ディスパッチの契機とします。
 *
 * @param tcb 対象タスクの TCB
 */
EXPORT void knl_make_ready( TCB *tcb )
{
	tcb->state = TS_READY;
	if ( knl_ready_queue_insert(&knl_ready_queue, tcb) ) {
		knl_schedtsk = tcb;
	}
}

/**
 * @brief タスクを実行可能状態から外す
 *
 * タスクを実行可能キューから削除します。削除したタスクが
 * 'knl_schedtsk' だった場合は、実行可能キューの最高優先度
 * タスクを新たな 'knl_schedtsk' に設定します。
 *
 * @param tcb 対象タスクの TCB（READY 状態であること）
 * @note tcb->state の更新は行わないため、呼び出し側で本関数から
 *       戻った後に状態を変更してください。
 */
EXPORT void knl_make_non_ready( TCB *tcb )
{
	knl_ready_queue_delete(&knl_ready_queue, tcb);
	if ( knl_schedtsk == tcb ) {
		knl_schedtsk = knl_ready_queue_top(&knl_ready_queue);
	}
}

/**
 * @brief タスク優先度の変更
 *
 * タスクの現在優先度を 'priority'（内部表現）に変更します。
 * READY 状態の場合は実行可能キューへつなぎ直して再スケジュール
 * します。待ち状態で優先度変更フックが定義されている場合は
 * フック（待ちキューの並べ替え等）を実行します。
 *
 * @param tcb      対象タスクの TCB
 * @param priority 新しい優先度（内部表現）
 */
EXPORT void knl_change_task_priority( TCB *tcb, INT priority )
{
	INT	oldpri;

	if ( tcb->state == TS_READY ) {
		/*
		 * 実行可能キューからの削除には TCB の 'priority'
		 * フィールドの値が必要となります。そのため
		 * 'tcb->priority' を変更する前に、実行可能キューから
		 * タスクを削除しておく必要があります。
		 */
		knl_ready_queue_delete(&knl_ready_queue, tcb);
		tcb->priority = (UB)priority;
		knl_ready_queue_insert(&knl_ready_queue, tcb);
		knl_reschedule();
	} else {
		oldpri = tcb->priority;
		tcb->priority = (UB)priority;

		/* タスク優先度変更時のフックルーチンが定義されていれば
		   実行する */
		if ( (tcb->state & TS_WAIT) != 0 && tcb->wspec->chg_pri_hook) {
			(*tcb->wspec->chg_pri_hook)(tcb, oldpri);
		}
	}
}

/**
 * @brief 実行可能キューの回転
 *
 * 指定優先度の実行可能キューを回転（先頭タスクを末尾へ移動）し、
 * 再スケジュールを行います。
 *
 * @param priority 回転対象の優先度（内部表現）
 */
EXPORT void knl_rotate_ready_queue( INT priority )
{
	knl_ready_queue_rotate(&knl_ready_queue, priority);
	knl_reschedule();
}

/**
 * @brief 最高優先度タスクを含む実行可能キューの回転
 *
 * 実行可能キュー中の最高優先度の待ち行列を回転し、
 * 再スケジュールを行います。実行すべきタスクが存在しない場合は
 * 何もしません。
 */
EXPORT void knl_rotate_ready_queue_run( void )
{
	if ( knl_schedtsk != NULL ) {
		knl_ready_queue_rotate(&knl_ready_queue,
				knl_ready_queue_top_priority(&knl_ready_queue));
		knl_reschedule();
	}
}

/* ------------------------------------------------------------------------ */
/*
 *	デバッグ支援機能
 */
#if USE_DBGSPT

#ifdef USE_FUNC_TD_RDY_QUE
/**
 * @brief 実行可能キューの参照
 *
 * 優先度 'pri' の実行可能キューにつながれたタスクの ID を、
 * 先頭から順に最大 'nent' 個まで 'list' に格納します。
 *
 * @param pri  参照する優先度
 * @param list タスク ID を格納する配列
 * @param nent 'list' に格納可能なエントリ数
 * @return 該当キューにつながれたタスク数（nent を超える場合あり）、
 *         または優先度不正時は E_PAR
 */
SYSCALL INT td_rdy_que( PRI pri, ID list[], INT nent )
{
	QUEUE	*q, *tskque;
	INT	n = 0;

	CHECK_PRI(pri);

	BEGIN_DISABLE_INTERRUPT;
	tskque = &knl_ready_queue.tskque[int_priority(pri)];
	for ( q = tskque->next; q != tskque; q = q->next ) {
		if ( n++ < nent ) {
			*(list++) = ((TCB*)q)->tskid;
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_RDY_QUE */

#endif /* USE_DBGSPT */
