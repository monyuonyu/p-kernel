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
 * @file	mailbox.c
 * @brief	メールボックス機能の実装
 *
 * タスク間でメッセージ（メモリ上のパケットへのポインタ）を受け渡す
 * メールボックス機能を提供します。生成・削除・送信・受信・状態参照の
 * 各 API（tk_cre_mbx / tk_del_mbx / tk_snd_mbx / tk_rcv_mbx / tk_ref_mbx）
 * およびデバッガサポート機能（td_lst_mbx / td_ref_mbx / td_mbx_que）を
 * 実装しています。
 */

#include "kernel.h"
#include "wait.h"
#include "check.h"
#include "mailbox.h"

#if USE_MAILBOX == 1

Noinit(EXPORT MBXCB	knl_mbxcb_table[NUM_MBXID]);	/* メールボックス管理ブロックテーブル */
Noinit(EXPORT QUEUE	knl_free_mbxcb);	/* 未使用管理ブロックのキュー（FreeQue） */


/**
 * @brief メールボックス管理ブロックの初期化
 *
 * すべてのメールボックス管理ブロックを未使用状態にし、
 * FreeQue に登録します。カーネル起動時に呼び出されます。
 *
 * @retval E_OK	正常終了
 * @retval E_SYS	メールボックス数（NUM_MBXID）が 1 未満
 */
EXPORT ER knl_mailbox_initialize( void )
{
	MBXCB	*mbxcb, *end;

	/* システム情報の確認 */
	if ( NUM_MBXID < 1 ) {
		return E_SYS;
	}

	/* 全管理ブロックを FreeQue に登録 */
	QueInit(&knl_free_mbxcb);
	end = knl_mbxcb_table + NUM_MBXID;
	for ( mbxcb = knl_mbxcb_table; mbxcb < end; mbxcb++ ) {
		mbxcb->mbxid = 0;
		QueInsert(&mbxcb->wait_queue, &knl_free_mbxcb);
	}

	return E_OK;
}


/**
 * @brief メールボックスの生成
 *
 * FreeQue から管理ブロックを取り出して初期化し、
 * 空のメッセージキューを持つメールボックスを生成します。
 *
 * @param pk_cmbx	メールボックス生成情報（属性・拡張情報など）
 * @return 正の値ならば生成したメールボックスの ID、負の値ならばエラーコード
 * @retval E_LIMIT	メールボックス数が上限（NUM_MBXID）を超過
 * @retval E_RSATR	不正な属性が指定された（CHK_RSATR 有効時）
 */
SYSCALL ID tk_cre_mbx( CONST T_CMBX *pk_cmbx )
{
#if CHK_RSATR
	const ATR VALID_MBXATR = {
		 TA_MPRI
		|TA_TPRI
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
	};
#endif
	MBXCB	*mbxcb;
	ID	mbxid;
	ER	ercd;

	CHECK_RSATR(pk_cmbx->mbxatr, VALID_MBXATR);

	BEGIN_CRITICAL_SECTION;
	/* FreeQue から管理ブロックを取得 */
	mbxcb = (MBXCB*)QueRemoveNext(&knl_free_mbxcb);
	if ( mbxcb == NULL ) {
		ercd = E_LIMIT;
	} else {
		mbxid = ID_MBX(mbxcb - knl_mbxcb_table);

		/* 管理ブロックの初期化 */
		QueInit(&mbxcb->wait_queue);
		mbxcb->mbxid  = mbxid;
		mbxcb->exinf  = pk_cmbx->exinf;
		mbxcb->mbxatr = pk_cmbx->mbxatr;
		mbxcb->mq_head.msgque[0] = NULL;
#if USE_OBJECT_NAME
		if ( (pk_cmbx->mbxatr & TA_DSNAME) != 0 ) {
			knl_strncpy((char*)mbxcb->name, (char*)pk_cmbx->dsname,
				OBJECT_NAME_LENGTH);
		}
#endif
		ercd = mbxid;
	}
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_DEL_MBX
/**
 * @brief メールボックスの削除
 *
 * 受信待ちのタスクがあれば E_DLT で待ちを解除したうえで、
 * 管理ブロックを FreeQue に返却します。
 * キューに残っているメッセージの解放は行いません。
 *
 * @param mbxid	削除するメールボックスの ID
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象のメールボックスが存在しない
 */
SYSCALL ER tk_del_mbx( ID mbxid )
{
	MBXCB	*mbxcb;
	ER	ercd = E_OK;

	CHECK_MBXID(mbxid);

	mbxcb = get_mbxcb(mbxid);

	BEGIN_CRITICAL_SECTION;
	if ( mbxcb->mbxid == 0 ) {
		ercd = E_NOEXS;
	} else {
		/* 待ちタスクの待ち状態を解除（E_DLT を返す） */
		knl_wait_delete(&mbxcb->wait_queue);

		/* FreeQue へ返却 */
		QueInsert(&mbxcb->wait_queue, &knl_free_mbxcb);
		mbxcb->mbxid = 0;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_MBX */

/**
 * @brief メールボックスへの送信
 *
 * 受信待ちタスクがあれば先頭のタスクへメッセージを直接渡して
 * 待ちを解除します。待ちタスクがなければメッセージキューに接続します。
 * TA_MPRI 属性の場合はメッセージ優先度順に、それ以外は FIFO 順に
 * 接続します。
 *
 * @param mbxid	送信先メールボックスの ID
 * @param pk_msg	送信するメッセージパケットの先頭アドレス
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象のメールボックスが存在しない
 * @retval E_PAR	TA_MPRI 属性でメッセージ優先度が 0 以下
 */
SYSCALL ER tk_snd_mbx( ID mbxid, T_MSG *pk_msg )
{
	MBXCB	*mbxcb;
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_MBXID(mbxid);

	mbxcb = get_mbxcb(mbxid);

	BEGIN_CRITICAL_SECTION;
	if (mbxcb->mbxid == 0) {
		ercd = E_NOEXS;
		goto error_exit;
	}

	if ( (mbxcb->mbxatr & TA_MPRI) != 0 ) {
		if ( ((T_MSG_PRI*)pk_msg)->msgpri <= 0 ) {
			ercd = E_PAR;
			goto error_exit;
		}
	}

	if ( !isQueEmpty(&mbxcb->wait_queue) ) {
		/* 受信待ちタスクへ直接送信 */
		tcb = (TCB*)(mbxcb->wait_queue.next);
		*tcb->winfo.mbx.ppk_msg = pk_msg;
		knl_wait_release_ok(tcb);

	} else {
		/* メッセージをキューに接続 */
		if ( (mbxcb->mbxatr & TA_MPRI) != 0 ) {
			/* 優先度順にキューへ接続 */
			knl_queue_insert_mpri((T_MSG_PRI*)pk_msg, &mbxcb->mq_head);
		} else {
			/* キューの末尾に接続 */
			nextmsg(pk_msg) = NULL;
			if ( headmsg(mbxcb) == NULL ) {
				headmsg(mbxcb) = pk_msg;
			} else {
				nextmsg(mbxcb->mq_tail) = pk_msg;
			}
			mbxcb->mq_tail = pk_msg;
		}
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

/**
 * @brief 待ちタスクの優先度変更時の処理
 *
 * 受信待ちタスクの優先度が変更された際に、TA_TPRI 属性の
 * 待ちキュー内での並び順を新しい優先度に合わせて更新します。
 *
 * @param tcb	優先度が変更されたタスクの TCB
 * @param oldpri	変更前の優先度（本関数では未使用）
 */
LOCAL void mbx_chg_pri( TCB *tcb, INT oldpri )
{
	MBXCB	*mbxcb;

	mbxcb = get_mbxcb(tcb->wid);
	knl_gcb_change_priority((GCB*)mbxcb, tcb);
}

/*
 * メールボックス待ち仕様の定義
 */
LOCAL CONST WSPEC knl_wspec_mbx_tfifo = { TTW_MBX, NULL, NULL };
LOCAL CONST WSPEC knl_wspec_mbx_tpri  = { TTW_MBX, mbx_chg_pri, NULL };

/**
 * @brief メールボックスからの受信
 *
 * メッセージキューにメッセージがあれば先頭のメッセージを取り出して
 * 返します。空の場合は tmout で指定した時間まで受信待ち状態に入ります。
 * 待ち解除条件はメッセージの到着（tk_snd_mbx による直接送信）です。
 *
 * @param mbxid	受信元メールボックスの ID
 * @param ppk_msg	受信したメッセージパケットの先頭アドレスを返す領域
 * @param tmout	タイムアウト時間（TMO_POL / TMO_FEVR 指定可）
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象のメールボックスが存在しない
 * @retval E_TMOUT	タイムアウト（ポーリング失敗を含む）
 * @retval E_RLWAI	待ち状態の強制解除
 * @retval E_DLT	待ちの間にメールボックスが削除された
 * @note ディスパッチ禁止中およびタスク独立部からは呼び出せません。
 */
SYSCALL ER tk_rcv_mbx( ID mbxid, T_MSG **ppk_msg, TMO tmout )
{
	MBXCB	*mbxcb;
	ER	ercd = E_OK;

	CHECK_MBXID(mbxid);
	CHECK_TMOUT(tmout);
	CHECK_DISPATCH();

	mbxcb = get_mbxcb(mbxid);

	BEGIN_CRITICAL_SECTION;
	if ( mbxcb->mbxid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}

	if ( headmsg(mbxcb) != NULL ) {
		/* キューの先頭からメッセージを取得 */
		*ppk_msg = headmsg(mbxcb);
		headmsg(mbxcb) = nextmsg(*ppk_msg);
	} else {
		/* 受信待ち状態に入る準備 */
		knl_ctxtsk->wspec = ( (mbxcb->mbxatr & TA_TPRI) != 0 )?
					&knl_wspec_mbx_tpri: &knl_wspec_mbx_tfifo;
		knl_ctxtsk->wercd = &ercd;
		knl_ctxtsk->winfo.mbx.ppk_msg = ppk_msg;
		knl_gcb_make_wait((GCB*)mbxcb, tmout);
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_REF_MBX
/**
 * @brief メールボックスの状態参照
 *
 * 拡張情報・待ちタスクの有無（先頭タスク ID）・
 * 先頭メッセージのアドレスを pk_rmbx に格納します。
 *
 * @param mbxid	参照するメールボックスの ID
 * @param pk_rmbx	メールボックス状態を返す領域
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象のメールボックスが存在しない
 */
SYSCALL ER tk_ref_mbx( ID mbxid, T_RMBX *pk_rmbx )
{
	MBXCB	*mbxcb;
	ER	ercd = E_OK;

	CHECK_MBXID(mbxid);

	mbxcb = get_mbxcb(mbxid);

	BEGIN_CRITICAL_SECTION;
	if ( mbxcb->mbxid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rmbx->exinf = mbxcb->exinf;
		pk_rmbx->wtsk = knl_wait_tskid(&mbxcb->wait_queue);
		pk_rmbx->pk_msg = headmsg(mbxcb);
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REF_MBX */

/* ------------------------------------------------------------------------ */
/*
 *	デバッガサポート機能
 */
#if USE_DBGSPT

#if USE_OBJECT_NAME
/**
 * @brief 管理ブロックからのオブジェクト名取得
 *
 * TA_DSNAME 属性付きで生成されたメールボックスの名前への
 * ポインタを返します。
 *
 * @param id	対象メールボックスの ID
 * @param name	名前文字列へのポインタを返す領域
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象のメールボックスが存在しない
 * @retval E_OBJ	TA_DSNAME 属性が指定されていない
 */
EXPORT ER knl_mailbox_getname(ID id, UB **name)
{
	MBXCB	*mbxcb;
	ER	ercd = E_OK;

	CHECK_MBXID(id);

	BEGIN_DISABLE_INTERRUPT;
	mbxcb = get_mbxcb(id);
	if ( mbxcb->mbxid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (mbxcb->mbxatr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = mbxcb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */

#ifdef USE_FUNC_TD_LST_MBX
/**
 * @brief メールボックス ID 一覧の参照
 *
 * 使用中のメールボックス ID を list に最大 nent 個まで格納します。
 *
 * @param list	ID を格納する配列
 * @param nent	list に格納できる最大数
 * @return 使用中のメールボックスの総数（nent を超える場合もある）
 */
SYSCALL INT td_lst_mbx( ID list[], INT nent )
{
	MBXCB	*mbxcb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_mbxcb_table + NUM_MBXID;
	for ( mbxcb = knl_mbxcb_table; mbxcb < end; mbxcb++ ) {
		if ( mbxcb->mbxid == 0 ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = mbxcb->mbxid;
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_MBX */

#ifdef USE_FUNC_TD_REF_MBX
/**
 * @brief メールボックスの状態参照（デバッガサポート）
 *
 * 拡張情報・待ちタスクの有無（先頭タスク ID）・
 * 先頭メッセージのアドレスを pk_rmbx に格納します。
 *
 * @param mbxid	参照するメールボックスの ID
 * @param pk_rmbx	メールボックス状態を返す領域
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象のメールボックスが存在しない
 */
SYSCALL ER td_ref_mbx( ID mbxid, TD_RMBX *pk_rmbx )
{
	MBXCB	*mbxcb;
	ER	ercd = E_OK;

	CHECK_MBXID(mbxid);

	mbxcb = get_mbxcb(mbxid);

	BEGIN_DISABLE_INTERRUPT;
	if ( mbxcb->mbxid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rmbx->exinf = mbxcb->exinf;
		pk_rmbx->wtsk = knl_wait_tskid(&mbxcb->wait_queue);
		pk_rmbx->pk_msg = headmsg(mbxcb);
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_MBX */

#ifdef USE_FUNC_TD_MBX_QUE
/**
 * @brief メールボックス待ちキューの参照
 *
 * 受信待ちキューに並ぶタスクの ID を待ち順に list へ
 * 最大 nent 個まで格納します。
 *
 * @param mbxid	参照するメールボックスの ID
 * @param list	タスク ID を格納する配列
 * @param nent	list に格納できる最大数
 * @return 正の値または 0 ならば待ちタスクの総数、負の値ならばエラーコード
 * @retval E_NOEXS	対象のメールボックスが存在しない
 */
SYSCALL INT td_mbx_que( ID mbxid, ID list[], INT nent )
{
	MBXCB	*mbxcb;
	QUEUE	*q;
	ER	ercd = E_OK;

	CHECK_MBXID(mbxid);

	mbxcb = get_mbxcb(mbxid);

	BEGIN_DISABLE_INTERRUPT;
	if ( mbxcb->mbxid == 0 ) {
		ercd = E_NOEXS;
	} else {
		INT n = 0;
		for ( q = mbxcb->wait_queue.next; q != &mbxcb->wait_queue; q = q->next ) {
			if ( n++ < nent ) {
				*list++ = ((TCB*)q)->tskid;
			}
		}
		ercd = n;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_MBX_QUE */

#endif /* USE_DBGSPT */
#endif /* USE_MAILBOX */
