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
 * @file mailbox.c
 * @brief メールボックス管理機能
 * 
 * T-Kernelのメールボックス機能を実装します。
 * メールボックスは、タスク間でメッセージを非同期に送受信するための通信機構です。
 * 
 * 主な機能：
 * - メールボックスの作成・削除
 * - メッセージの送信・受信
 * - 優先度付きメッセージキューの管理
 * - FIFO/優先度順のタスク待ち管理
 * - デバッグサポート機能
 * 
 * メールボックスの特徴：
 * - 可変長メッセージの送受信
 * - メッセージ優先度による順序制御（TA_MPRI指定時）
 * - 受信待ちタスクの優先度管理（TA_TPRI指定時）
 * - ゼロコピーによる効率的なメッセージ転送
 */

/** [BEGIN Common Definitions] */
#include "kernel.h"
#include "task.h"
#include "wait.h"
#include "check.h"
#include "mailbox.h"
/** [END Common Definitions] */

#if CFN_MAX_MBXID > 0

#ifdef USE_FUNC_MBXCB_TABLE
Noinit(EXPORT MBXCB	knl_mbxcb_table[NUM_MBXID]);	/* Mailbox control block */
Noinit(EXPORT QUEUE	knl_free_mbxcb);	/* FreeQue */
#endif /* USE_FUNC_MBXCB_TABLE */


#ifdef USE_FUNC_MAILBOX_INITIALIZE
/**
 * @brief メールボックス制御ブロックの初期化
 * 
 * システム起動時にメールボックス管理機構を初期化します。
 * 全てのメールボックス制御ブロックを空きキューに登録し、
 * 使用可能な状態にします。
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_SYS システムエラー（メールボックス数が不正）
 * 
 * @note この関数はシステム初期化時に一度だけ呼び出されます
 * @note NUM_MBXID が1未満の場合はエラーとなります
 */
EXPORT ER knl_mailbox_initialize( void )
{
	MBXCB	*mbxcb, *end;

	/* Get system information */
	if ( NUM_MBXID < 1 ) {
		return E_SYS;
	}

	/* Register all control blocks onto FreeQue */
	QueInit(&knl_free_mbxcb);
	end = knl_mbxcb_table + NUM_MBXID;
	for ( mbxcb = knl_mbxcb_table; mbxcb < end; mbxcb++ ) {
		mbxcb->mbxid = 0;
		QueInsert(&mbxcb->wait_queue, &knl_free_mbxcb);
	}

	return E_OK;
}
#endif /* USE_FUNC_MAILBOX_INITIALIZE */


#ifdef USE_FUNC_TK_CRE_MBX
/**
 * @brief メールボックスの作成
 * 
 * 指定された属性でメールボックスを作成します。
 * 作成されたメールボックスには一意のIDが割り当てられます。
 * 
 * @param pk_cmbx メールボックス作成情報パケットへのポインタ
 * @return ID 作成されたメールボックスID（正の値）、またはエラーコード（負の値）
 * @retval E_LIMIT 利用可能なメールボックスがない
 * @retval E_RSATR 予約属性が指定された
 * 
 * 対応する属性：
 * - TA_MPRI: メッセージ優先度順キューイング
 * - TA_TPRI: タスク優先度順待ち
 * - TA_DSNAME: オブジェクト名の指定
 * 
 * @note メールボックスIDは1以上の値が割り当てられます
 * @note 作成時点ではメッセージキューは空です
 */
SYSCALL ID tk_cre_mbx_impl( CONST T_CMBX *pk_cmbx )
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
	/* Get control block from FreeQue */
	mbxcb = (MBXCB*)QueRemoveNext(&knl_free_mbxcb);
	if ( mbxcb == NULL ) {
		ercd = E_LIMIT;
	} else {
		mbxid = ID_MBX(mbxcb - knl_mbxcb_table);

		/* Initialize control block */
		QueInit(&mbxcb->wait_queue);
		mbxcb->mbxid  = mbxid;
		mbxcb->exinf  = pk_cmbx->exinf;
		mbxcb->mbxatr = pk_cmbx->mbxatr;
		mbxcb->mq_head.msgque[0] = NULL;
#if USE_OBJECT_NAME
		if ( (pk_cmbx->mbxatr & TA_DSNAME) != 0 ) {
			strncpy((char*)mbxcb->name, (char*)pk_cmbx->dsname,
				OBJECT_NAME_LENGTH);
		}
#endif
		ercd = mbxid;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_CRE_MBX */

#ifdef USE_FUNC_TK_DEL_MBX
/**
 * @brief メールボックスの削除
 * 
 * 指定されたメールボックスを削除し、リソースを解放します。
 * 削除時に待ちタスクがある場合は、E_DLTエラーで待ち解除されます。
 * 
 * @param mbxid 削除対象のメールボックスID
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正なメールボックスID
 * @retval E_NOEXS 指定のメールボックスが存在しない
 * 
 * @note 削除されたメールボックスIDは再利用される可能性があります
 * @note 削除時点でキューに残っているメッセージは破棄されます
 * @note 待ちタスクは全てE_DLTエラーで待ち解除されます
 */
SYSCALL ER tk_del_mbx_impl( ID mbxid )
{
	MBXCB	*mbxcb;
	ER	ercd = E_OK;

	CHECK_MBXID(mbxid);

	mbxcb = get_mbxcb(mbxid);

	BEGIN_CRITICAL_SECTION;
	if ( mbxcb->mbxid == 0 ) {
		ercd = E_NOEXS;
	} else {
		/* Release wait state of task (E_DLT) */
		knl_wait_delete(&mbxcb->wait_queue);

		/* Return to FreeQue */
		QueInsert(&mbxcb->wait_queue, &knl_free_mbxcb);
		mbxcb->mbxid = 0;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_MBX */

#ifdef USE_FUNC_TK_SND_MBX
/**
 * @brief メールボックスへのメッセージ送信
 * 
 * 指定されたメールボックスにメッセージを送信します。
 * 受信待ちタスクがある場合は直接メッセージを渡し、
 * 待ちタスクがない場合はメッセージキューに追加します。
 * 
 * @param mbxid 送信先のメールボックスID
 * @param pk_msg 送信するメッセージへのポインタ
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正なメールボックスID
 * @retval E_NOEXS 指定のメールボックスが存在しない
 * @retval E_PAR 不正なメッセージ優先度（TA_MPRI時）
 * 
 * メッセージキューイング方式：
 * - TA_MPRI未指定: FIFO順でキューに追加
 * - TA_MPRI指定: メッセージ優先度順でキューに挿入
 * 
 * @note メッセージの内容はコピーされず、ポインタのみが転送されます
 * @note TA_MPRI指定時は、メッセージ優先度が1以上である必要があります
 */
SYSCALL ER tk_snd_mbx_impl( ID mbxid, T_MSG *pk_msg )
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
		/* Directly send to receive wait task */
		tcb = (TCB*)(mbxcb->wait_queue.next);
		*tcb->winfo.mbx.ppk_msg = pk_msg;
		knl_wait_release_ok(tcb);

	} else {
		/* Connect message to queue */
		if ( (mbxcb->mbxatr & TA_MPRI) != 0 ) {
			/* Connect message to queue following priority */
			knl_queue_insert_mpri((T_MSG_PRI*)pk_msg, &mbxcb->mq_head);
		} else {
			/* Connect to end of queue */
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
#endif /* USE_FUNC_TK_SND_MBX */

#ifdef USE_FUNC_TK_RCV_MBX
/**
 * @brief 待ちタスクの優先度変更時の処理
 * 
 * メールボックス受信待ち中のタスクの優先度が変更された場合に
 * 呼び出される内部関数です。待ちキューの順序を再調整します。
 * 
 * @param tcb 優先度が変更されたタスクの制御ブロック
 * @param oldpri 変更前の優先度（未使用）
 * 
 * @note この関数はTA_TPRI属性のメールボックスでのみ使用されます
 */
LOCAL void mbx_chg_pri( TCB *tcb, INT oldpri )
{
	MBXCB	*mbxcb;

	mbxcb = get_mbxcb(tcb->wid);
	knl_gcb_change_priority((GCB*)mbxcb, tcb);
}

/**
 * @brief メールボックス待ち仕様の定義
 * 
 * FIFO順待ち（TA_TPRI未指定）と優先度順待ち（TA_TPRI指定）の
 * 待ち仕様を定義します。
 */
LOCAL CONST WSPEC knl_wspec_mbx_tfifo = { TTW_MBX, NULL, NULL };
LOCAL CONST WSPEC knl_wspec_mbx_tpri  = { TTW_MBX, mbx_chg_pri, NULL };

/**
 * @brief メールボックスからのメッセージ受信
 * 
 * 指定されたメールボックスからメッセージを受信します。
 * メッセージがある場合は即座に取得し、ない場合は指定時間まで待機します。
 * 
 * @param mbxid 受信元のメールボックスID
 * @param ppk_msg 受信したメッセージポインタを格納する領域
 * @param tmout タイムアウト時間（TMO_FEVR=無限待ち、TMO_POL=ポーリング）
 * @return ER エラーコード
 * @retval E_OK 正常終了（メッセージ受信成功）
 * @retval E_ID 不正なメールボックスID
 * @retval E_NOEXS 指定のメールボックスが存在しない
 * @retval E_TMOUT タイムアウト
 * @retval E_RLWAI 待ち状態の強制解除
 * @retval E_DLT 待ち対象の削除
 * @retval E_CTX コンテキストエラー
 * 
 * @note 受信したメッセージは呼び出し側が責任を持って処理する必要があります
 * @note TA_TPRI指定時は優先度順、未指定時はFIFO順で待ちキューが管理されます
 */
SYSCALL ER tk_rcv_mbx_impl( ID mbxid, T_MSG **ppk_msg, TMO tmout )
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
		/* Get message from head of queue */
		*ppk_msg = headmsg(mbxcb);
		headmsg(mbxcb) = nextmsg(*ppk_msg);
	} else {
		/* Ready for receive wait */
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
#endif /* USE_FUNC_TK_RCV_MBX */

#ifdef USE_FUNC_TK_REF_MBX
/**
 * @brief メールボックス状態の参照
 * 
 * 指定されたメールボックスの現在の状態を取得します。
 * 拡張情報、待ちタスクID、先頭メッセージポインタを返します。
 * 
 * @param mbxid 参照対象のメールボックスID
 * @param pk_rmbx メールボックス状態を格納する領域
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正なメールボックスID
 * @retval E_NOEXS 指定のメールボックスが存在しない
 * 
 * 取得される情報：
 * - exinf: 拡張情報
 * - wtsk: 受信待ちタスクのID（待ちタスクがない場合は0）
 * - pk_msg: 先頭メッセージへのポインタ（メッセージがない場合はNULL）
 * 
 * @note この関数は状態参照のみで、メールボックスの動作には影響しません
 */
SYSCALL ER tk_ref_mbx_impl( ID mbxid, T_RMBX *pk_rmbx )
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
/**
 * @brief デバッガサポート機能
 * 
 * デバッグ時のメールボックス情報取得機能を提供します。
 */
#if USE_DBGSPT

#ifdef USE_FUNC_MAILBOX_GETNAME
#if USE_OBJECT_NAME
/**
 * @brief メールボックスオブジェクト名の取得
 * 
 * デバッガ用：指定されたメールボックスIDからオブジェクト名を取得します。
 * 
 * @param id メールボックスID
 * @param name オブジェクト名を格納するポインタ変数
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正なメールボックスID
 * @retval E_NOEXS 指定のメールボックスが存在しない
 * @retval E_OBJ オブジェクト名が設定されていない
 * 
 * @note この関数はデバッグサポート機能が有効な場合のみ利用可能です
 * @note TA_DSNAMEが指定されていないメールボックスではE_OBJエラーとなります
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
#endif /* USE_FUNC_MAILBOX_GETNAME */

#ifdef USE_FUNC_TD_LST_MBX
/**
 * @brief メールボックス使用状況の参照
 * 
 * デバッガ用：現在作成されているメールボックスのIDリストを取得します。
 * 
 * @param list メールボックスIDを格納する配列
 * @param nent 配列の要素数
 * @return INT 実際のメールボックス数
 * 
 * @note 戻り値が nent より大きい場合、全てのIDを取得するには
 *       より大きな配列が必要です
 * @note この関数はデバッグサポート機能が有効な場合のみ利用可能です
 */
SYSCALL INT td_lst_mbx_impl( ID list[], INT nent )
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
 * @brief メールボックス状態の参照（デバッガ用）
 * 
 * デバッガ用：指定されたメールボックスの詳細な状態を取得します。
 * tk_ref_mbx と同様の情報を取得しますが、デバッガ用の追加情報も含みます。
 * 
 * @param mbxid 参照対象のメールボックスID
 * @param pk_rmbx メールボックス状態を格納する領域
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正なメールボックスID
 * @retval E_NOEXS 指定のメールボックスが存在しない
 * 
 * @note この関数はデバッグサポート機能が有効な場合のみ利用可能です
 */
SYSCALL ER td_ref_mbx_impl( ID mbxid, TD_RMBX *pk_rmbx )
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
 * デバッガ用：指定されたメールボックスで受信待ちしているタスクのIDリストを取得します。
 * 
 * @param mbxid 対象のメールボックスID
 * @param list 待ちタスクIDを格納する配列
 * @param nent 配列の要素数
 * @return INT 実際の待ちタスク数（正の値）、またはエラーコード（負の値）
 * @retval E_ID 不正なメールボックスID
 * @retval E_NOEXS 指定のメールボックスが存在しない
 * 
 * @note 戻り値が nent より大きい場合、全てのタスクIDを取得するには
 *       より大きな配列が必要です
 * @note この関数はデバッグサポート機能が有効な場合のみ利用可能です
 */
SYSCALL INT td_mbx_que_impl( ID mbxid, ID list[], INT nent )
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
#endif /* CFN_MAX_MBXID */
