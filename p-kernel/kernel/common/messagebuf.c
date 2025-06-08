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
 * @file messagebuf.c
 * @brief メッセージバッファ管理機能
 * 
 * T-Kernelのメッセージバッファ（Message Buffer）の実装を提供する。
 * メッセージバッファは、可変長のメッセージをタスク間で非同期に送受信するための
 * 同期・通信オブジェクトである。
 * 
 * 主な機能：
 * - メッセージバッファの作成・削除（tk_cre_mbf, tk_del_mbf）
 * - メッセージの送信・受信（tk_snd_mbf, tk_rcv_mbf）
 * - メッセージバッファの状態参照（tk_ref_mbf）
 * - 循環バッファによる効率的なメッセージ管理
 * - 送信・受信待ちキューの管理（FIFO/優先度順）
 * 
 * メッセージバッファは内部的に循環バッファとして実装され、
 * ヘッダ情報付きでメッセージを格納・管理する。
 */

/** [BEGIN Common Definitions] */
#include "kernel.h"
#include "task.h"
#include "wait.h"
#include "check.h"
#include "messagebuf.h"
/** [END Common Definitions] */

#if CFN_MAX_MBFID > 0


#ifdef USE_FUNC_MBFCB_TABLE
Noinit(EXPORT MBFCB knl_mbfcb_table[NUM_MBFID]);	/* Message buffer control block */
Noinit(EXPORT QUEUE knl_free_mbfcb);	/* FreeQue */
#endif /* USE_FUNC_MBFCB_TABLE */


#ifdef USE_FUNC_MESSAGEBUFFER_INITIALIZE
/**
 * @brief メッセージバッファ制御ブロック初期化
 * 
 * システム起動時にメッセージバッファ制御ブロックテーブルを初期化する。
 * 全ての制御ブロックをフリーキューに登録し、使用可能な状態にする。
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_SYS システムエラー（メッセージバッファID数が無効）
 * 
 * @note システム初期化時に一度だけ呼び出される
 * @note 割り込み禁止状態で呼び出される
 */
EXPORT ER knl_messagebuffer_initialize( void )
{
	MBFCB	*mbfcb, *end;

	/* Get system information */
	if ( NUM_MBFID < 1 ) {
		return E_SYS;
	}

	/* Register all control blocks onto FreeQue */
	QueInit(&knl_free_mbfcb);
	end = knl_mbfcb_table + NUM_MBFID;
	for ( mbfcb = knl_mbfcb_table; mbfcb < end; mbfcb++ ) {
		mbfcb->mbfid = 0;
		QueInsert(&mbfcb->send_queue, &knl_free_mbfcb);
	}

	return E_OK;
}
#endif /* USE_FUNC_MESSAGEBUFFER_INITIALIZE */

/* ------------------------------------------------------------------------ */

#ifdef USE_FUNC_MSG_TO_MBF
/**
 * @brief メッセージのメッセージバッファへの格納
 * 
 * 指定されたメッセージを循環バッファに格納する。
 * メッセージにはヘッダ情報が付加され、バッファの境界をまたぐ場合は
 * 2回に分けてコピーされる。
 * 
 * @param mbfcb メッセージバッファ制御ブロックへのポインタ
 * @param msg 格納するメッセージデータへのポインタ
 * @param msgsz メッセージサイズ
 * 
 * @note 空きバッファサイズのチェックは呼び出し側で行う
 * @note 循環バッファの管理により連続領域でないメッセージも格納可能
 */
EXPORT void knl_msg_to_mbf( MBFCB *mbfcb, CONST void *msg, INT msgsz )
{
	W	tail = mbfcb->tail;
	VB	*buffer = mbfcb->buffer;
	W	remsz;

	mbfcb->frbufsz -= (W)(HEADERSZ + ROUNDSZ(msgsz));

	*(HEADER*)&buffer[tail] = msgsz;
	tail += HEADERSZ;
	if ( tail >= mbfcb->bufsz ) {
		tail = 0;
	}

	if ( (remsz = mbfcb->bufsz - tail) < (W)msgsz ) {
		memcpy(&buffer[tail], msg, (size_t)remsz);
		msg = (VB*)msg + remsz;
		msgsz -= (INT)remsz;
		tail = 0;
	}
	memcpy(&buffer[tail], msg, (size_t)msgsz);
	tail += (W)ROUNDSZ(msgsz);
	if ( tail >= mbfcb->bufsz ) {
		tail = 0;
	}

	mbfcb->tail = tail;
}
#endif /* USE_FUNC_MSG_TO_MBF */

/* ------------------------------------------------------------------------ */

#ifdef USE_FUNC_MBF_WAKEUP
/**
 * @brief メッセージバッファ送信待ちタスクの起床処理
 * 
 * メッセージバッファに十分な空き領域がある限り、送信待ちキューから
 * タスクを起床させてメッセージを格納する。
 * 
 * @param mbfcb メッセージバッファ制御ブロックへのポインタ
 * 
 * @note 送信待ちキューの先頭から順番に処理される
 * @note 各タスクのメッセージサイズがバッファに収まる場合のみ起床させる
 * @note メッセージ受信後やバッファ操作後に呼び出される
 */
EXPORT void knl_mbf_wakeup( MBFCB *mbfcb )
{
	TCB	*top;
	INT	msgsz;

	while ( !isQueEmpty(&mbfcb->send_queue) ) {
		top = (TCB*)mbfcb->send_queue.next;
		msgsz = top->winfo.smbf.msgsz;
		if ( !knl_mbf_free(mbfcb, msgsz) ) {
			break;
		}

		/* Store a message from waiting task and release it */
		knl_msg_to_mbf(mbfcb, top->winfo.smbf.msg, msgsz);
		knl_wait_release_ok(top);
	}
}
#endif /* USE_FUNC_MBF_WAKEUP */


#ifdef USE_FUNC_TK_CRE_MBF
/**
 * @brief メッセージバッファ生成
 * 
 * 指定されたパラメータに基づいてメッセージバッファを生成する。
 * ユーザーバッファまたはシステムが確保したメモリを使用してバッファを構築する。
 * 
 * @param pk_cmbf メッセージバッファ生成情報パケットへのポインタ
 * 
 * @return ID 生成されたメッセージバッファID
 * @retval 正の値 生成されたメッセージバッファのID
 * @retval E_RSATR 予約属性エラー
 * @retval E_PAR パラメータエラー（バッファサイズや最大メッセージサイズが不正等）
 * @retval E_NOMEM メモリ不足
 * @retval E_LIMIT メッセージバッファ数の上限超過
 * 
 * @note TA_USERBUFが指定された場合はユーザー提供のバッファを使用
 * @note TA_TPRIが指定された場合は優先度順で待ちキューを管理
 */
SYSCALL ID tk_cre_mbf_impl( CONST T_CMBF *pk_cmbf )
{
#if CHK_RSATR
	const ATR VALID_MBFATR = {
		 TA_TPRI
		|TA_USERBUF
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
	};
#endif
	MBFCB	*mbfcb;
	ID	mbfid;
	W	bufsz;
	VB	*msgbuf;
	ER	ercd;

	CHECK_RSATR(pk_cmbf->mbfatr, VALID_MBFATR);
	CHECK_PAR(pk_cmbf->bufsz >= 0);
	CHECK_PAR(pk_cmbf->maxmsz > 0);
#if !USE_IMALLOC
	/* TA_USERBUF must be specified if configured in no Imalloc */
	CHECK_PAR((pk_cmbf->mbfatr & TA_USERBUF) != 0);
#endif
	bufsz = (W)ROUNDSZ(pk_cmbf->bufsz);

	if ( bufsz > 0 ) {
#if USE_IMALLOC
		if ( (pk_cmbf->mbfatr & TA_USERBUF) != 0 ) {
			/* Size of user buffer must be multiples of sizeof(HEADER) */
			if ( bufsz != pk_cmbf->bufsz ) {
				return E_PAR;
			}
			/* Use user buffer */
			msgbuf = (VB*) pk_cmbf->bufptr;
		} else {
			/* Allocate by kernel */
			msgbuf = knl_Imalloc((UW)bufsz);
			if ( msgbuf == NULL ) {
				return E_NOMEM;
			}
		}
#else
		/* Size of user buffer must be multiples of sizeof(HEADER) */
		if ( bufsz != pk_cmbf->bufsz ) {
			return E_PAR;
		}
		/* Use user buffer */
		msgbuf = (VB*) pk_cmbf->bufptr;
#endif
	} else {
		msgbuf = NULL;
	}

	BEGIN_CRITICAL_SECTION;
	/* Get control block from FreeQue */
	mbfcb = (MBFCB*)QueRemoveNext(&knl_free_mbfcb);
	if ( mbfcb == NULL ) {
		ercd = E_LIMIT;
	} else {
		mbfid = ID_MBF(mbfcb - knl_mbfcb_table);

		/* Initialize control block */
		QueInit(&mbfcb->send_queue);
		mbfcb->mbfid = mbfid;
		mbfcb->exinf = pk_cmbf->exinf;
		mbfcb->mbfatr = pk_cmbf->mbfatr;
		QueInit(&mbfcb->recv_queue);
		mbfcb->buffer = msgbuf;
		mbfcb->bufsz = mbfcb->frbufsz = bufsz;
		mbfcb->maxmsz = pk_cmbf->maxmsz;
		mbfcb->head = mbfcb->tail = 0;
#if USE_OBJECT_NAME
		if ( (pk_cmbf->mbfatr & TA_DSNAME) != 0 ) {
			strncpy((char*)mbfcb->name, (char*)pk_cmbf->dsname,
				OBJECT_NAME_LENGTH);
		}
#endif
		ercd = mbfid;
	}
	END_CRITICAL_SECTION;

#if USE_IMALLOC
	if ( (ercd < E_OK) && (msgbuf != NULL) && ((pk_cmbf->mbfatr & TA_USERBUF) == 0 ) ) {
		knl_Ifree(msgbuf);
	}
#endif

	return ercd;
}
#endif /* USE_FUNC_TK_CRE_MBF */

#ifdef USE_FUNC_TK_DEL_MBF
/**
 * @brief メッセージバッファ削除
 * 
 * 指定されたメッセージバッファを削除し、関連リソースを解放する。
 * 送信・受信待ちタスクがある場合は全て起床させる（E_DLTエラーで）。
 * 
 * @param mbfid 削除するメッセージバッファのID
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正ID
 * @retval E_NOEXS オブジェクト未生成
 * 
 * @note TA_USERBUFでない場合はシステムが確保したメモリも解放される
 * @note 削除後、該当IDのメッセージバッファは使用できなくなる
 */
SYSCALL ER tk_del_mbf_impl( ID mbfid )
{
	MBFCB	*mbfcb;
	VB	*msgbuf = NULL;
	ER	ercd = E_OK;

	CHECK_MBFID(mbfid);

	mbfcb = get_mbfcb(mbfid);

	BEGIN_CRITICAL_SECTION;
	if ( mbfcb->mbfid == 0 ) {
		ercd = E_NOEXS;
	} else {
		msgbuf = mbfcb->buffer;

		/* Release wait state of task (E_DLT) */
		knl_wait_delete(&mbfcb->recv_queue);
		knl_wait_delete(&mbfcb->send_queue);

		/* Return to FreeQue */
		QueInsert(&mbfcb->send_queue, &knl_free_mbfcb);
		mbfcb->mbfid = 0;
	}
	END_CRITICAL_SECTION;

#if USE_IMALLOC
	if ( msgbuf != NULL && ((mbfcb->mbfatr & TA_USERBUF) == 0 ) ) {
		knl_Ifree(msgbuf);
	}
#endif

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_MBF */

#ifdef USE_FUNC_TK_SND_MBF
/**
 * @brief 送信待ちタスクの優先度変更時処理
 * 
 * メッセージバッファで送信待ち中のタスクの優先度が変更された場合の処理を行う。
 * 優先度順の待ちキューを再構築し、新しい優先度に基づいてメッセージ送信を試行する。
 * 
 * @param tcb 優先度が変更されたタスクの制御ブロック
 * @param oldpri 変更前の優先度（負の値の場合は新規待ち）
 * 
 * @note 優先度変更により新たにメッセージ送信可能なタスクが生じる可能性がある
 */
LOCAL void knl_mbf_chg_pri( TCB *tcb, INT oldpri )
{
	MBFCB	*mbfcb;

	mbfcb = get_mbfcb(tcb->wid);
	if ( oldpri >= 0 ) {
		/* Reorder wait queue */
		knl_gcb_change_priority((GCB*)mbfcb, tcb);
	}

	/* If the new head task in a send wait queue is able to sent, 
	   send its message */
	knl_mbf_wakeup(mbfcb);
}

/**
 * @brief 送信待ちタスク解放時処理
 * 
 * メッセージバッファで送信待ち中のタスクが解放される際の処理を行う。
 * 
 * @param tcb 解放されるタスクの制御ブロック
 * 
 * @note 内部的にknl_mbf_chg_pri()を呼び出して処理を委譲する
 */
LOCAL void knl_mbf_rel_wai( TCB *tcb )
{
	knl_mbf_chg_pri(tcb, -1);
}

/*
 * Definition of message buffer wait specification
 */
LOCAL CONST WSPEC knl_wspec_smbf_tfifo = { TTW_SMBF, NULL, knl_mbf_rel_wai };
LOCAL CONST WSPEC knl_wspec_smbf_tpri  = { TTW_SMBF, knl_mbf_chg_pri, knl_mbf_rel_wai };

/**
 * @brief メッセージバッファへのメッセージ送信
 * 
 * 指定されたメッセージバッファにメッセージを送信する。
 * 受信待ちタスクがあれば直接転送し、なければバッファに格納する。
 * バッファが満杯の場合は指定されたタイムアウト時間まで待機する。
 * 
 * @param mbfid メッセージバッファID
 * @param msg 送信するメッセージデータへのポインタ
 * @param msgsz メッセージサイズ
 * @param tmout タイムアウト時間（TMO_POL:ポーリング、TMO_FEVR:永久待ち）
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正ID
 * @retval E_PAR パラメータエラー（サイズが0以下または最大メッセージサイズ超過等）
 * @retval E_NOEXS オブジェクト未生成
 * @retval E_TMOUT タイムアウト発生
 * @retval E_RLWAI 待ち状態の強制解除
 * 
 * @note 受信待ちタスクがある場合は直接データ転送される
 * @note メッセージバッファの属性によりFIFO順または優先度順で待機する
 */
SYSCALL ER tk_snd_mbf_impl( ID mbfid, CONST void *msg, INT msgsz, TMO tmout )
{
	MBFCB	*mbfcb;
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_MBFID(mbfid);
	CHECK_PAR(msgsz > 0);
	CHECK_TMOUT(tmout);
	CHECK_DISPATCH_POL(tmout);

	mbfcb = get_mbfcb(mbfid);

	BEGIN_CRITICAL_SECTION;
	if ( mbfcb->mbfid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
#if CHK_PAR
	if ( msgsz > mbfcb->maxmsz ) {
		ercd = E_PAR;
		goto error_exit;
	}
#endif

	if ( !isQueEmpty(&mbfcb->recv_queue) ) {
		/* Send directly to the receive wait task */
		tcb = (TCB*)mbfcb->recv_queue.next;
		memcpy(tcb->winfo.rmbf.msg, msg, (size_t)msgsz);
		*tcb->winfo.rmbf.p_msgsz = msgsz;
		knl_wait_release_ok(tcb);

	} else if ( (in_indp() || knl_gcb_top_of_wait_queue((GCB*)mbfcb, knl_ctxtsk) == knl_ctxtsk)
		  &&(knl_mbf_free(mbfcb, msgsz)) ) {
		/* Store the message to message buffer */
		knl_msg_to_mbf(mbfcb, msg, msgsz);

	} else {
		ercd = E_TMOUT;
		if ( tmout != TMO_POL ) {
			/* Ready for send wait */
			knl_ctxtsk->wspec = ( (mbfcb->mbfatr & TA_TPRI) != 0 )?
					&knl_wspec_smbf_tpri: &knl_wspec_smbf_tfifo;
			knl_ctxtsk->wercd = &ercd;
			knl_ctxtsk->winfo.smbf.msg = msg;
			knl_ctxtsk->winfo.smbf.msgsz = msgsz;
			knl_gcb_make_wait((GCB*)mbfcb, tmout);
		}
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_SND_MBF */

#ifdef USE_FUNC_TK_RCV_MBF

LOCAL CONST WSPEC knl_wspec_rmbf = { TTW_RMBF, NULL, NULL };

/**
 * @brief メッセージバッファからのメッセージ取得
 * 
 * メッセージバッファから1つのメッセージを取得し、指定されたバッファに
 * コピーする。循環バッファの管理により、境界をまたぐメッセージも適切に処理される。
 * 
 * @param mbfcb メッセージバッファ制御ブロックへのポインタ
 * @param msg メッセージを格納するバッファへのポインタ
 * 
 * @return INT メッセージサイズ
 * 
 * @note バッファからメッセージを削除し、空きバッファサイズを更新する
 * @note 循環バッファの特性により分割されたメッセージも正常に復元される
 */
LOCAL INT knl_mbf_to_msg( MBFCB *mbfcb, void *msg )
{
	W	head = mbfcb->head;
	VB	*buffer = mbfcb->buffer;
	INT	msgsz, actsz;
	W	remsz;

	actsz = msgsz = *(HEADER*)&buffer[head];
	mbfcb->frbufsz += (W)(HEADERSZ + ROUNDSZ(msgsz));

	head += (W)HEADERSZ;
	if ( head >= mbfcb->bufsz ) {
		head = 0;
	}

	if ( (remsz = mbfcb->bufsz - head) < (W)msgsz ) {
		memcpy(msg, &buffer[head], (size_t)remsz);
		msg = (VB*)msg + remsz;
		msgsz -= (INT)remsz;
		head = 0;
	}
	memcpy(msg, &buffer[head], (size_t)msgsz);
	head += (INT)ROUNDSZ(msgsz);
	if ( head >= mbfcb->bufsz ) {
		head = 0;
	}

	mbfcb->head = head;

	return actsz;
}

/**
 * @brief メッセージバッファからのメッセージ受信
 * 
 * 指定されたメッセージバッファからメッセージを受信する。
 * バッファにメッセージがあれば取得し、なければ送信待ちタスクから直接受信する。
 * メッセージがない場合は指定されたタイムアウト時間まで待機する。
 * 
 * @param mbfid メッセージバッファID
 * @param msg 受信したメッセージを格納するバッファへのポインタ
 * @param tmout タイムアウト時間（TMO_POL:ポーリング、TMO_FEVR:永久待ち）
 * 
 * @return INT 受信したメッセージサイズ
 * @retval 正の値 受信したメッセージのサイズ
 * @retval E_ID 不正ID
 * @retval E_NOEXS オブジェクト未生成
 * @retval E_TMOUT タイムアウト発生
 * @retval E_RLWAI 待ち状態の強制解除
 * 
 * @note 送信待ちタスクがある場合は直接データ転送される
 * @note 受信によりバッファに空きが生じると送信待ちタスクが起床する可能性がある
 */
SYSCALL INT tk_rcv_mbf_impl( ID mbfid, void *msg, TMO tmout )
{
	MBFCB	*mbfcb;
	TCB	*tcb;
	INT	rcvsz;
	ER	ercd = E_OK;

	CHECK_MBFID(mbfid);
	CHECK_TMOUT(tmout);
	CHECK_DISPATCH();

	mbfcb = get_mbfcb(mbfid);

	BEGIN_CRITICAL_SECTION;
	if (mbfcb->mbfid == 0) {
		ercd = E_NOEXS;
		goto error_exit;
	}

	if ( !knl_mbf_empty(mbfcb) ) {
		/* Read from message buffer */
		rcvsz = knl_mbf_to_msg(mbfcb, msg);

		/* Accept message from sending task(s) */
		knl_mbf_wakeup(mbfcb);

	} else if ( !isQueEmpty(&mbfcb->send_queue) ) {
		/* Receive directly from send wait task */
		tcb = (TCB*)mbfcb->send_queue.next;
		rcvsz = tcb->winfo.smbf.msgsz;
		memcpy(msg, tcb->winfo.smbf.msg, (size_t)rcvsz);
		knl_wait_release_ok(tcb);
		knl_mbf_wakeup(mbfcb);
	} else {
		ercd = E_TMOUT;
		if ( tmout != TMO_POL ) {
			/* Ready for receive wait */
			knl_ctxtsk->wspec = &knl_wspec_rmbf;
			knl_ctxtsk->wid = mbfid;
			knl_ctxtsk->wercd = &ercd;
			knl_ctxtsk->winfo.rmbf.msg = msg;
			knl_ctxtsk->winfo.rmbf.p_msgsz = &rcvsz;
			knl_make_wait(tmout, mbfcb->mbfatr);
			QueInsert(&knl_ctxtsk->tskque, &mbfcb->recv_queue);
		}
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ( ercd < E_OK )? ercd: rcvsz;
}
#endif /* USE_FUNC_TK_RCV_MBF */

#ifdef USE_FUNC_TK_REF_MBF
/**
 * @brief メッセージバッファ状態参照
 * 
 * 指定されたメッセージバッファの現在の状態情報を取得する。
 * 待機タスク情報、メッセージサイズ、空きバッファサイズ等の情報を提供する。
 * 
 * @param mbfid メッセージバッファID
 * @param pk_rmbf メッセージバッファ状態情報を格納するパケットへのポインタ
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正ID
 * @retval E_NOEXS オブジェクト未生成
 * 
 * @note 返される情報には受信・送信待ちタスクID、先頭メッセージサイズ、
 *       空きバッファサイズ、最大メッセージサイズが含まれる
 * @note 状態参照時点でのスナップショット情報である
 */
SYSCALL ER tk_ref_mbf_impl( ID mbfid, T_RMBF *pk_rmbf )
{
	MBFCB	*mbfcb;
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_MBFID(mbfid);

	mbfcb = get_mbfcb(mbfid);

	BEGIN_CRITICAL_SECTION;
	if ( mbfcb->mbfid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rmbf->exinf = mbfcb->exinf;
		pk_rmbf->wtsk = knl_wait_tskid(&mbfcb->recv_queue);
		pk_rmbf->stsk = knl_wait_tskid(&mbfcb->send_queue);
		if ( !knl_mbf_empty(mbfcb) ) {
			pk_rmbf->msgsz = *(HEADER*)&mbfcb->buffer[mbfcb->head];
		} else {
			if ( !isQueEmpty(&mbfcb->send_queue) ) {
				tcb = (TCB*)mbfcb->send_queue.next;
				pk_rmbf->msgsz = tcb->winfo.smbf.msgsz;
			} else {
				pk_rmbf->msgsz = 0;
			}
		}
		pk_rmbf->frbufsz = mbfcb->frbufsz;
		pk_rmbf->maxmsz = mbfcb->maxmsz;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REF_MBF */

/* ------------------------------------------------------------------------ */
/*
 *	Debugger support function
 */
#if USE_DBGSPT

#ifdef USE_FUNC_MESSAGEBUFFER_GETNAME
#if USE_OBJECT_NAME
/*
 * Get object name from control block
 */
EXPORT ER knl_messagebuffer_getname(ID id, UB **name)
{
	MBFCB	*mbfcb;
	ER	ercd = E_OK;

	CHECK_MBFID(id);

	BEGIN_DISABLE_INTERRUPT;
	mbfcb = get_mbfcb(id);
	if ( mbfcb->mbfid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (mbfcb->mbfatr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = mbfcb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */
#endif /* USE_FUNC_MESSAGEBUFFER_GETNAME */

#ifdef USE_FUNC_TD_LST_MBF
/*
 * Refer message buffer usage state
 */
SYSCALL INT td_lst_mbf_impl( ID list[], INT nent )
{
	MBFCB	*mbfcb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_mbfcb_table + NUM_MBFID;
	for ( mbfcb = knl_mbfcb_table; mbfcb < end; mbfcb++ ) {
		if ( mbfcb->mbfid == 0 ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = mbfcb->mbfid;
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_MBF */

#ifdef USE_FUNC_TD_REF_MBF
/*
 * Refer message buffer state
 */
SYSCALL ER td_ref_mbf_impl( ID mbfid, TD_RMBF *pk_rmbf )
{
	MBFCB	*mbfcb;
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_MBFID(mbfid);

	mbfcb = get_mbfcb(mbfid);

	BEGIN_DISABLE_INTERRUPT;
	if ( mbfcb->mbfid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rmbf->exinf = mbfcb->exinf;
		pk_rmbf->wtsk = knl_wait_tskid(&mbfcb->recv_queue);
		pk_rmbf->stsk = knl_wait_tskid(&mbfcb->send_queue);
		if ( !knl_mbf_empty(mbfcb) ) {
			pk_rmbf->msgsz = *(HEADER*)&mbfcb->buffer[mbfcb->head];
		} else {
			if ( !isQueEmpty(&mbfcb->send_queue) ) {
				tcb = (TCB*)mbfcb->send_queue.next;
				pk_rmbf->msgsz = tcb->winfo.smbf.msgsz;
			} else {
				pk_rmbf->msgsz = 0;
			}
		}
		pk_rmbf->frbufsz = mbfcb->frbufsz;
		pk_rmbf->maxmsz = mbfcb->maxmsz;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_MBF */

#ifdef USE_FUNC_TD_SMBF_QUE
/*
 * Refer message buffer send wait queue
 */
SYSCALL INT td_smbf_que_impl( ID mbfid, ID list[], INT nent )
{
	MBFCB	*mbfcb;
	QUEUE	*q;
	ER	ercd = E_OK;

	CHECK_MBFID(mbfid);

	mbfcb = get_mbfcb(mbfid);

	BEGIN_DISABLE_INTERRUPT;
	if ( mbfcb->mbfid == 0 ) {
		ercd = E_NOEXS;
	} else {
		INT n = 0;
		for ( q = mbfcb->send_queue.next; q != &mbfcb->send_queue; q = q->next ) {
			if ( n++ < nent ) {
				*list++ = ((TCB*)q)->tskid;
			}
		}
		ercd = n;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_SMBF_QUE */

#ifdef USE_FUNC_TD_RMBF_QUE
/*
 * Refer message buffer receive wait queue
 */
SYSCALL INT td_rmbf_que_impl( ID mbfid, ID list[], INT nent )
{
	MBFCB	*mbfcb;
	QUEUE	*q;
	ER	ercd = E_OK;

	CHECK_MBFID(mbfid);

	mbfcb = get_mbfcb(mbfid);

	BEGIN_DISABLE_INTERRUPT;
	if ( mbfcb->mbfid == 0 ) {
		ercd = E_NOEXS;
	} else {
		INT n = 0;
		for ( q = mbfcb->recv_queue.next; q != &mbfcb->recv_queue; q = q->next ) {
			if ( n++ < nent ) {
				*list++ = ((TCB*)q)->tskid;
			}
		}
		ercd = n;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_RMBF_QUE */

#endif /* USE_DBGSPT */
#endif /* CFN_MAX_MBFID */
