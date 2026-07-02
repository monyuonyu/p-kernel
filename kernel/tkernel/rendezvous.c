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
 * @file	rendezvous.c
 * @brief	ランデブ機能の実装
 *
 * タスク間の同期的なメッセージ交換を行うランデブポートの
 * 生成・削除・呼出・受付・回送・返答・状態参照 API（tk_cre_por 等）と、
 * デバッガサポート機能（td_lst_por 等）を提供します。
 * レガシー API（USE_LEGACY_API）構成でのみ組み込まれます。
 */


#include "kernel.h"
#include "wait.h"
#include "check.h"
#include "rendezvous.h"

#if USE_LEGACY_API && USE_RENDEZVOUS


Noinit(EXPORT PORCB knl_porcb_table[NUM_PORID]);	/* ランデブポート管理ブロックテーブル */
Noinit(EXPORT QUEUE knl_free_porcb);	/* 未使用管理ブロックのキュー（FreeQue） */


/**
 * @brief ランデブポート管理ブロックの初期化
 *
 * すべての管理ブロックを未使用状態にして FreeQue に登録します。
 * カーネル起動時に一度だけ呼び出されます。
 *
 * @retval E_OK	正常終了
 * @retval E_SYS	ランデブポート数（NUM_PORID）が 1 未満
 */
EXPORT ER knl_rendezvous_initialize( void )
{
	PORCB	*porcb, *end;

	/* システム情報の確認 */
	if ( NUM_PORID < 1 ) {
		return E_SYS;
	}

	/* 全管理ブロックを FreeQue に登録 */
	QueInit(&knl_free_porcb);
	end = knl_porcb_table + NUM_PORID;
	for ( porcb = knl_porcb_table; porcb < end; porcb++ ) {
		porcb->porid = 0;
		QueInsert(&porcb->call_queue, &knl_free_porcb);
	}

	return E_OK;
}


/**
 * @brief ランデブ呼出待ちタスクの優先度変更時の処理
 *
 * 呼出待ちキューをタスクの新しい優先度に従って並べ替えます。
 *
 * @param tcb	優先度が変更されたタスクの TCB
 * @param oldpri	変更前の優先度（未使用）
 */
LOCAL void cal_chg_pri( TCB *tcb, INT oldpri )
{
	PORCB	*porcb;

	porcb = get_porcb(tcb->wid);
	knl_gcb_change_priority((GCB*)porcb, tcb);
}

/*
 * ランデブ待ちの待ち仕様定義
 */
EXPORT CONST WSPEC knl_wspec_cal_tfifo = { TTW_CAL, NULL, NULL };
EXPORT CONST WSPEC knl_wspec_cal_tpri  = { TTW_CAL, cal_chg_pri, NULL };

EXPORT CONST WSPEC knl_wspec_rdv       = { TTW_RDV, NULL, NULL };


/**
 * @brief ランデブポートの生成
 *
 * FreeQue から管理ブロックを取得して初期化し、ランデブポートを
 * 生成します。
 *
 * @param pk_cpor	ランデブポート生成情報へのポインタ
 *
 * @return 生成したランデブポートの ID（正値）、またはエラーコード
 * @retval E_RSATR	不正な属性（poratr）
 * @retval E_PAR	パラメータ不正（maxcmsz < 0 または maxrmsz < 0）
 * @retval E_LIMIT	ランデブポート数が上限（NUM_PORID）を超過
 *
 * @note タスク独立部からは呼び出せません。
 */
SYSCALL ID tk_cre_por( CONST T_CPOR *pk_cpor )
{
#if CHK_RSATR
	const ATR VALID_PORATR = {
		 TA_TPRI
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
	};
#endif
	PORCB	*porcb;
	ID	porid;
	ER	ercd;

	CHECK_RSATR(pk_cpor->poratr, VALID_PORATR);
	CHECK_PAR(pk_cpor->maxcmsz >= 0);
	CHECK_PAR(pk_cpor->maxrmsz >= 0);
	CHECK_INTSK();

	BEGIN_CRITICAL_SECTION;
	/* FreeQue から管理ブロックを取得 */
	porcb = (PORCB*)QueRemoveNext(&knl_free_porcb);
	if ( porcb == NULL ) {
		ercd = E_LIMIT;
	} else {
		porid = ID_POR(porcb - knl_porcb_table);

		/* 管理ブロックの初期化 */
		QueInit(&porcb->call_queue);
		porcb->porid = porid;
		porcb->exinf = pk_cpor->exinf;
		porcb->poratr = pk_cpor->poratr;
		QueInit(&porcb->accept_queue);
		porcb->maxcmsz = pk_cpor->maxcmsz;
		porcb->maxrmsz = pk_cpor->maxrmsz;
#if USE_OBJECT_NAME
		if ( (pk_cpor->poratr & TA_DSNAME) != 0 ) {
			knl_strncpy((char*)porcb->name, (char*)pk_cpor->dsname,
				OBJECT_NAME_LENGTH);
		}
#endif
		ercd = porid;
	}
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_DEL_POR
/**
 * @brief ランデブポートの削除
 *
 * 呼出・受付の各待ちキューにつながれたタスクを E_DLT で待ち解除し、
 * 管理ブロックを FreeQue へ返却します。ランデブ成立後の終了待ち
 * （TTW_RDV）のタスクはポートにつながれていないため解除されません。
 *
 * @param porid	削除するランデブポートの ID
 *
 * @retval E_OK	正常終了
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象ランデブポートが未生成
 *
 * @note タスク独立部からは呼び出せません。
 */
SYSCALL ER tk_del_por( ID porid )
{
	PORCB	*porcb;
	ER	ercd = E_OK;

	CHECK_PORID(porid);
	CHECK_INTSK();

	porcb = get_porcb(porid);

	BEGIN_CRITICAL_SECTION;
	if ( porcb->porid == 0 ) {
		ercd = E_NOEXS;
	} else {
		/* 待ちタスクの待ち解除（E_DLT） */
		knl_wait_delete(&porcb->call_queue);
		knl_wait_delete(&porcb->accept_queue);

		/* FreeQue へ返却 */
		QueInsert(&porcb->call_queue, &knl_free_porcb);
		porcb->porid = 0;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_POR */

/**
 * @brief ランデブの呼出
 *
 * 受付待ちキューから calptn とビットパターンが一致する受付タスクを
 * 探します。見つかった場合は呼出メッセージを渡して受付タスクを待ち解除
 * し、自タスクはランデブ終了待ち（TTW_RDV、無期限待ち）になります。
 * 見つからない場合は tmout に従いランデブ呼出待ち（TTW_CAL）に
 * なります。
 *
 * @param porid	呼び出すランデブポートの ID
 * @param calptn	呼出条件のビットパターン（0 以外）
 * @param msg	呼出メッセージ領域（返答メッセージの受け取りにも使用）
 * @param cmsgsz	呼出メッセージのサイズ（バイト数、0 以上）
 * @param tmout	呼出待ちのタイムアウト時間（TMO_POL / TMO_FEVR 指定可）
 *
 * @return 返答メッセージのサイズ（正値または 0）、またはエラーコード
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象ランデブポートが未生成
 * @retval E_PAR	パラメータ不正（calptn == 0、cmsgsz < 0、
 *			cmsgsz > maxcmsz）
 * @retval E_TMOUT	呼出待ちのタイムアウト
 * @retval E_RLWAI	待ち状態の強制解除
 * @retval E_DLT	呼出待ちの間に対象ポートが削除された
 *
 * @note ランデブ成立後の終了待ちにはタイムアウトはなく、tk_rpl_rdv 等に
 *	よる返答まで待ち続けます。タスク独立部およびディスパッチ禁止中は
 *	呼び出せません。
 */
SYSCALL INT tk_cal_por( ID porid, UINT calptn, void *msg, INT cmsgsz, TMO tmout )
{
	PORCB	*porcb;
	TCB	*tcb;
	QUEUE	*queue;
	RNO	rdvno;
	INT	rmsgsz;
	ER	ercd = E_OK;

	CHECK_PORID(porid);
	CHECK_PAR(calptn != 0);
	CHECK_PAR(cmsgsz >= 0);
	CHECK_TMOUT(tmout);
	CHECK_DISPATCH();

	porcb = get_porcb(porid);

	BEGIN_CRITICAL_SECTION;
	if ( porcb->porid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
#if CHK_PAR
	if ( cmsgsz > porcb->maxcmsz ) {
		ercd = E_PAR;
		goto error_exit;
	}
#endif

	/* 受付待ちタスクの探索 */
	queue = porcb->accept_queue.next;
	while ( queue != &porcb->accept_queue ) {
		tcb = (TCB*)queue;
		queue = queue->next;
		if ( (calptn & tcb->winfo.acp.acpptn) == 0 ) {
			continue;
		}

		/* メッセージの送信 */
		rdvno = knl_gen_rdvno(knl_ctxtsk);
		if ( cmsgsz > 0 ) {
			knl_memcpy(tcb->winfo.acp.msg, msg, (UINT)cmsgsz);
		}
		*tcb->winfo.acp.p_rdvno = rdvno;
		*tcb->winfo.acp.p_cmsgsz = cmsgsz;
		knl_wait_release_ok(tcb);

		/* ランデブ終了待ち状態への移行準備 */
		ercd = E_TMOUT;
		knl_ctxtsk->wspec = &knl_wspec_rdv;
		knl_ctxtsk->wid = 0;
		knl_ctxtsk->wercd = &ercd;
		knl_ctxtsk->winfo.rdv.rdvno = rdvno;
		knl_ctxtsk->winfo.rdv.msg = msg;
		knl_ctxtsk->winfo.rdv.maxrmsz = porcb->maxrmsz;
		knl_ctxtsk->winfo.rdv.p_rmsgsz = &rmsgsz;
		knl_make_wait(TMO_FEVR, porcb->poratr);
		QueInit(&knl_ctxtsk->tskque);

		goto error_exit;
	}

	/* ランデブ呼出待ち状態への移行準備 */
	knl_ctxtsk->wspec = ( (porcb->poratr & TA_TPRI) != 0 )?
					&knl_wspec_cal_tpri: &knl_wspec_cal_tfifo;
	knl_ctxtsk->wercd = &ercd;
	knl_ctxtsk->winfo.cal.calptn = calptn;
	knl_ctxtsk->winfo.cal.msg = msg;
	knl_ctxtsk->winfo.cal.cmsgsz = cmsgsz;
	knl_ctxtsk->winfo.cal.p_rmsgsz = &rmsgsz;
	knl_gcb_make_wait((GCB*)porcb, tmout);

    error_exit:
	END_CRITICAL_SECTION;

	return ( ercd < E_OK )? ercd: rmsgsz;
}


LOCAL CONST WSPEC knl_wspec_acp       = { TTW_ACP, NULL, NULL };

/**
 * @brief ランデブの受付
 *
 * 呼出待ちキューから acpptn とビットパターンが一致する呼出タスクを
 * 探します。見つかった場合は呼出メッセージを受け取り、呼出タスクを
 * ランデブ終了待ち（TTW_RDV、無期限待ち）に移行させます。
 * 見つからない場合は tmout に従いランデブ受付待ち（TTW_ACP）に
 * なります。
 *
 * @param porid	受け付けるランデブポートの ID
 * @param acpptn	受付条件のビットパターン（0 以外）
 * @param p_rdvno	成立したランデブのランデブ番号の格納先アドレス
 * @param msg	呼出メッセージの格納先アドレス
 * @param tmout	受付待ちのタイムアウト時間（TMO_POL / TMO_FEVR 指定可）
 *
 * @return 呼出メッセージのサイズ（正値または 0）、またはエラーコード
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象ランデブポートが未生成
 * @retval E_PAR	パラメータ不正（acpptn == 0）
 * @retval E_TMOUT	タイムアウト（TMO_POL 指定時はポーリング失敗）
 * @retval E_RLWAI	待ち状態の強制解除
 * @retval E_DLT	受付待ちの間に対象ポートが削除された
 *
 * @note タスク独立部およびディスパッチ禁止中は呼び出せません。
 */
SYSCALL INT tk_acp_por( ID porid, UINT acpptn, RNO *p_rdvno, void *msg, TMO tmout )
{
	PORCB	*porcb;
	TCB	*tcb;
	QUEUE	*queue;
	RNO	rdvno;
	INT	cmsgsz;
	ER	ercd = E_OK;

	CHECK_PORID(porid);
	CHECK_PAR(acpptn != 0);
	CHECK_TMOUT(tmout);
	CHECK_DISPATCH();

	porcb = get_porcb(porid);

	BEGIN_CRITICAL_SECTION;
	if ( porcb->porid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}

	/* 呼出待ちタスクの探索 */
	queue = porcb->call_queue.next;
	while ( queue != &porcb->call_queue ) {
		tcb = (TCB*)queue;
		queue = queue->next;
		if ( (acpptn & tcb->winfo.cal.calptn) == 0 ) {
			continue;
		}

		/* メッセージの受信 */
		*p_rdvno = rdvno = knl_gen_rdvno(tcb);
		cmsgsz = tcb->winfo.cal.cmsgsz;
		if ( cmsgsz > 0 ) {
			knl_memcpy(msg, tcb->winfo.cal.msg, (UINT)cmsgsz);
		}

		knl_wait_cancel(tcb);

		/* 相手タスクをランデブ終了待ち状態に移行 */
		tcb->wspec = &knl_wspec_rdv;
		tcb->wid = 0;
		tcb->winfo.rdv.rdvno = rdvno;
		tcb->winfo.rdv.msg = tcb->winfo.cal.msg;
		tcb->winfo.rdv.maxrmsz = porcb->maxrmsz;
		tcb->winfo.rdv.p_rmsgsz = tcb->winfo.cal.p_rmsgsz;
		knl_timer_insert(&tcb->wtmeb, TMO_FEVR,
					(CBACK)knl_wait_release_tmout, tcb);
		QueInit(&tcb->tskque);

		goto error_exit;
	}

	ercd = E_TMOUT;
	if ( tmout != TMO_POL ) {
		/* ランデブ受付待ち状態への移行準備 */
		knl_ctxtsk->wspec = &knl_wspec_acp;
		knl_ctxtsk->wid = porid;
		knl_ctxtsk->wercd = &ercd;
		knl_ctxtsk->winfo.acp.acpptn = acpptn;
		knl_ctxtsk->winfo.acp.msg = msg;
		knl_ctxtsk->winfo.acp.p_rdvno = p_rdvno;
		knl_ctxtsk->winfo.acp.p_cmsgsz = &cmsgsz;
		knl_make_wait(tmout, porcb->poratr);
		QueInsert(&knl_ctxtsk->tskque, &porcb->accept_queue);
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ( ercd < E_OK )? ercd: cmsgsz;
}

#ifdef USE_FUNC_TK_FWD_POR
/**
 * @brief ランデブの他ポートへの回送
 *
 * 成立済みのランデブ（rdvno）を別のランデブポート porid へ回送します。
 * 回送先に受付待ちタスクがあればメッセージを渡してランデブを成立させ、
 * 呼出タスクは新しいランデブ番号での終了待ちに更新します。受付待ち
 * タスクがなければ、呼出タスクを回送先ポートの呼出待ち状態に戻します。
 *
 * @param porid	回送先ランデブポートの ID
 * @param calptn	回送後の呼出条件のビットパターン（0 以外）
 * @param rdvno	回送するランデブのランデブ番号
 * @param msg	回送する呼出メッセージの先頭アドレス
 * @param cmsgsz	回送する呼出メッセージのサイズ（バイト数、0 以上）
 *
 * @retval E_OK	正常終了
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	回送先ランデブポートが未生成
 * @retval E_PAR	パラメータ不正（calptn == 0、cmsgsz < 0、
 *			cmsgsz が回送先の maxcmsz や呼出タスクの
 *			maxrmsz を超過）
 * @retval E_OBJ	rdvno に対応するランデブ終了待ちタスクが存在
 *			しない、または回送先の maxrmsz が呼出タスクの
 *			maxrmsz を超えている
 *
 * @note タスク独立部からは呼び出せません。
 */
SYSCALL ER tk_fwd_por( ID porid, UINT calptn, RNO rdvno, CONST void *msg, INT cmsgsz )
{
	PORCB	*porcb;
	TCB	*caltcb, *tcb;
	QUEUE	*queue;
	RNO	new_rdvno;
	ER	ercd = E_OK;

	CHECK_PORID(porid);
	CHECK_PAR(calptn != 0);
	CHECK_RDVNO(rdvno);
	CHECK_PAR(cmsgsz >= 0);
	CHECK_INTSK();

	porcb = get_porcb(porid);
	caltcb = get_tcb(knl_get_tskid_rdvno(rdvno));

	BEGIN_CRITICAL_SECTION;
	if ( porcb->porid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
#if CHK_PAR
	if ( cmsgsz > porcb->maxcmsz ) {
		ercd = E_PAR;
		goto error_exit;
	}
#endif
	if ( (caltcb->state & TS_WAIT) == 0
	  || caltcb->wspec != &knl_wspec_rdv
	  || rdvno != caltcb->winfo.rdv.rdvno ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	if ( porcb->maxrmsz > caltcb->winfo.rdv.maxrmsz ) {
		ercd = E_OBJ;
		goto error_exit;
	}
#if CHK_PAR
	if ( cmsgsz > caltcb->winfo.rdv.maxrmsz ) {
		ercd = E_PAR;
		goto error_exit;
	}
#endif

	/* 受付待ちタスクの探索 */
	queue = porcb->accept_queue.next;
	while ( queue != &porcb->accept_queue ) {
		tcb = (TCB*)queue;
		queue = queue->next;
		if ( (calptn & tcb->winfo.acp.acpptn) == 0 ) {
			continue;
		}

		/* メッセージの送信 */
		new_rdvno = knl_gen_rdvno(caltcb);
		if ( cmsgsz > 0 ) {
			knl_memcpy(tcb->winfo.acp.msg, msg, (UINT)cmsgsz);
		}
		*tcb->winfo.acp.p_rdvno = new_rdvno;
		*tcb->winfo.acp.p_cmsgsz = cmsgsz;
		knl_wait_release_ok(tcb);

		/* 相手タスクのランデブ終了待ち情報を更新 */
		caltcb->winfo.rdv.rdvno = new_rdvno;
		caltcb->winfo.rdv.msg = caltcb->winfo.cal.msg;
		caltcb->winfo.rdv.maxrmsz = porcb->maxrmsz;
		caltcb->winfo.rdv.p_rmsgsz = caltcb->winfo.cal.p_rmsgsz;

		goto error_exit;
	}

	/* 相手タスクをランデブ呼出待ち状態に移行 */
	caltcb->wspec = ( (porcb->poratr & TA_TPRI) != 0 )?
				&knl_wspec_cal_tpri: &knl_wspec_cal_tfifo;
	caltcb->wid = porid;
	caltcb->winfo.cal.calptn = calptn;
	caltcb->winfo.cal.msg = caltcb->winfo.rdv.msg;
	caltcb->winfo.cal.cmsgsz = cmsgsz;
	caltcb->winfo.cal.p_rmsgsz = caltcb->winfo.rdv.p_rmsgsz;
	knl_timer_insert(&caltcb->wtmeb, TMO_FEVR,
			(CBACK)knl_wait_release_tmout, caltcb);
	if ( (porcb->poratr & TA_TPRI) != 0 ) {
		knl_queue_insert_tpri(caltcb, &porcb->call_queue);
	} else {
		QueInsert(&caltcb->tskque, &porcb->call_queue);
	}

	if ( cmsgsz > 0 ) {
		knl_memcpy(caltcb->winfo.cal.msg, msg, (UINT)cmsgsz);
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_FWD_POR */

/**
 * @brief ランデブへの返答
 *
 * ランデブ終了待ち中の呼出タスクへ返答メッセージを渡し、
 * 待ち状態を解除してランデブを終了させます。
 *
 * @param rdvno	返答するランデブのランデブ番号
 * @param msg	返答メッセージの先頭アドレス
 * @param rmsgsz	返答メッセージのサイズ（バイト数、0 以上）
 *
 * @retval E_OK	正常終了
 * @retval E_PAR	パラメータ不正（rmsgsz < 0 または
 *			rmsgsz > 呼出タスクの maxrmsz）
 * @retval E_OBJ	rdvno に対応するランデブ終了待ちタスクが存在しない
 *
 * @note タスク独立部からは呼び出せません。
 */
SYSCALL ER tk_rpl_rdv( RNO rdvno, CONST void *msg, INT rmsgsz )
{
	TCB	*caltcb;
	ER	ercd = E_OK;

	CHECK_RDVNO(rdvno);
	CHECK_PAR(rmsgsz >= 0);
	CHECK_INTSK();

	caltcb = get_tcb(knl_get_tskid_rdvno(rdvno));

	BEGIN_CRITICAL_SECTION;
	if ( (caltcb->state & TS_WAIT) == 0
	  || caltcb->wspec != &knl_wspec_rdv
	  || rdvno != caltcb->winfo.rdv.rdvno ) {
		ercd = E_OBJ;
		goto error_exit;
	}
#if CHK_PAR
	if ( rmsgsz > caltcb->winfo.rdv.maxrmsz ) {
		ercd = E_PAR;
		goto error_exit;
	}
#endif

	/* 返答メッセージの送信と待ち解除 */
	if ( rmsgsz > 0 ) {
		knl_memcpy(caltcb->winfo.rdv.msg, msg, (UINT)rmsgsz);
	}
	*caltcb->winfo.rdv.p_rmsgsz = rmsgsz;
	knl_wait_release_ok(caltcb);

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_REF_POR
/**
 * @brief ランデブポートの状態参照
 *
 * 拡張情報、呼出・受付待ちタスクの ID、呼出・返答メッセージの
 * 最大サイズを pk_rpor へ返します。
 *
 * @param porid	参照するランデブポートの ID
 * @param pk_rpor	状態情報の格納先アドレス
 *
 * @retval E_OK	正常終了
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象ランデブポートが未生成
 */
SYSCALL ER tk_ref_por( ID porid, T_RPOR *pk_rpor )
{
	PORCB	*porcb;
	ER	ercd = E_OK;

	CHECK_PORID(porid);

	porcb = get_porcb(porid);

	BEGIN_CRITICAL_SECTION;
	if ( porcb->porid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rpor->exinf = porcb->exinf;
		pk_rpor->wtsk = knl_wait_tskid(&porcb->call_queue);
		pk_rpor->atsk = knl_wait_tskid(&porcb->accept_queue);
		pk_rpor->maxcmsz = porcb->maxcmsz;
		pk_rpor->maxrmsz = porcb->maxrmsz;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REF_POR */

/* ------------------------------------------------------------------------ */
/*
 *	デバッガサポート機能
 */
#if USE_DBGSPT

#if USE_OBJECT_NAME
/**
 * @brief 管理ブロックからのオブジェクト名取得
 *
 * TA_DSNAME 属性付きで生成されたランデブポートの名前への
 * ポインタを返します。
 *
 * @param id	対象ランデブポートの ID
 * @param name	名前へのポインタの格納先アドレス
 *
 * @retval E_OK	正常終了
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象ランデブポートが未生成
 * @retval E_OBJ	TA_DSNAME 属性が指定されていない
 */
EXPORT ER knl_rendezvous_getname(ID id, UB **name)
{
	PORCB	*porcb;
	ER	ercd = E_OK;

	CHECK_PORID(id);

	BEGIN_DISABLE_INTERRUPT;
	porcb = get_porcb(id);
	if ( porcb->porid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (porcb->poratr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = porcb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */

#ifdef USE_FUNC_TD_LST_POR
/**
 * @brief ランデブポート ID の一覧取得
 *
 * 生成済みランデブポートの ID を list へ最大 nent 個格納します。
 *
 * @param list	ID リストの格納先配列
 * @param nent	list に格納可能な最大エントリ数
 *
 * @return 生成済みランデブポートの総数（nent を超える場合もその総数）
 */
SYSCALL INT td_lst_por( ID list[], INT nent )
{
	PORCB	*porcb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_porcb_table + NUM_PORID;
	for ( porcb = knl_porcb_table; porcb < end; porcb++ ) {
		if ( porcb->porid == 0 ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = porcb->porid;
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_POR */

#ifdef USE_FUNC_TD_REF_POR
/**
 * @brief ランデブポートの状態参照（デバッガサポート）
 *
 * tk_ref_por と同等の状態情報を TD_RPOR 形式で返します。
 *
 * @param porid	参照するランデブポートの ID
 * @param pk_rpor	状態情報の格納先アドレス
 *
 * @retval E_OK	正常終了
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象ランデブポートが未生成
 */
SYSCALL ER td_ref_por( ID porid, TD_RPOR *pk_rpor )
{
	PORCB	*porcb;
	ER	ercd = E_OK;

	CHECK_PORID(porid);

	porcb = get_porcb(porid);

	BEGIN_DISABLE_INTERRUPT;
	if ( porcb->porid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rpor->exinf = porcb->exinf;
		pk_rpor->wtsk = knl_wait_tskid(&porcb->call_queue);
		pk_rpor->atsk = knl_wait_tskid(&porcb->accept_queue);
		pk_rpor->maxcmsz = porcb->maxcmsz;
		pk_rpor->maxrmsz = porcb->maxrmsz;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_POR */

#ifdef USE_FUNC_TD_CAL_QUE
/**
 * @brief ランデブ呼出待ちキューの参照
 *
 * 呼出待ちキューにつながれたタスクの ID を待ち順に list へ最大
 * nent 個格納します。
 *
 * @param porid	参照するランデブポートの ID
 * @param list	タスク ID リストの格納先配列
 * @param nent	list に格納可能な最大エントリ数
 *
 * @return 呼出待ちタスクの総数（正値または 0）、またはエラーコード
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象ランデブポートが未生成
 */
SYSCALL INT td_cal_que( ID porid, ID list[], INT nent )
{
	PORCB	*porcb;
	QUEUE	*q;
	ER	ercd = E_OK;

	CHECK_PORID(porid);

	porcb = get_porcb(porid);

	BEGIN_DISABLE_INTERRUPT;
	if ( porcb->porid == 0 ) {
		ercd = E_NOEXS;
	} else {
		INT n = 0;
		for ( q = porcb->call_queue.next; q != &porcb->call_queue; q = q->next ) {
			if ( n++ < nent ) {
				*list++ = ((TCB*)q)->tskid;
			}
		}
		ercd = n;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_CAL_QUE */

#ifdef USE_FUNC_TD_ACP_QUE
/**
 * @brief ランデブ受付待ちキューの参照
 *
 * 受付待ちキューにつながれたタスクの ID を待ち順に list へ最大
 * nent 個格納します。
 *
 * @param porid	参照するランデブポートの ID
 * @param list	タスク ID リストの格納先配列
 * @param nent	list に格納可能な最大エントリ数
 *
 * @return 受付待ちタスクの総数（正値または 0）、またはエラーコード
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象ランデブポートが未生成
 */
SYSCALL INT td_acp_que( ID porid, ID list[], INT nent )
{
	PORCB	*porcb;
	QUEUE	*q;
	ER	ercd = E_OK;

	CHECK_PORID(porid);

	porcb = get_porcb(porid);

	BEGIN_DISABLE_INTERRUPT;
	if ( porcb->porid == 0 ) {
		ercd = E_NOEXS;
	} else {
		INT n = 0;
		for ( q = porcb->accept_queue.next; q != &porcb->accept_queue; q = q->next ) {
			if ( n++ < nent ) {
				*list++ = ((TCB*)q)->tskid;
			}
		}
		ercd = n;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_ACP_QUE */

#endif /* USE_DBGSPT */
#endif /* USE_LEGACY_API && USE_RENDEZVOUS */
