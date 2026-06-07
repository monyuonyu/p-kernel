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
 * @file eventflag.c
 * @brief イベントフラグ管理機能
 *
 * このファイルは、T-Kernelのイベントフラグ同期オブジェクトの
 * 実装を提供します。イベントフラグは、複数のタスク間でビット
 * パターンによる同期を取るためのオブジェクトです。
 *
 * 主な機能：
 * - イベントフラグの生成・削除
 * - イベントフラグパターンの設定・クリア
 * - イベントフラグ待ち（AND/OR条件）
 * - イベントフラグ状態参照
 * - 複数タスク待ちサポート（TA_WMUL属性）
 * - 優先度別待ちキューサポート（TA_TPRI属性）
 * - ビット自動クリア機能（TWF_BITCLR, TWF_CLR）
 */

/** [BEGIN Common Definitions] */
#include "kernel.h"
#include "task.h"
#include "wait.h"
#include "check.h"
#include "eventflag.h"
/** [END Common Definitions] */

#if CFN_MAX_FLGID > 0

#ifdef USE_FUNC_FLGCB_TABLE
Noinit(EXPORT FLGCB	knl_flgcb_table[NUM_FLGID]);	/* Event flag control block */
Noinit(EXPORT QUEUE	knl_free_flgcb);	/* FreeQue */
#endif /* USE_FUNC_FLGCB_TABLE */


#ifdef USE_FUNC_EVENTFLAG_INITIALIZE
/**
 * @brief イベントフラグ管理ブロックの初期化
 *
 * イベントフラグ管理ブロックの初期化を行います。
 * 全ての管理ブロックをフリーキューに登録します。
 *
 * @return エラーコード
 *   @retval E_OK 正常終了
 *   @retval E_SYS システムエラー（使用可能なイベントフラグ数が0）
 *
 * @note この関数はカーネル初期化時に呼び出されます。
 */
EXPORT ER knl_eventflag_initialize( void )
{
	FLGCB	*flgcb, *end;

	/* Get system information */
	if ( NUM_FLGID < 1 ) {
		return E_SYS;
	}

	/* Register all control blocks onto FreeQue */
	QueInit(&knl_free_flgcb);
	end = knl_flgcb_table + NUM_FLGID;
	for ( flgcb = knl_flgcb_table; flgcb < end; flgcb++ ) {
		flgcb->flgid = 0;
		QueInsert(&flgcb->wait_queue, &knl_free_flgcb);
	}

	return E_OK;
}
#endif /* USE_FUNC_EVENTFLAG_INITIALIZE */

#ifdef USE_FUNC_TK_CRE_FLG
/**
 * @brief イベントフラグ生成システムコール
 *
 * イベントフラグを生成し、イベントフラグIDを返します。
 * 初期パターンや属性を設定できます。
 *
 * @param pk_cflg イベントフラグ生成情報パケットへのポインタ
 *   - exinf: 拡張情報
 *   - flgatr: イベントフラグ属性（TA_TPRI, TA_WMUL, TA_DSNAME）
 *   - iflgptn: 初期パターン
 *   - dsname: オブジェクト名（TA_DSNAME指定時）
 *
 * @return イベントフラグID（正の値）またはエラーコード（負の値）
 *   @retval E_LIMIT イベントフラグ数が上限に達した
 *   @retval E_RSATR 不正な属性
 *
 * @note TA_TPRI: 優先度順待ちキュー, TA_WMUL: 複数タスク待ち許可
 */
SYSCALL ID tk_cre_flg_impl( CONST T_CFLG *pk_cflg )
{
#if CHK_RSATR
	const ATR VALID_FLGATR = {
		 TA_TPRI
		|TA_WMUL
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
	};
#endif
	FLGCB	*flgcb;
	ID	flgid;
	ER	ercd;

	CHECK_RSATR(pk_cflg->flgatr, VALID_FLGATR);

	BEGIN_CRITICAL_SECTION;
	/* Get control block from FreeQue */
	flgcb = (FLGCB*)QueRemoveNext(&knl_free_flgcb);
	if ( flgcb == NULL ) {
		ercd = E_LIMIT;
	} else {
		flgid = ID_FLG(flgcb - knl_flgcb_table);

		/* Initialize control block */
		QueInit(&flgcb->wait_queue);
		flgcb->flgid = flgid;
		flgcb->exinf = pk_cflg->exinf;
		flgcb->flgatr = pk_cflg->flgatr;
		flgcb->flgptn = pk_cflg->iflgptn;
#if USE_OBJECT_NAME
		if ( (pk_cflg->flgatr & TA_DSNAME) != 0 ) {
			strncpy((char*)flgcb->name, (char*)pk_cflg->dsname,
				OBJECT_NAME_LENGTH);
		}
#endif
		ercd = flgid;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_CRE_FLG */

#ifdef USE_FUNC_TK_DEL_FLG
/**
 * @brief イベントフラグ削除システムコール
 *
 * 指定されたイベントフラグを削除します。
 * 待ち状態のタスクがある場合は、E_DLTエラーで待ち解除します。
 *
 * @param flgid イベントフラグID
 *
 * @return エラーコード
 *   @retval E_OK 正常終了
 *   @retval E_ID 無効なイベントフラグID
 *   @retval E_NOEXS イベントフラグが存在しない
 *
 * @note 削除されたイベントフラグの管理ブロックはフリーキューに返却されます。
 */
SYSCALL ER tk_del_flg_impl( ID flgid )
{
	FLGCB	*flgcb;
	ER	ercd = E_OK;

	CHECK_FLGID(flgid);

	flgcb = get_flgcb(flgid);

	BEGIN_CRITICAL_SECTION;
	if ( flgcb->flgid == 0 ) {
		ercd = E_NOEXS;
	} else {
		/* Release wait state of task (E_DLT) */
		knl_wait_delete(&flgcb->wait_queue);

		/* Return to FreeQue */
		QueInsert(&flgcb->wait_queue, &knl_free_flgcb);
		flgcb->flgid = 0;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_FLG */

#ifdef USE_FUNC_TK_SET_FLG
/**
 * @brief イベントフラグ設定システムコール
 *
 * 指定されたイベントフラグにビットパターンを設定（OR演算）します。
 * 待ち条件が満たされたタスクがある場合、待ち解除します。
 *
 * @param flgid イベントフラグID
 * @param setptn 設定するビットパターン
 *
 * @return エラーコード
 *   @retval E_OK 正常終了
 *   @retval E_ID 無効なイベントフラグID
 *   @retval E_NOEXS イベントフラグが存在しない
 *
 * @note フラグ設定後、待ちタスクの条件をチェックし、
 *       条件満足時はTWF_BITCLR/TWF_CLRに従ってフラグをクリアします。
 */
SYSCALL ER tk_set_flg_impl( ID flgid, UINT setptn )
{
	FLGCB	*flgcb;
	TCB	*tcb;
	QUEUE	*queue;
	UINT	wfmode, waiptn;
	ER	ercd = E_OK;

	CHECK_FLGID(flgid);

	flgcb = get_flgcb(flgid);

	BEGIN_CRITICAL_SECTION;
	if ( flgcb->flgid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}

	/* Set event flag */
	flgcb->flgptn |= setptn;

	/* Search task which should be released */
	queue = flgcb->wait_queue.next;
	while ( queue != &flgcb->wait_queue ) {
		tcb = (TCB*)queue;
		queue = queue->next;

		/* Meet condition for release wait? */
		waiptn = tcb->winfo.flg.waiptn;
		wfmode = tcb->winfo.flg.wfmode;
		if ( knl_eventflag_cond(flgcb, waiptn, wfmode) ) {

			/* Release wait */
			*tcb->winfo.flg.p_flgptn = flgcb->flgptn;
			knl_wait_release_ok(tcb);

			/* Clear event flag */
			if ( (wfmode & TWF_BITCLR) != 0 ) {
				if ( (flgcb->flgptn &= ~waiptn) == 0 ) {
					break;
				}
			}
			if ( (wfmode & TWF_CLR) != 0 ) {
				flgcb->flgptn = 0;
				break;
			}
		}
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_SET_FLG */

#ifdef USE_FUNC_TK_CLR_FLG
/**
 * @brief イベントフラグクリアシステムコール
 *
 * 指定されたイベントフラグのビットパターンをクリア（AND演算）します。
 *
 * @param flgid イベントフラグID
 * @param clrptn クリアパターン（ビット、1:保持, 0:クリア）
 *
 * @return エラーコード
 *   @retval E_OK 正常終了
 *   @retval E_ID 無効なイベントフラグID
 *   @retval E_NOEXS イベントフラグが存在しない
 *
 * @note フラグパターン &= clrptn の演算でクリアします。
 *       0を指定すると全ビットがクリアされます。
 */
SYSCALL ER tk_clr_flg_impl( ID flgid, UINT clrptn )
{
	FLGCB	*flgcb;
	ER	ercd = E_OK;

	CHECK_FLGID(flgid);

	flgcb = get_flgcb(flgid);

	BEGIN_CRITICAL_SECTION;
	if ( flgcb->flgid == 0 ) {
		ercd = E_NOEXS;
	} else {
		flgcb->flgptn &= clrptn;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_CLR_FLG */

#ifdef USE_FUNC_TK_WAI_FLG
/**
 * @brief 待ちタスクの優先度変更時の処理
 *
 * イベントフラグ待ち中のタスクの優先度が変更された場合の
 * 待ちキューの再編成処理を行います。
 *
 * @param tcb 優先度が変更されたタスクのタスク管理ブロック
 * @param oldpri 変更前の優先度
 *
 * @note この関数はTA_TPRI属性のイベントフラグで使用されます。
 */
LOCAL void flg_chg_pri( TCB *tcb, INT oldpri )
{
	FLGCB	*flgcb;

	flgcb = get_flgcb(tcb->wid);
	knl_gcb_change_priority((GCB*)flgcb, tcb);
}

/*
 * Definition of event flag wait specification
 */
LOCAL CONST WSPEC knl_wspec_flg_tfifo = { TTW_FLG, NULL, NULL };
LOCAL CONST WSPEC knl_wspec_flg_tpri  = { TTW_FLG, flg_chg_pri, NULL };

/**
 * @brief イベントフラグ待ちシステムコール
 *
 * 指定されたイベントフラグのパターンが条件を満たすまで待ちます。
 * AND条件やOR条件、および待ち後のフラグクリア指定が可能です。
 *
 * @param flgid イベントフラグID
 * @param waiptn 待ちパターン（0以外）
 * @param wfmode 待ちモード（TWF_ORW|TWF_CLR|TWF_BITCLR）
 *   - TWF_ORW: OR待ち（デフォルトはAND待ち）
 *   - TWF_CLR: 待ち解除時に全ビットクリア
 *   - TWF_BITCLR: 待ち解除時に条件ビットのみクリア
 * @param p_flgptn 現在のフラグパターンの格納先
 * @param tmout タイムアウト時間
 *
 * @return エラーコード
 *   @retval E_OK 正常終了
 *   @retval E_ID 無効なイベントフラグID
 *   @retval E_NOEXS イベントフラグが存在しない
 *   @retval E_OBJ 複数タスク待ち禁止イベントフラグに既に待ちタスクあり
 *   @retval E_PAR パラメータエラー
 *   @retval E_TMOUT タイムアウト
 *   @retval E_CTX コンテキストエラー
 *
 * @note AND条件: (flgptn & waiptn) == waiptn
 *       OR条件: (flgptn & waiptn) != 0
 */
SYSCALL ER tk_wai_flg_impl( ID flgid, UINT waiptn, UINT wfmode, UINT *p_flgptn, TMO tmout )
{
	FLGCB	*flgcb;
	ER	ercd = E_OK;

	CHECK_FLGID(flgid);
	CHECK_PAR(waiptn != 0);
	CHECK_PAR((wfmode & ~(TWF_ORW|TWF_CLR|TWF_BITCLR)) == 0);
	CHECK_TMOUT(tmout);
	CHECK_DISPATCH();

	flgcb = get_flgcb(flgid);

	BEGIN_CRITICAL_SECTION;
	if ( flgcb->flgid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (flgcb->flgatr & TA_WMUL) == 0 && !isQueEmpty(&flgcb->wait_queue) ) {
		/* Disable multiple tasks wait */
		ercd = E_OBJ;
		goto error_exit;
	}

	/* Meet condition for release wait? */
	if ( knl_eventflag_cond(flgcb, waiptn, wfmode) ) {
		*p_flgptn = flgcb->flgptn;

		/* Clear event flag */
		if ( (wfmode & TWF_BITCLR) != 0 ) {
			flgcb->flgptn &= ~waiptn;
		}
		if ( (wfmode & TWF_CLR) != 0 ) {
			flgcb->flgptn = 0;
		}
	} else {
		/* Ready for wait */
		knl_ctxtsk->wspec = ( (flgcb->flgatr & TA_TPRI) != 0 )?
					&knl_wspec_flg_tpri: &knl_wspec_flg_tfifo;
		knl_ctxtsk->wercd = &ercd;
		knl_ctxtsk->winfo.flg.waiptn = waiptn;
		knl_ctxtsk->winfo.flg.wfmode = wfmode;
		knl_ctxtsk->winfo.flg.p_flgptn = p_flgptn;
		knl_gcb_make_wait((GCB*)flgcb, tmout);
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_WAI_FLG */

#ifdef USE_FUNC_TK_REF_FLG
/**
 * @brief イベントフラグ状態参照システムコール
 *
 * 指定されたイベントフラグの現在状態を参照します。
 *
 * @param flgid イベントフラグID
 * @param pk_rflg イベントフラグ状態パケットの格納先
 *   - exinf: 拡張情報
 *   - wtsk: 待ちタスクID（待ちタスクがない場合は0）
 *   - flgptn: 現在のフラグパターン
 *
 * @return エラーコード
 *   @retval E_OK 正常終了
 *   @retval E_ID 無効なイベントフラグID
 *   @retval E_NOEXS イベントフラグが存在しない
 *
 * @note 複数タスク待ちの場合、wtskは最初の待ちタスクIDを返します。
 */
SYSCALL ER tk_ref_flg_impl( ID flgid, T_RFLG *pk_rflg )
{
	FLGCB	*flgcb;
	ER	ercd = E_OK;

	CHECK_FLGID(flgid);

	flgcb = get_flgcb(flgid);

	BEGIN_CRITICAL_SECTION;
	if ( flgcb->flgid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rflg->exinf = flgcb->exinf;
		pk_rflg->wtsk = knl_wait_tskid(&flgcb->wait_queue);
		pk_rflg->flgptn = flgcb->flgptn;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REF_FLG */

/* ------------------------------------------------------------------------ */
/*
 *	Debugger support function
 */
#if USE_DBGSPT

#ifdef USE_FUNC_EVENTFLAG_GETNAME
#if USE_OBJECT_NAME
/**
 * @brief イベントフラグのオブジェクト名取得
 *
 * 指定されたイベントフラグのオブジェクト名を取得します。
 *
 * @param id イベントフラグID
 * @param name オブジェクト名のポインタの格納先
 *
 * @return エラーコード
 *   @retval E_OK 正常終了
 *   @retval E_ID 無効なイベントフラグID
 *   @retval E_NOEXS イベントフラグが存在しない
 *   @retval E_OBJ オブジェクト名が設定されていない
 *
 * @note この関数はデバッグサポート用です。
 */
EXPORT ER knl_eventflag_getname(ID id, UB **name)
{
	FLGCB	*flgcb;
	ER	ercd = E_OK;

	CHECK_FLGID(id);

	BEGIN_DISABLE_INTERRUPT;
	flgcb = get_flgcb(id);
	if ( flgcb->flgid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (flgcb->flgatr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = flgcb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */
#endif /* USE_FUNC_EVENTFLAG_GETNAME */

#ifdef USE_FUNC_TD_LST_FLG
/**
 * @brief イベントフラグ使用状態参照（デバッグ機能）
 *
 * 現在使用中のイベントフラグのID一覧を取得します。
 *
 * @param list イベントフラグID格納配列
 * @param nent 配列のエントリ数
 *
 * @return 使用中のイベントフラグ数
 *
 * @note この関数はデバッグサポート用です。
 *       戻り値がnentより大きい場合、全てのIDが格納されていないことを示します。
 */
SYSCALL INT td_lst_flg_impl( ID list[], INT nent )
{
	FLGCB	*flgcb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_flgcb_table + NUM_FLGID;
	for ( flgcb = knl_flgcb_table; flgcb < end; flgcb++ ) {
		if ( flgcb->flgid == 0 ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = flgcb->flgid;
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_FLG */

#ifdef USE_FUNC_TD_REF_FLG
/**
 * @brief イベントフラグ状態参照（デバッグ機能）
 *
 * 指定されたイベントフラグの現在状態をデバッグ用に参照します。
 *
 * @param flgid イベントフラグID
 * @param pk_rflg イベントフラグ状態パケットの格納先
 *
 * @return エラーコード
 *   @retval E_OK 正常終了
 *   @retval E_ID 無効なイベントフラグID
 *   @retval E_NOEXS イベントフラグが存在しない
 *
 * @note この関数はデバッグサポート用です。
 */
SYSCALL ER td_ref_flg_impl( ID flgid, TD_RFLG *pk_rflg )
{
	FLGCB	*flgcb;
	ER	ercd = E_OK;

	CHECK_FLGID(flgid);

	flgcb = get_flgcb(flgid);

	BEGIN_DISABLE_INTERRUPT;
	if ( flgcb->flgid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rflg->exinf = flgcb->exinf;
		pk_rflg->wtsk = knl_wait_tskid(&flgcb->wait_queue);
		pk_rflg->flgptn = flgcb->flgptn;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_FLG */

#ifdef USE_FUNC_TD_FLG_QUE
/**
 * @brief イベントフラグ待ちキュー参照（デバッグ機能）
 *
 * 指定されたイベントフラグの待ちキューにあるタスクIDの一覧を取得します。
 *
 * @param flgid イベントフラグID
 * @param list タスクID格納配列
 * @param nent 配列のエントリ数
 *
 * @return 待ちキューにあるタスク数（正の値）またはエラーコード（負の値）
 *   @retval E_ID 無効なイベントフラグID
 *   @retval E_NOEXS イベントフラグが存在しない
 *
 * @note この関数はデバッグサポート用です。
 *       戻り値がnentより大きい場合、全てのタスクIDが格納されていないことを示します。
 */
SYSCALL INT td_flg_que_impl( ID flgid, ID list[], INT nent )
{
	FLGCB	*flgcb;
	QUEUE	*q;
	ER	ercd = E_OK;

	CHECK_FLGID(flgid);

	flgcb = get_flgcb(flgid);

	BEGIN_DISABLE_INTERRUPT;
	if ( flgcb->flgid == 0 ) {
		ercd = E_NOEXS;
	} else {
		INT n = 0;
		for ( q = flgcb->wait_queue.next; q != &flgcb->wait_queue; q = q->next ) {
			if ( n++ < nent ) {
				*list++ = ((TCB*)q)->tskid;
			}
		}
		ercd = n;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_FLG_QUE */

#endif /* USE_DBGSPT */
#endif /* CFN_MAX_FLGID */
