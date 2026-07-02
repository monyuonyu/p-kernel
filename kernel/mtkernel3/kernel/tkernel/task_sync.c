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
 * @file	task_sync.c
 * @brief	タスク付属同期機能
 *
 * タスクの強制待ち（tk_sus_tsk / tk_rsm_tsk / tk_frsm_tsk）、
 * 起床待ち（tk_slp_tsk / tk_wup_tsk / tk_can_wup）、
 * タスク遅延（tk_dly_tsk）の各システムコールを実装します。
 */

#include "kernel.h"
#include "wait.h"
#include "check.h"
#include "limits.h"

#ifdef USE_FUNC_TK_SUS_TSK
/**
 * @brief	タスクを強制待ち状態へ移行
 *
 * 対象タスクの強制待ち要求数（suscnt）を1増やします。対象タスクが
 * 実行可能状態なら強制待ち状態（TS_SUSPEND）へ、待ち状態なら
 * 二重待ち状態（TS_WAITSUS）へ移行させます。
 *
 * @param	tskid	対象タスクのID（自タスクは指定不可）
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象タスクが存在しない
 * @retval	E_OBJ	対象タスクが休止状態、または自タスクを指定
 * @retval	E_CTX	ディスパッチ禁止中に実行中タスクを指定
 * @retval	E_QOVR	強制待ち要求数が上限（INT_MAX）を超過
 */
SYSCALL ER tk_sus_tsk( ID tskid )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;

	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	state = (TSTAT)tcb->state;
	if ( !knl_task_alive(state) ) {
		ercd = ( state == TS_NONEXIST )? E_NOEXS: E_OBJ;
		goto error_exit;
	}
	if ( tcb == knl_ctxtsk && knl_dispatch_disabled >= DDS_DISABLE ) {
		ercd = E_CTX;
		goto error_exit;
	}
	if ( tcb->suscnt == INT_MAX ) {
		ercd = E_QOVR;
		goto error_exit;
	}

	/* 強制待ち要求数の更新 */
	++tcb->suscnt;

	/* 強制待ち状態へ移行 */
	if ( state == TS_READY ) {
		knl_make_non_ready(tcb);
		tcb->state = TS_SUSPEND;

	} else if ( state == TS_WAIT ) {
		tcb->state = TS_WAITSUS;
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_SUS_TSK */

#ifdef USE_FUNC_TK_RSM_TSK
/**
 * @brief	強制待ち状態のタスクの再開
 *
 * 対象タスクの強制待ち要求数（suscnt）を1減らし、0になった時点で
 * 強制待ち状態を解除します。二重待ち状態の場合は元の待ち状態に
 * 戻します。
 *
 * @param	tskid	対象タスクのID（自タスクは指定不可）
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象タスクが存在しない
 * @retval	E_OBJ	対象タスクが強制待ち状態でない、または自タスクを指定
 * @retval	E_SYS	タスク状態の不整合（システムエラー）
 */
SYSCALL ER tk_rsm_tsk( ID tskid )
{
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	switch ( tcb->state ) {
	  case TS_NONEXIST:
		ercd = E_NOEXS;
		break;

	  case TS_DORMANT:
	  case TS_READY:
	  case TS_WAIT:
		ercd = E_OBJ;
		break;

	  case TS_SUSPEND:
		if ( --tcb->suscnt == 0 ) {
			knl_make_ready(tcb);
		}
		break;
	  case TS_WAITSUS:
		if ( --tcb->suscnt == 0 ) {
			tcb->state = TS_WAIT;
		}
		break;

	  default:
		ercd = E_SYS;
		break;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_RSM_TSK */

#ifdef USE_FUNC_TK_FRSM_TSK
/**
 * @brief	強制待ち状態のタスクの強制再開
 *
 * 強制待ち要求数（suscnt）の残数にかかわらず0にクリアし、
 * 強制待ち状態を直ちに解除します。
 *
 * @param	tskid	対象タスクのID（自タスクは指定不可）
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象タスクが存在しない
 * @retval	E_OBJ	対象タスクが強制待ち状態でない、または自タスクを指定
 * @retval	E_SYS	タスク状態の不整合（システムエラー）
 */
SYSCALL ER tk_frsm_tsk( ID tskid )
{
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	switch ( tcb->state ) {
	  case TS_NONEXIST:
		ercd = E_NOEXS;
		break;

	  case TS_DORMANT:
	  case TS_READY:
	  case TS_WAIT:
		ercd = E_OBJ;
		break;

	  case TS_SUSPEND:
		tcb->suscnt = 0;
		knl_make_ready(tcb);
		break;
	  case TS_WAITSUS:
		tcb->suscnt = 0;
		tcb->state = TS_WAIT;
		break;

	  default:
		ercd = E_SYS;
		break;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_FRSM_TSK */

/* ------------------------------------------------------------------------ */

/*
 * 起床待ち（TTW_SLP）の待ち仕様定義
 */
LOCAL CONST WSPEC knl_wspec_slp = { TTW_SLP, NULL, NULL };

#ifdef USE_FUNC_TK_SLP_TSK
/**
 * @brief	自タスクを起床待ち状態へ移行
 *
 * 起床要求数（wupcnt）が残っていれば1減らして直ちに復帰します。
 * 残っていなければ、tk_wup_tsk による起床要求があるまで自タスクを
 * 待ち状態にします。
 *
 * @param	tmout	タイムアウト時間（ms）。TMO_POL はポーリング、
 *			TMO_FEVR は永久待ち
 * @retval	E_OK	正常終了（起床された）
 * @retval	E_TMOUT	タイムアウト（TMO_POL 指定時は起床要求なし）
 * @retval	E_PAR	tmout の値が不正
 * @retval	E_CTX	ディスパッチ禁止中の呼び出し
 * @retval	E_RLWAI	待ち状態の強制解除（tk_rel_wai 受付）
 */
SYSCALL ER tk_slp_tsk( TMO tmout )
{
	ER	ercd = E_OK;

	CHECK_TMOUT(tmout);
	CHECK_DISPATCH();

	BEGIN_CRITICAL_SECTION;

	if ( knl_ctxtsk->wupcnt > 0 ) {
		knl_ctxtsk->wupcnt--;
	} else {
		ercd = E_TMOUT;
		if ( tmout != TMO_POL ) {
			knl_ctxtsk->wspec = &knl_wspec_slp;
			knl_ctxtsk->wid = 0;
			knl_ctxtsk->wercd = &ercd;
			knl_make_wait(tmout, TA_NULL);
			QueInit(&knl_ctxtsk->tskque);
		}
	}

	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_SLP_TSK */

#ifdef USE_FUNC_TK_WUP_TSK
/**
 * @brief	タスクの起床
 *
 * 対象タスクが起床待ち状態であれば待ちを解除します。
 * それ以外の状態であれば起床要求数（wupcnt）を1増やして蓄積します。
 *
 * @param	tskid	対象タスクのID（自タスクは指定不可）
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象タスクが存在しない
 * @retval	E_OBJ	対象タスクが休止状態、または自タスクを指定
 * @retval	E_QOVR	起床要求数が上限（INT_MAX）を超過
 */
SYSCALL ER tk_wup_tsk( ID tskid )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;

	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	state = (TSTAT)tcb->state;
	if ( !knl_task_alive(state) ) {
		ercd = ( state == TS_NONEXIST )? E_NOEXS: E_OBJ;

	} else if ( (state & TS_WAIT) != 0 && tcb->wspec == &knl_wspec_slp ) {
		knl_wait_release_ok(tcb);

	} else if ( tcb->wupcnt == INT_MAX ) {
		ercd = E_QOVR;
	} else {
		++tcb->wupcnt;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_WUP_TSK */

#ifdef USE_FUNC_TK_CAN_WUP
/**
 * @brief	起床要求のキャンセル
 *
 * 対象タスクに蓄積された起床要求数（wupcnt）を返し、0にクリアします。
 *
 * @param	tskid	対象タスクのID（TSK_SELF で自タスクを指定可）
 * @return	キャンセルした起床要求数（0以上）、またはエラーコード
 * @retval	E_NOEXS	対象タスクが存在しない
 * @retval	E_OBJ	対象タスクが休止状態
 */
SYSCALL INT tk_can_wup( ID tskid )
{
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_TSKID_SELF(tskid);

	tcb = get_tcb_self(tskid);

	BEGIN_CRITICAL_SECTION;
	switch ( tcb->state ) {
	  case TS_NONEXIST:
		ercd = E_NOEXS;
		break;
	  case TS_DORMANT:
		ercd = E_OBJ;
		break;

	  default:
		ercd = tcb->wupcnt;
		tcb->wupcnt = 0;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_CAN_WUP */

#ifdef USE_FUNC_TK_DLY_TSK
/*
 * タスク遅延（TTW_DLY）の待ち仕様定義
 */
LOCAL CONST WSPEC knl_wspec_dly = { TTW_DLY, NULL, NULL };

/**
 * @brief	自タスクの遅延（時間経過待ち）
 *
 * 指定した相対時間が経過するまで自タスクを待ち状態にします。
 * tk_slp_tsk と異なり、tk_wup_tsk では待ちは解除されません。
 *
 * @param	dlytim	遅延時間（ms）。0 の場合は待たずに復帰
 * @retval	E_OK	正常終了（指定時間が経過）
 * @retval	E_PAR	dlytim の値が不正
 * @retval	E_CTX	ディスパッチ禁止中の呼び出し
 * @retval	E_RLWAI	待ち状態の強制解除（tk_rel_wai 受付）
 */
SYSCALL ER tk_dly_tsk( RELTIM dlytim )
{
	ER	ercd = E_OK;

	CHECK_RELTIM(dlytim);

	CHECK_DISPATCH();

	if ( dlytim > 0 ) {
		BEGIN_CRITICAL_SECTION;
		knl_ctxtsk->wspec = &knl_wspec_dly;
		knl_ctxtsk->wid = 0;
		knl_ctxtsk->wercd = &ercd;
		knl_make_wait_reltim(dlytim, TA_NULL);
		QueInit(&knl_ctxtsk->tskque);
		END_CRITICAL_SECTION;
	}

	return ercd;
}
#endif /* USE_FUNC_TK_DLY_TSK */

