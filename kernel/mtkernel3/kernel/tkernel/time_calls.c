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
 * @file	time_calls.c
 * @brief	時間管理機能
 *
 * システム時刻の設定・参照（tk_set_utc / tk_get_utc / tk_set_tim /
 * tk_get_tim / tk_get_otm）、周期ハンドラ、アラームハンドラの
 * 各システムコールとデバッガサポート機能を実装します。
 */

#include "kernel.h"
#include "timer.h"
#include "wait.h"
#include "check.h"
#include "time_calls.h"
#include "../sysdepend/sys_timer.h"

/* ------------------------------------------------------------------------ */
/*
 *	時刻管理
 */
#if USE_TIMEMANAGEMENT

#ifdef USE_FUNC_TK_SET_UTC
/**
 * @brief	システム時刻の設定（UTC）
 *
 * 指定された UTC 時刻（1970年1月1日0時0分0秒からのミリ秒）と
 * システム稼働時間との差分を実時刻オフセットとして記録します。
 *
 * @param	pk_tim	設定する時刻（UTC、ms 単位）
 * @retval	E_OK	正常終了
 * @retval	E_PAR	pk_tim->hi が負（時刻値が不正）
 */
SYSCALL ER tk_set_utc( CONST SYSTIM *pk_tim )
{
	CHECK_PAR(pk_tim->hi >= 0);

	BEGIN_CRITICAL_SECTION;
	knl_real_time_ofs = ll_sub(knl_toLSYSTIM(pk_tim), knl_current_time);
	END_CRITICAL_SECTION;

	return E_OK;
}
#endif /* USE_FUNC_TK_SET_UTC */

#ifdef USE_FUNC_TK_GET_UTC
/**
 * @brief	システム時刻の参照（UTC）
 *
 * システム稼働時間に実時刻オフセットを加えた現在時刻（UTC）を
 * 返します。
 *
 * @param	pk_tim	時刻の格納先（UTC、ms 単位）
 * @retval	E_OK	正常終了
 */
SYSCALL ER tk_get_utc( SYSTIM *pk_tim )
{
	BEGIN_CRITICAL_SECTION;
	*pk_tim = knl_toSYSTIM(ll_add(knl_current_time, knl_real_time_ofs));
	END_CRITICAL_SECTION;

	return E_OK;
}
#endif /* USE_FUNC_TK_GET_UTC */

#ifdef USE_FUNC_TK_SET_TIM
/**
 * @brief	システム時刻の設定（TRON 時間）
 *
 * TRON 時間（1985年1月1日0時0分0秒 GMT 基準）で指定された時刻を
 * UTC に換算し、実時刻オフセットとして記録します。
 *
 * @param	pk_tim	設定する時刻（TRON 時間、ms 単位）
 * @retval	E_OK	正常終了
 * @retval	E_PAR	pk_tim->hi が負（時刻値が不正）
 */
SYSCALL ER tk_set_tim( CONST SYSTIM *pk_tim )
{
	LSYSTIM		utc_time;

	CHECK_PAR(pk_tim->hi >= 0);
	utc_time = ll_add(knl_toLSYSTIM(pk_tim), DIFF_TRON_UTC);

	BEGIN_CRITICAL_SECTION;
	knl_real_time_ofs = ll_sub(utc_time, knl_current_time);
	END_CRITICAL_SECTION;

	return E_OK;
}
#endif /* USE_FUNC_TK_SET_TIM */

#ifdef USE_FUNC_TK_GET_TIM
/**
 * @brief	システム時刻の参照（TRON 時間）
 *
 * 現在時刻を TRON 時間（1985年1月1日0時0分0秒 GMT 基準）に
 * 換算して返します。
 *
 * @param	pk_tim	時刻の格納先（TRON 時間、ms 単位）
 * @retval	E_OK	正常終了
 */
SYSCALL ER tk_get_tim( SYSTIM *pk_tim )
{
	LSYSTIM		utc_time;

	BEGIN_CRITICAL_SECTION;
	utc_time = ll_add(knl_current_time, knl_real_time_ofs);
	END_CRITICAL_SECTION;

	*pk_tim = knl_toSYSTIM(ll_sub(utc_time, DIFF_TRON_UTC));

	return E_OK;
}
#endif /* USE_FUNC_TK_GET_TIM */

#ifdef USE_FUNC_TK_GET_OTM
/**
 * @brief	システム稼働時間の参照
 *
 * システム起動からの経過時間（実時刻オフセットを含まない値）を
 * 返します。
 *
 * @param	pk_tim	稼働時間の格納先（ms 単位）
 * @retval	E_OK	正常終了
 */
SYSCALL ER tk_get_otm( SYSTIM *pk_tim )
{
	BEGIN_CRITICAL_SECTION;
	*pk_tim = knl_toSYSTIM(knl_current_time);
	END_CRITICAL_SECTION;

	return E_OK;
}
#endif /* USE_FUNC_TK_GET_OTM */

#if USE_DBGSPT
#ifdef USE_FUNC_TD_GET_TIM
/**
 * @brief	システム時刻の参照（デバッガサポート機能）
 *
 * 現在時刻（UTC）に加えて、ハードウェアタイマから取得した
 * ミリ秒未満の端数（ns 単位）も返します。
 *
 * @param	tim	時刻の格納先（UTC、ms 単位）
 * @param	ofs	ミリ秒未満の端数の格納先（ns 単位）
 * @retval	E_OK	正常終了
 */
SYSCALL ER td_get_tim( SYSTIM *tim, UW *ofs )
{
	BEGIN_DISABLE_INTERRUPT;
	*ofs = knl_get_hw_timer_nsec();
	*tim = knl_toSYSTIM(ll_add(knl_current_time, knl_real_time_ofs));
	END_DISABLE_INTERRUPT;

	return E_OK;
}
#endif /* USE_FUNC_TD_GET_TIM */

#ifdef USE_FUNC_TD_GET_OTM
/**
 * @brief	システム稼働時間の参照（デバッガサポート機能）
 *
 * システム稼働時間に加えて、ハードウェアタイマから取得した
 * ミリ秒未満の端数（ns 単位）も返します。
 *
 * @param	tim	稼働時間の格納先（ms 単位）
 * @param	ofs	ミリ秒未満の端数の格納先（ns 単位）
 * @retval	E_OK	正常終了
 */
SYSCALL ER td_get_otm( SYSTIM *tim, UW *ofs )
{
	BEGIN_DISABLE_INTERRUPT;
	*ofs = knl_get_hw_timer_nsec();
	*tim = knl_toSYSTIM(knl_current_time);
	END_DISABLE_INTERRUPT;

	return E_OK;
}
#endif /* USE_FUNC_TD_GET_OTM */
#endif /* USE_DBGSPT */
#endif /* USE_TIMEMANAGEMENT */


/* ------------------------------------------------------------------------ */
/*
 *	周期ハンドラ
 */

#if USE_CYCLICHANDLER

Noinit(EXPORT CYCCB knl_cyccb_table[NUM_CYCID]);	/* 周期ハンドラ管理ブロックテーブル */
Noinit(EXPORT QUEUE	knl_free_cyccb);	/* 未使用管理ブロックのキュー（FreeQue） */


/**
 * @brief	周期ハンドラ管理ブロックの初期化
 *
 * すべての周期ハンドラ管理ブロックを未登録状態にして FreeQue に
 * 登録します。カーネル初期化時に呼び出されます。
 *
 * @retval	E_OK	正常終了
 * @retval	E_SYS	周期ハンドラの最大数（NUM_CYCID）が1未満
 */
EXPORT ER knl_cyclichandler_initialize( void )
{
	CYCCB	*cyccb, *end;

	/* システム情報の取得 */
	if ( NUM_CYCID < 1 ) {
		return E_SYS;
	}

	/* すべての管理ブロックを FreeQue に登録 */
	QueInit(&knl_free_cyccb);
	end = knl_cyccb_table + NUM_CYCID;
	for ( cyccb = knl_cyccb_table; cyccb < end; cyccb++ ) {
		cyccb->cychdr = NULL; /* 未登録ハンドラ */
		QueInsert((QUEUE*)cyccb, &knl_free_cyccb);
	}

	return E_OK;
}


/**
 * @brief	周期ハンドラの起動ルーチン
 *
 * タイマ割込みからコールバックとして呼び出され、次回起動時刻を
 * タイマイベントキューへ登録したうえでユーザの周期ハンドラを実行
 * します。ハンドラ実行中は TIMER_INTLEVEL までの割込みネストを
 * 許可します。
 *
 * @param	cyccb	起動する周期ハンドラの管理ブロック
 */
EXPORT void knl_call_cychdr( CYCCB *cyccb )
{
	/* 次回起動時刻の設定 */
	knl_cyc_timer_insert(cyccb, knl_cyc_next_time(cyccb));

	/* 周期ハンドラの実行（割込みネストを許可） */
	ENABLE_INTERRUPT_UPTO(TIMER_INTLEVEL);
	CallUserHandlerP1(cyccb->exinf, cyccb->cychdr, cyccb);
	DISABLE_INTERRUPT;
}

/**
 * @brief	周期ハンドラの即時起動
 *
 * tk_cre_cyc で起動位相 0 かつ TA_STA 指定の場合に、次回起動時刻を
 * 登録したうえで周期ハンドラをタスク独立部として直ちに実行します
 * （割込み禁止のまま実行）。
 *
 * @param	cyccb	起動する周期ハンドラの管理ブロック
 */
LOCAL void knl_immediate_call_cychdr( CYCCB *cyccb )
{
	/* 次回起動時刻の設定 */
	knl_cyc_timer_insert(cyccb, knl_cyc_next_time(cyccb));

	/* 周期ハンドラをタスク独立部として実行
	   （割込み禁止のまま） */
	ENTER_TASK_INDEPENDENT;
	CallUserHandlerP1(cyccb->exinf, cyccb->cychdr, cyccb);
	LEAVE_TASK_INDEPENDENT;
}

/**
 * @brief	周期ハンドラの生成
 *
 * FreeQue から管理ブロックを獲得して周期ハンドラを生成します。
 * TA_STA 属性が指定されていれば動作状態で生成し、起動位相
 * （cycphs）が 0 の場合はハンドラを直ちに実行します。
 *
 * @param	pk_ccyc	周期ハンドラ生成情報
 * @return	生成した周期ハンドラのID、またはエラーコード
 * @retval	E_LIMIT	周期ハンドラ数が上限（NUM_CYCID）を超過
 * @retval	E_RSATR	不正な属性を指定
 * @retval	E_PAR	cychdr が NULL、または cyctim の値が不正
 */
SYSCALL ID tk_cre_cyc( CONST T_CCYC *pk_ccyc )
{
#if CHK_RSATR
	const ATR VALID_CYCATR = {
		 TA_HLNG
		|TA_STA
		|TA_PHS
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
	};
#endif
	CYCCB	*cyccb;
	ABSTIM	tm;
	ER	ercd = E_OK;

	CHECK_RSATR(pk_ccyc->cycatr, VALID_CYCATR);
	CHECK_PAR(pk_ccyc->cychdr != NULL);
	CHECK_PAR(pk_ccyc->cyctim > 0);
	CHECK_RELTIM(pk_ccyc->cyctim);

	BEGIN_CRITICAL_SECTION;
	/* FreeQue から管理ブロックを獲得 */
	cyccb = (CYCCB*)QueRemoveNext(&knl_free_cyccb);
	if ( cyccb == NULL ) {
		ercd = E_LIMIT;
		goto error_exit;
	}

	/* 管理ブロックの初期化 */
	cyccb->exinf   = pk_ccyc->exinf;
	cyccb->cycatr  = pk_ccyc->cycatr;
	cyccb->cychdr  = pk_ccyc->cychdr;
	cyccb->cyctim  = pk_ccyc->cyctim;
#if USE_OBJECT_NAME
	if ( (pk_ccyc->cycatr & TA_DSNAME) != 0 ) {
		knl_strncpy((char*)cyccb->name, (char*)pk_ccyc->dsname, OBJECT_NAME_LENGTH);
	}
#endif

	/* 初回起動時刻
	 *	指定時間経過後のハンドラ起動を保証するため、
	 *	TIMER_PERIOD を加算する。
	 */
	tm = lltoul(knl_current_time) + pk_ccyc->cycphs + TIMER_PERIOD;

	if ( (pk_ccyc->cycatr & TA_STA) != 0 ) {
		/* 周期ハンドラの動作開始 */
		cyccb->cycstat = TCYC_STA;

		if ( pk_ccyc->cycphs == 0 ) {
			/* 即時実行 */
			cyccb->cyctmeb.time = tm;
			knl_immediate_call_cychdr(cyccb);
		} else {
			/* タイマイベントキューへ登録 */
			knl_cyc_timer_insert(cyccb, tm);
		}
	} else {
		/* 起動時刻の初期化のみ行う */
		cyccb->cycstat = TCYC_STP;
		cyccb->cyctmeb.time = tm;
	}

	ercd = ID_CYC(cyccb - knl_cyccb_table);

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_DEL_CYC
/**
 * @brief	周期ハンドラの削除
 *
 * 動作中であればタイマイベントキューから削除したうえで、
 * 管理ブロックを FreeQue に返却します。
 *
 * @param	cycid	削除する周期ハンドラのID
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象の周期ハンドラが存在しない
 * @retval	E_ID	cycid の値が不正
 */
SYSCALL ER tk_del_cyc( ID cycid )
{
	CYCCB	*cyccb;
	ER	ercd = E_OK;

	CHECK_CYCID(cycid);

	cyccb = get_cyccb(cycid);

	BEGIN_CRITICAL_SECTION;
	if ( cyccb->cychdr == NULL ) { /* 未登録ハンドラ */
		ercd = E_NOEXS;
	} else {
		if ( (cyccb->cycstat & TCYC_STA) != 0 ) {
			/* タイマイベントキューから削除 */
			knl_timer_delete(&cyccb->cyctmeb);
		}

		/* FreeQue へ返却 */
		QueInsert((QUEUE*)cyccb, &knl_free_cyccb);
		cyccb->cychdr = NULL; /* 未登録ハンドラ */
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_CYC */

#ifdef USE_FUNC_TK_STA_CYC
/**
 * @brief	周期ハンドラの動作開始
 *
 * 周期ハンドラを動作状態にします。TA_PHS 属性の場合は生成時からの
 * 起動位相を保存したまま動作を再開し、それ以外の場合は現在時刻を
 * 基準に起動周期を再設定します。
 *
 * @param	cycid	対象の周期ハンドラのID
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象の周期ハンドラが存在しない
 * @retval	E_ID	cycid の値が不正
 */
SYSCALL ER tk_sta_cyc( ID cycid )
{
	CYCCB	*cyccb;
	ABSTIM	tm, cur;
	ER	ercd = E_OK;

	CHECK_CYCID(cycid);

	cyccb = get_cyccb(cycid);

	BEGIN_CRITICAL_SECTION;
	if ( cyccb->cychdr == NULL ) { /* 未登録ハンドラ */
		ercd = E_NOEXS;
		goto error_exit;
	}

	cur = lltoul(knl_current_time);

	if ( (cyccb->cycatr & TA_PHS) != 0 ) {
		/* 起動位相を保存して継続 */
		if ( (cyccb->cycstat & TCYC_STA) == 0 ) {
			/* 周期ハンドラの動作開始 */
			tm = cyccb->cyctmeb.time;
			if ( knl_abstim_reached(cur, tm) ) {
				tm = knl_cyc_next_time(cyccb);
			}
			knl_cyc_timer_insert(cyccb, tm);
		}
	} else {
		/* 起動周期を再設定 */
		if ( (cyccb->cycstat & TCYC_STA) != 0 ) {
			/* いったん停止する */
			knl_timer_delete(&cyccb->cyctmeb);
		}

		/* 初回起動時刻
		 *	初回起動時刻を TIMER_PERIOD で調整する。
		 *	TIMER_PERIOD はタイマ割込み間隔（ミリ秒）。
		 */
		tm = cur + cyccb->cyctim + TIMER_PERIOD;

		/* 周期ハンドラの動作開始 */
		knl_cyc_timer_insert(cyccb, tm);
	}
	cyccb->cycstat |= TCYC_STA;

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_STA_CYC */

#ifdef USE_FUNC_TK_STP_CYC
/**
 * @brief	周期ハンドラの動作停止
 *
 * 動作中であればタイマイベントキューから削除し、
 * 周期ハンドラを停止状態にします。
 *
 * @param	cycid	対象の周期ハンドラのID
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象の周期ハンドラが存在しない
 * @retval	E_ID	cycid の値が不正
 */
SYSCALL ER tk_stp_cyc( ID cycid )
{
	CYCCB	*cyccb;
	ER	ercd = E_OK;

	CHECK_CYCID(cycid);

	cyccb = get_cyccb(cycid);

	BEGIN_CRITICAL_SECTION;
	if ( cyccb->cychdr == NULL ) { /* 未登録ハンドラ */
		ercd = E_NOEXS;
	} else {
		if ( (cyccb->cycstat & TCYC_STA) != 0 ) {
			/* 周期ハンドラの停止 */
			knl_timer_delete(&cyccb->cyctmeb);
		}
		cyccb->cycstat &= ~TCYC_STA;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_STP_CYC */

#ifdef USE_FUNC_TK_REF_CYC
/**
 * @brief	周期ハンドラ状態の参照
 *
 * 拡張情報、次回起動までの残り時間、動作状態を pk_rcyc に返します。
 *
 * @param	cycid	対象の周期ハンドラのID
 * @param	pk_rcyc	周期ハンドラ状態の格納先
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象の周期ハンドラが存在しない
 * @retval	E_ID	cycid の値が不正
 */
SYSCALL ER tk_ref_cyc( ID cycid, T_RCYC* pk_rcyc )
{
	CYCCB	*cyccb;
	ABSTIM	tm, cur;
	ER	ercd = E_OK;

	CHECK_CYCID(cycid);

	cyccb = get_cyccb(cycid);

	BEGIN_CRITICAL_SECTION;
	if ( cyccb->cychdr == NULL ) { /* 未登録ハンドラ */
		ercd = E_NOEXS;
	} else {
		tm = cyccb->cyctmeb.time;
		cur = lltoul(knl_current_time);
		if ( (cyccb->cycstat & TCYC_STA) == 0 ) {
			if ( knl_abstim_reached(cur, tm) ) {
				tm = knl_cyc_next_time(cyccb);
			}
		}
		if ( knl_abstim_reached(cur + TIMER_PERIOD, tm) ) {
			tm = 0;
		}
		else {
			tm -= (cur + TIMER_PERIOD);
		}

		pk_rcyc->exinf   = cyccb->exinf;
		pk_rcyc->lfttim  = tm;
		pk_rcyc->cycstat = cyccb->cycstat;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REF_CYC */

#if USE_DBGSPT

#if USE_OBJECT_NAME
/**
 * @brief	周期ハンドラのオブジェクト名の取得
 *
 * TA_DSNAME 属性付きで生成された周期ハンドラの名前へのポインタを
 * 返します。
 *
 * @param	id	対象の周期ハンドラのID
 * @param	name	名前へのポインタの格納先
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象の周期ハンドラが存在しない
 * @retval	E_OBJ	TA_DSNAME 属性が指定されていない
 * @retval	E_ID	id の値が不正
 */
EXPORT ER knl_cyclichandler_getname(ID id, UB **name)
{
	CYCCB	*cyccb;
	ER	ercd = E_OK;

	CHECK_CYCID(id);

	BEGIN_DISABLE_INTERRUPT;
	cyccb = get_cyccb(id);
	if ( cyccb->cychdr == NULL ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (cyccb->cycatr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = cyccb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */

#ifdef USE_FUNC_TD_LST_CYC
/**
 * @brief	周期ハンドラIDリストの参照（デバッガサポート機能）
 *
 * 使用中の周期ハンドラのIDを list に列挙します。nent を超える分は
 * 格納されませんが、総数は戻り値で返します。
 *
 * @param	list	IDリストの格納先配列
 * @param	nent	list に格納可能な最大数
 * @return	使用中の周期ハンドラの総数
 */
SYSCALL INT td_lst_cyc( ID list[], INT nent )
{
	CYCCB	*cyccb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_cyccb_table + NUM_CYCID;
	for ( cyccb = knl_cyccb_table; cyccb < end; cyccb++ ) {
		/* 未登録ハンドラは読み飛ばす */
		if ( cyccb->cychdr == NULL ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = ID_CYC(cyccb - knl_cyccb_table);
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_CYC */

#ifdef USE_FUNC_TD_REF_CYC
/**
 * @brief	周期ハンドラ状態の参照（デバッガサポート機能）
 *
 * 拡張情報、次回起動までの残り時間、動作状態を pk_rcyc に返します。
 *
 * @param	cycid	対象の周期ハンドラのID
 * @param	pk_rcyc	周期ハンドラ状態の格納先
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象の周期ハンドラが存在しない
 * @retval	E_ID	cycid の値が不正
 */
SYSCALL ER td_ref_cyc( ID cycid, TD_RCYC* pk_rcyc )
{
	CYCCB	*cyccb;
	ABSTIM	tm, cur;
	ER	ercd = E_OK;

	CHECK_CYCID(cycid);

	cyccb = get_cyccb(cycid);

	BEGIN_DISABLE_INTERRUPT;
	if ( cyccb->cychdr == NULL ) { /* 未登録ハンドラ */
		ercd = E_NOEXS;
	} else {
		tm = cyccb->cyctmeb.time;
		cur = lltoul(knl_current_time);
		if ( (cyccb->cycstat & TCYC_STA) == 0 ) {
			if ( knl_abstim_reached(cur, tm) ) {
				tm = knl_cyc_next_time(cyccb);
			}
		}
		if ( knl_abstim_reached(cur + TIMER_PERIOD, tm) ) {
			tm = 0;
		}
		else {
			tm -= (cur + TIMER_PERIOD);
		}

		pk_rcyc->exinf   = cyccb->exinf;
		pk_rcyc->lfttim  = lltoul(tm);
		pk_rcyc->cycstat = cyccb->cycstat;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_CYC */

#endif /* USE_DBGSPT */
#endif /* USE_CYCLICHANDLER */

/* ------------------------------------------------------------------------ */
/*
 *	アラームハンドラ
 */

#if USE_ALARMHANDLER

Noinit(EXPORT ALMCB knl_almcb_table[NUM_ALMID]);	/* アラームハンドラ管理ブロックテーブル */
Noinit(EXPORT QUEUE	knl_free_almcb);	/* 未使用管理ブロックのキュー（FreeQue） */


/**
 * @brief	アラームハンドラ管理ブロックの初期化
 *
 * すべてのアラームハンドラ管理ブロックを未登録状態にして FreeQue
 * に登録します。カーネル初期化時に呼び出されます。
 *
 * @retval	E_OK	正常終了
 * @retval	E_SYS	アラームハンドラの最大数（NUM_ALMID）が1未満
 */
EXPORT ER knl_alarmhandler_initialize( void )
{
	ALMCB	*almcb, *end;

	/* システム情報の取得 */
	if ( NUM_ALMID < 1 ) {
		return E_SYS;
	}

	/* すべての管理ブロックを FreeQue に登録 */
	QueInit(&knl_free_almcb);
	end = knl_almcb_table + NUM_ALMID;
	for ( almcb = knl_almcb_table; almcb < end; almcb++ ) {
		almcb->almhdr = NULL; /* 未登録ハンドラ */
		QueInsert((QUEUE*)almcb, &knl_free_almcb);
	}

	return E_OK;
}


/**
 * @brief	アラームハンドラの起動ルーチン
 *
 * タイマ割込みからコールバックとして呼び出され、アラームハンドラを
 * 停止状態にしたうえでユーザのハンドラを実行します。ハンドラ実行中
 * は TIMER_INTLEVEL までの割込みネストを許可します。
 *
 * @param	almcb	起動するアラームハンドラの管理ブロック
 */
EXPORT void knl_call_almhdr( ALMCB *almcb )
{
	almcb->almstat &= ~TALM_STA;

	/* アラームハンドラの実行（割込みネストを許可） */
	ENABLE_INTERRUPT_UPTO(TIMER_INTLEVEL);
	CallUserHandlerP1(almcb->exinf, almcb->almhdr, almcb);
	DISABLE_INTERRUPT;
}


/**
 * @brief	アラームハンドラの生成
 *
 * FreeQue から管理ブロックを獲得してアラームハンドラを生成します。
 * 生成直後は停止状態（TALM_STP）であり、tk_sta_alm で起動時刻を
 * 設定します。
 *
 * @param	pk_calm	アラームハンドラ生成情報
 * @return	生成したアラームハンドラのID、またはエラーコード
 * @retval	E_LIMIT	アラームハンドラ数が上限（NUM_ALMID）を超過
 * @retval	E_RSATR	不正な属性を指定
 * @retval	E_PAR	almhdr が NULL
 */
SYSCALL ID tk_cre_alm( CONST T_CALM *pk_calm )
{
#if CHK_RSATR
	const ATR VALID_ALMATR = {
		 TA_HLNG
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
	};
#endif
	ALMCB	*almcb;
	ER	ercd = E_OK;

	CHECK_RSATR(pk_calm->almatr, VALID_ALMATR);
	CHECK_PAR(pk_calm->almhdr != NULL);

	BEGIN_CRITICAL_SECTION;
	/* FreeQue から管理ブロックを獲得 */
	almcb = (ALMCB*)QueRemoveNext(&knl_free_almcb);
	if ( almcb == NULL ) {
		ercd = E_LIMIT;
		goto error_exit;
	}

	/* 管理ブロックの初期化 */
	almcb->exinf   = pk_calm->exinf;
	almcb->almatr  = pk_calm->almatr;
	almcb->almhdr  = pk_calm->almhdr;
	almcb->almstat = TALM_STP;
#if USE_OBJECT_NAME
	if ( (pk_calm->almatr & TA_DSNAME) != 0 ) {
		knl_strncpy((char*)almcb->name, (char*)pk_calm->dsname, OBJECT_NAME_LENGTH);
	}
#endif

	ercd = ID_ALM(almcb - knl_almcb_table);

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_DEL_ALM
/**
 * @brief	アラームハンドラの削除
 *
 * 動作中であればタイマイベントキューから削除したうえで、
 * 管理ブロックを FreeQue に返却します。
 *
 * @param	almid	削除するアラームハンドラのID
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象のアラームハンドラが存在しない
 * @retval	E_ID	almid の値が不正
 */
SYSCALL ER tk_del_alm( ID almid )
{
	ALMCB	*almcb;
	ER	ercd = E_OK;

	CHECK_ALMID(almid);

	almcb = get_almcb(almid);

	BEGIN_CRITICAL_SECTION;
	if ( almcb->almhdr == NULL ) { /* 未登録ハンドラ */
		ercd = E_NOEXS;
	} else {
		if ( (almcb->almstat & TALM_STA) != 0 ) {
			/* タイマイベントキューから削除 */
			knl_timer_delete(&almcb->almtmeb);
		}

		/* FreeQue へ返却 */
		QueInsert((QUEUE*)almcb, &knl_free_almcb);
		almcb->almhdr = NULL; /* 未登録ハンドラ */
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_ALM */

/**
 * @brief	アラームハンドラの即時起動
 *
 * tk_sta_alm で起動時刻 0 が指定された場合に、アラームハンドラを
 * タスク独立部として直ちに実行します（割込み禁止のまま実行）。
 *
 * @param	almcb	起動するアラームハンドラの管理ブロック
 */
LOCAL void knl_immediate_call_almhdr( ALMCB *almcb )
{
	almcb->almstat &= ~TALM_STA;

	/* アラームハンドラをタスク独立部として実行
	   （割込み禁止のまま） */
	ENTER_TASK_INDEPENDENT;
	CallUserHandlerP1(almcb->exinf, almcb->almhdr, almcb);
	LEAVE_TASK_INDEPENDENT;
}

/**
 * @brief	アラームハンドラの動作開始
 *
 * 現在時刻から almtim 経過後にアラームハンドラが起動されるよう
 * 設定します。既に起動時刻が設定されている場合はいったん解除して
 * 再設定します。almtim が 0 の場合はハンドラを直ちに実行します。
 *
 * @param	almid	対象のアラームハンドラのID
 * @param	almtim	起動までの相対時間（ms）
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象のアラームハンドラが存在しない
 * @retval	E_ID	almid の値が不正
 * @retval	E_PAR	almtim の値が不正
 */
SYSCALL ER tk_sta_alm( ID almid, RELTIM almtim )
{
	ALMCB	*almcb;
	ER	ercd = E_OK;

	CHECK_ALMID(almid);
	CHECK_RELTIM(almtim);

	almcb = get_almcb(almid);

	BEGIN_CRITICAL_SECTION;
	if ( almcb->almhdr == NULL ) { /* 未登録ハンドラ */
		ercd = E_NOEXS;
		goto error_exit;
	}

	if ( (almcb->almstat & TALM_STA) != 0 ) {
		/* 現在の設定を解除 */
		knl_timer_delete(&almcb->almtmeb);
	}

	if ( almtim > 0 ) {
		/* タイマイベントキューへ登録 */
		knl_alm_timer_insert(almcb, almtim);
		almcb->almstat |= TALM_STA;
	} else {
		/* 即時実行 */
		knl_immediate_call_almhdr(almcb);
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_STP_ALM
/**
 * @brief	アラームハンドラの動作停止
 *
 * 起動時刻の設定を解除し、アラームハンドラを停止状態にします。
 * 既に停止状態の場合は何もしません。
 *
 * @param	almid	対象のアラームハンドラのID
 * @retval	E_OK	正常終了
 * @retval	E_NOEXS	対象のアラームハンドラが存在しない
 * @retval	E_ID	almid の値が不正
 */
SYSCALL ER tk_stp_alm( ID almid )
{
	ALMCB	*almcb;
	ER	ercd = E_OK;

	CHECK_ALMID(almid);

	almcb = get_almcb(almid);

	BEGIN_CRITICAL_SECTION;
	if ( almcb->almhdr == NULL ) { /* 未登録ハンドラ */
		ercd = E_NOEXS;
	} else {
		if ( (almcb->almstat & TALM_STA) != 0 ) {
			/* アラームハンドラの停止 */
			knl_timer_delete(&almcb->almtmeb);
			almcb->almstat &= ~TALM_STA;
		}
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_STP_ALM */

#ifdef USE_FUNC_TK_REF_ALM
/*
 * Refer alarm handler state
 */
SYSCALL ER tk_ref_alm( ID almid, T_RALM *pk_ralm )
{
	ALMCB	*almcb;
	ABSTIM	tm, cur;
	ER	ercd = E_OK;

	CHECK_ALMID(almid);

	almcb = get_almcb(almid);

	BEGIN_CRITICAL_SECTION;
	if ( almcb->almhdr == NULL ) { /* Unregistered handler */
		ercd = E_NOEXS;
	} else {
		cur = lltoul(knl_current_time);
		if ( (almcb->almstat & TALM_STA) != 0 ) {
			tm = almcb->almtmeb.time;
			if ( knl_abstim_reached(cur + TIMER_PERIOD, tm) ) {
				tm = 0;
			}
			else {
				tm -= (cur + TIMER_PERIOD);
			}
		} else {
			tm = 0;
		}

		pk_ralm->exinf   = almcb->exinf;
		pk_ralm->lfttim  = tm;
		pk_ralm->almstat = almcb->almstat;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REF_ALM */

#if USE_DBGSPT

#if USE_OBJECT_NAME
/*
 * Get object name from control block
 */
EXPORT ER knl_alarmhandler_getname(ID id, UB **name)
{
	ALMCB	*almcb;
	ER	ercd = E_OK;

	CHECK_ALMID(id);

	BEGIN_DISABLE_INTERRUPT;
	almcb = get_almcb(id);
	if ( almcb->almhdr == NULL ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (almcb->almatr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = almcb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */

#ifdef USE_FUNC_TD_LST_ALM
/*
 * Refer alarm handler usage state
 */
SYSCALL INT td_lst_alm( ID list[], INT nent )
{
	ALMCB	*almcb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_almcb_table + NUM_ALMID;
	for ( almcb = knl_almcb_table; almcb < end; almcb++ ) {
		/* Unregistered handler */
		if ( almcb->almhdr == NULL ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = ID_ALM(almcb - knl_almcb_table);
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_ALM */

#ifdef USE_FUNC_TD_REF_ALM
/*
 * Refer alarm handler state
 */
SYSCALL ER td_ref_alm( ID almid, TD_RALM *pk_ralm )
{
	ALMCB	*almcb;
	ABSTIM	tm, cur;
	ER	ercd = E_OK;

	CHECK_ALMID(almid);

	almcb = get_almcb(almid);

	BEGIN_DISABLE_INTERRUPT;
	if ( almcb->almhdr == NULL ) { /* Unregistered handler */
		ercd = E_NOEXS;
	} else {
		cur = lltoul(knl_current_time);
		if ( (almcb->almstat & TALM_STA) != 0 ) {
			tm = almcb->almtmeb.time;
			if ( knl_abstim_reached(cur + TIMER_PERIOD, tm) ) {
				tm = 0;
			}
			else {
				tm -= (cur + TIMER_PERIOD);
			}
		} else {
			tm = 0;
		}

		pk_ralm->exinf   = almcb->exinf;
		pk_ralm->lfttim  = tm;
		pk_ralm->almstat = almcb->almstat;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_ALM */

#endif /* USE_DBGSPT */
#endif /* USE_ALARMHANDLER */
