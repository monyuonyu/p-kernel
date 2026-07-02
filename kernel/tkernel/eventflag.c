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
 * @file	eventflag.c
 * @brief	イベントフラグ機能の実装
 *
 * イベントフラグの生成・削除、セット（tk_set_flg）・クリア（tk_clr_flg）、
 * パターン待ち（tk_wai_flg）、状態参照、およびデバッガサポート機能を
 * 提供します。
 */

#include "kernel.h"
#include "wait.h"
#include "check.h"
#include "eventflag.h"

#if USE_EVENTFLAG == 1

Noinit(EXPORT FLGCB	knl_flgcb_table[NUM_FLGID]);	/* イベントフラグ制御ブロックテーブル */
Noinit(EXPORT QUEUE	knl_free_flgcb);	/* 未使用制御ブロックのキュー（FreeQue） */


/**
 * @brief イベントフラグ制御ブロックの初期化
 *
 * カーネル起動時に呼ばれ、全イベントフラグ制御ブロックを未使用状態にして
 * FreeQue に登録します。
 *
 * @return E_OK: 正常終了 / E_SYS: NUM_FLGID が 1 未満
 */
EXPORT ER knl_eventflag_initialize( void )
{
	FLGCB	*flgcb, *end;

	/* システム情報の確認 */
	if ( NUM_FLGID < 1 ) {
		return E_SYS;
	}

	/* すべての制御ブロックを FreeQue に登録 */
	QueInit(&knl_free_flgcb);
	end = knl_flgcb_table + NUM_FLGID;
	for ( flgcb = knl_flgcb_table; flgcb < end; flgcb++ ) {
		flgcb->flgid = 0;
		QueInsert(&flgcb->wait_queue, &knl_free_flgcb);
	}

	return E_OK;
}

/**
 * @brief イベントフラグの生成
 *
 * 生成情報 pk_cflg に従ってイベントフラグを生成し、
 * イベントフラグIDを割り当てます。フラグの初期値は iflgptn です。
 *
 * @param pk_cflg イベントフラグ生成情報へのポインタ
 * @return 正の値: 生成したイベントフラグID
 * @retval E_LIMIT 制御ブロックに空きがない（イベントフラグ数の上限超過）
 * @retval E_RSATR 不正な属性指定（CHK_RSATR 有効時）
 */
SYSCALL ID tk_cre_flg( CONST T_CFLG *pk_cflg )
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
	/* FreeQue から制御ブロックを取得 */
	flgcb = (FLGCB*)QueRemoveNext(&knl_free_flgcb);
	if ( flgcb == NULL ) {
		ercd = E_LIMIT;
	} else {
		flgid = ID_FLG(flgcb - knl_flgcb_table);

		/* 制御ブロックの初期化 */
		QueInit(&flgcb->wait_queue);
		flgcb->flgid = flgid;
		flgcb->exinf = pk_cflg->exinf;
		flgcb->flgatr = pk_cflg->flgatr;
		flgcb->flgptn = pk_cflg->iflgptn;
#if USE_OBJECT_NAME
		if ( (pk_cflg->flgatr & TA_DSNAME) != 0 ) {
			knl_strncpy((char*)flgcb->name, (char*)pk_cflg->dsname,
				OBJECT_NAME_LENGTH);
		}
#endif
		ercd = flgid;
	}
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_DEL_FLG
/**
 * @brief イベントフラグの削除
 *
 * 指定したイベントフラグを削除します。待ち状態のタスクがあれば
 * E_DLT を返して待ち解除し、制御ブロックを FreeQue に返却します。
 *
 * @param flgid 削除するイベントフラグID
 * @retval E_OK    正常終了
 * @retval E_NOEXS 対象のイベントフラグが存在しない
 * @retval E_ID    flgid が不正
 */
SYSCALL ER tk_del_flg( ID flgid )
{
	FLGCB	*flgcb;
	ER	ercd = E_OK;

	CHECK_FLGID(flgid);

	flgcb = get_flgcb(flgid);

	BEGIN_CRITICAL_SECTION;
	if ( flgcb->flgid == 0 ) {
		ercd = E_NOEXS;
	} else {
		/* 待ちタスクの待ち解除（E_DLT を返す） */
		knl_wait_delete(&flgcb->wait_queue);

		/* FreeQue へ返却 */
		QueInsert(&flgcb->wait_queue, &knl_free_flgcb);
		flgcb->flgid = 0;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_FLG */

/**
 * @brief イベントフラグのセット
 *
 * 指定したビットパターン setptn を OR でフラグにセットし、待ちキュー内の
 * タスクを先頭から順に調べて、待ち解除条件を満たすタスクの待ちを解除
 * します。待ち解除したタスクに TWF_BITCLR / TWF_CLR が指定されていれば、
 * 対応するビットのクリアも行います。
 *
 * @param flgid  対象のイベントフラグID
 * @param setptn セットするビットパターン
 * @retval E_OK    正常終了
 * @retval E_NOEXS 対象のイベントフラグが存在しない
 * @retval E_ID    flgid が不正
 * @note タスク独立部（割込みハンドラ等）からも呼び出せます。
 */
SYSCALL ER tk_set_flg( ID flgid, UINT setptn )
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

	/* イベントフラグのセット */
	flgcb->flgptn |= setptn;

	/* 待ち解除すべきタスクの探索 */
	queue = flgcb->wait_queue.next;
	while ( queue != &flgcb->wait_queue ) {
		tcb = (TCB*)queue;
		queue = queue->next;

		/* 待ち解除条件を満たすか？ */
		waiptn = tcb->winfo.flg.waiptn;
		wfmode = tcb->winfo.flg.wfmode;
		if ( knl_eventflag_cond(flgcb, waiptn, wfmode) ) {

			/* 待ち解除 */
			*tcb->winfo.flg.p_flgptn = flgcb->flgptn;
			knl_wait_release_ok(tcb);

			/* イベントフラグのクリア */
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

/**
 * @brief イベントフラグのクリア
 *
 * フラグの現在パターンと clrptn の AND を取り、clrptn 中で 0 の
 * ビットをクリアします。この操作で待ち解除が起きることはありません。
 *
 * @param flgid  対象のイベントフラグID
 * @param clrptn クリアするビットを 0 にしたパターン
 * @retval E_OK    正常終了
 * @retval E_NOEXS 対象のイベントフラグが存在しない
 * @retval E_ID    flgid が不正
 */
SYSCALL ER tk_clr_flg( ID flgid, UINT clrptn )
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

/**
 * @brief 待ちタスクの優先度変更時の処理
 *
 * イベントフラグ待ちキューを優先度順に並べ替えます。
 *
 * @param tcb    優先度が変更されたタスクの TCB
 * @param oldpri 変更前の優先度（未使用）
 */
LOCAL void flg_chg_pri( TCB *tcb, INT oldpri )
{
	FLGCB	*flgcb;

	flgcb = get_flgcb(tcb->wid);
	knl_gcb_change_priority((GCB*)flgcb, tcb);
}

/*
 * イベントフラグ待ち仕様の定義
 */
LOCAL CONST WSPEC knl_wspec_flg_tfifo = { TTW_FLG, NULL, NULL };
LOCAL CONST WSPEC knl_wspec_flg_tpri  = { TTW_FLG, flg_chg_pri, NULL };

/**
 * @brief イベントフラグ待ち
 *
 * waiptn と wfmode（TWF_ANDW / TWF_ORW、および TWF_CLR / TWF_BITCLR）で
 * 指定した条件が成立するまで待ちます。条件成立時のフラグパターンを
 * p_flgptn に返します。TA_WMUL 属性でない場合、既に待ちタスクが
 * 存在すると E_OBJ になります。
 *
 * @param flgid    対象のイベントフラグID
 * @param waiptn   待ちビットパターン（0 は不可）
 * @param wfmode   待ちモード
 * @param p_flgptn 待ち解除時のフラグパターンを格納する領域
 * @param tmout    タイムアウト時間（TMO_POL / TMO_FEVR 指定可）
 * @retval E_OK    正常終了（条件成立）
 * @retval E_NOEXS 対象のイベントフラグが存在しない
 * @retval E_OBJ   多重待ち禁止（TA_WMUL でない）のフラグに複数タスクが待ち
 * @retval E_TMOUT タイムアウトまたはポーリング失敗
 * @retval E_DLT   待ち中に対象イベントフラグが削除された
 * @retval E_RLWAI 待ち中に tk_rel_wai により強制解除された
 * @retval E_ID    flgid が不正
 * @retval E_PAR   waiptn・wfmode・tmout が不正
 * @retval E_CTX   ディスパッチ禁止状態またはタスク独立部からの呼出し
 * @note 待ち状態になり得るため、タスク部からのみ呼び出せます。
 */
SYSCALL ER tk_wai_flg( ID flgid, UINT waiptn, UINT wfmode, UINT *p_flgptn, TMO tmout )
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
		/* 複数タスクの待ちは禁止 */
		ercd = E_OBJ;
		goto error_exit;
	}

	/* 待ち解除条件を満たすか？ */
	if ( knl_eventflag_cond(flgcb, waiptn, wfmode) ) {
		*p_flgptn = flgcb->flgptn;

		/* イベントフラグのクリア */
		if ( (wfmode & TWF_BITCLR) != 0 ) {
			flgcb->flgptn &= ~waiptn;
		}
		if ( (wfmode & TWF_CLR) != 0 ) {
			flgcb->flgptn = 0;
		}
	} else {
		/* 待ち状態へ移行する準備 */
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

#ifdef USE_FUNC_TK_REF_FLG
/**
 * @brief イベントフラグ状態の参照
 *
 * イベントフラグの拡張情報・待ちタスクの有無・現在のフラグパターンを
 * 取得します。
 *
 * @param flgid   対象のイベントフラグID
 * @param pk_rflg 状態を格納する領域へのポインタ
 * @retval E_OK    正常終了
 * @retval E_NOEXS 対象のイベントフラグが存在しない
 * @retval E_ID    flgid が不正
 */
SYSCALL ER tk_ref_flg( ID flgid, T_RFLG *pk_rflg )
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
 *	デバッガサポート機能
 */
#if USE_DBGSPT

#if USE_OBJECT_NAME
/**
 * @brief 制御ブロックからのオブジェクト名取得
 *
 * TA_DSNAME 属性で生成されたイベントフラグの名前へのポインタを返します。
 *
 * @param id   対象のイベントフラグID
 * @param name 名前へのポインタを格納する領域
 * @retval E_OK    正常終了
 * @retval E_NOEXS 対象のイベントフラグが存在しない
 * @retval E_OBJ   TA_DSNAME 属性が指定されていない
 * @retval E_ID    id が不正
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

#ifdef USE_FUNC_TD_LST_FLG
/**
 * @brief イベントフラグID一覧の参照
 *
 * 使用中のイベントフラグIDを list に最大 nent 個格納します。
 *
 * @param list ID を格納する配列
 * @param nent 配列の要素数
 * @return 使用中のイベントフラグ総数（nent を超える場合もそのまま返す）
 */
SYSCALL INT td_lst_flg( ID list[], INT nent )
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
 * @brief イベントフラグ状態の参照（デバッガサポート）
 *
 * イベントフラグの拡張情報・待ちタスクの有無・現在のフラグパターンを
 * 取得します。
 *
 * @param flgid   対象のイベントフラグID
 * @param pk_rflg 状態を格納する領域へのポインタ
 * @retval E_OK    正常終了
 * @retval E_NOEXS 対象のイベントフラグが存在しない
 * @retval E_ID    flgid が不正
 */
SYSCALL ER td_ref_flg( ID flgid, TD_RFLG *pk_rflg )
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
 * @brief イベントフラグ待ちキューの参照
 *
 * イベントフラグ待ちキューに並ぶタスクのIDを先頭から順に list に
 * 最大 nent 個格納します。
 *
 * @param flgid 対象のイベントフラグID
 * @param list  タスクID を格納する配列
 * @param nent  配列の要素数
 * @return 正の値または 0: 待ちタスクの総数
 * @retval E_NOEXS 対象のイベントフラグが存在しない
 * @retval E_ID    flgid が不正
 */
SYSCALL INT td_flg_que( ID flgid, ID list[], INT nent )
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
#endif /* USE_EVENTFLAG */
