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
 * @file	messagebuf.c
 * @brief	メッセージバッファ機能の実装
 *
 * 可変長メッセージをリングバッファ経由で送受信するメッセージバッファの
 * 生成・削除・送信・受信・状態参照 API（tk_cre_mbf 等）と、
 * デバッガサポート機能（td_lst_mbf 等）を提供します。
 */

#include "kernel.h"
#include "wait.h"
#include "check.h"
#include "messagebuf.h"

#if USE_MESSAGEBUFFER == 1


Noinit(EXPORT MBFCB knl_mbfcb_table[NUM_MBFID]);	/* メッセージバッファ管理ブロックテーブル */
Noinit(EXPORT QUEUE knl_free_mbfcb);	/* 未使用管理ブロックのキュー（FreeQue） */


/**
 * @brief メッセージバッファ管理ブロックの初期化
 *
 * すべての管理ブロックを未使用状態にして FreeQue に登録します。
 * カーネル起動時に一度だけ呼び出されます。
 *
 * @retval E_OK	正常終了
 * @retval E_SYS	メッセージバッファ数（NUM_MBFID）が 1 未満
 */
EXPORT ER knl_messagebuffer_initialize( void )
{
	MBFCB	*mbfcb, *end;

	/* システム情報の確認 */
	if ( NUM_MBFID < 1 ) {
		return E_SYS;
	}

	/* 全管理ブロックを FreeQue に登録 */
	QueInit(&knl_free_mbfcb);
	end = knl_mbfcb_table + NUM_MBFID;
	for ( mbfcb = knl_mbfcb_table; mbfcb < end; mbfcb++ ) {
		mbfcb->mbfid = 0;
		QueInsert(&mbfcb->send_queue, &knl_free_mbfcb);
	}

	return E_OK;
}

/* ------------------------------------------------------------------------ */

/**
 * @brief メッセージバッファへのメッセージ格納
 *
 * メッセージサイズを記録したヘッダに続けてメッセージ本体をリングバッファへ
 * 書き込み、tail と空き容量（frbufsz）を更新します。バッファ末尾に達した
 * 場合は先頭へ折り返します。
 *
 * @param mbfcb	対象メッセージバッファの管理ブロック
 * @param msg	格納するメッセージの先頭アドレス
 * @param msgsz	メッセージサイズ（バイト数）
 *
 * @note 空き容量の事前確認（knl_mbf_free）は呼び出し側の責任です。
 */
LOCAL void knl_msg_to_mbf( MBFCB *mbfcb, CONST void *msg, INT msgsz )
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
		knl_memcpy(&buffer[tail], msg, (SZ)remsz);
		msg = (VB*)msg + remsz;
		msgsz -= (INT)remsz;
		tail = 0;
	}
	knl_memcpy(&buffer[tail], msg, (SZ)msgsz);
	tail += (W)ROUNDSZ(msgsz);
	if ( tail >= mbfcb->bufsz ) {
		tail = 0;
	}

	mbfcb->tail = tail;
}

/* ------------------------------------------------------------------------ */

/**
 * @brief 送信待ちタスクのメッセージ受け入れと待ち解除
 *
 * 送信待ちキューの先頭タスクから順に、そのメッセージが格納できるだけの
 * 空きがある限りメッセージをバッファへ格納し、タスクの待ち状態を解除
 * します。空きが不足した時点で処理を打ち切ります。
 *
 * @param mbfcb	対象メッセージバッファの管理ブロック
 */
LOCAL void knl_mbf_wakeup( MBFCB *mbfcb )
{
	TCB	*top;
	INT	msgsz;

	while ( !isQueEmpty(&mbfcb->send_queue) ) {
		top = (TCB*)mbfcb->send_queue.next;
		msgsz = top->winfo.smbf.msgsz;
		if ( !knl_mbf_free(mbfcb, msgsz) ) {
			break;
		}

		/* 待ちタスクのメッセージを格納し、待ち解除 */
		knl_msg_to_mbf(mbfcb, top->winfo.smbf.msg, msgsz);
		knl_wait_release_ok(top);
	}
}


/**
 * @brief メッセージバッファの生成
 *
 * FreeQue から管理ブロックを取得して初期化し、メッセージバッファを
 * 生成します。バッファ領域は TA_USERBUF 指定時はユーザ提供の領域を、
 * それ以外はカーネルメモリ（knl_Imalloc）から確保します。
 * bufsz が 0 の場合はバッファなし（送受信タスク間の直接転送のみ）と
 * なります。
 *
 * @param pk_cmbf	メッセージバッファ生成情報へのポインタ
 *
 * @return 生成したメッセージバッファの ID（正値）、またはエラーコード
 * @retval E_RSATR	不正な属性（mbfatr）
 * @retval E_PAR	パラメータ不正（bufsz < 0、maxmsz <= 0、
 *			ユーザバッファサイズが sizeof(HEADER) の倍数でない等）
 * @retval E_NOMEM	バッファ領域のメモリ確保失敗
 * @retval E_LIMIT	メッセージバッファ数が上限（NUM_MBFID）を超過
 */
SYSCALL ID tk_cre_mbf( CONST T_CMBF *pk_cmbf )
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
	/* Imalloc なし構成では TA_USERBUF の指定が必須 */
	CHECK_PAR((pk_cmbf->mbfatr & TA_USERBUF) != 0);
#endif
	bufsz = (W)ROUNDSZ(pk_cmbf->bufsz);

	if ( bufsz > 0 ) {
#if USE_IMALLOC
		if ( (pk_cmbf->mbfatr & TA_USERBUF) != 0 ) {
			/* ユーザバッファのサイズは sizeof(HEADER) の倍数であること */
			if ( bufsz != pk_cmbf->bufsz ) {
				return E_PAR;
			}
			/* ユーザ提供バッファを使用 */
			msgbuf = (VB*) pk_cmbf->bufptr;
		} else {
			/* カーネルによるメモリ確保 */
			msgbuf = knl_Imalloc((UW)bufsz);
			if ( msgbuf == NULL ) {
				return E_NOMEM;
			}
		}
#else
		/* ユーザバッファのサイズは sizeof(HEADER) の倍数であること */
		if ( bufsz != pk_cmbf->bufsz ) {
			return E_PAR;
		}
		/* ユーザ提供バッファを使用 */
		msgbuf = (VB*) pk_cmbf->bufptr;
#endif
	} else {
		msgbuf = NULL;
	}

	BEGIN_CRITICAL_SECTION;
	/* FreeQue から管理ブロックを取得 */
	mbfcb = (MBFCB*)QueRemoveNext(&knl_free_mbfcb);
	if ( mbfcb == NULL ) {
		ercd = E_LIMIT;
	} else {
		mbfid = ID_MBF(mbfcb - knl_mbfcb_table);

		/* 管理ブロックの初期化 */
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
			knl_strncpy((char*)mbfcb->name, (char*)pk_cmbf->dsname,
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

#ifdef USE_FUNC_TK_DEL_MBF
/**
 * @brief メッセージバッファの削除
 *
 * 送信・受信の各待ちキューにつながれたタスクを E_DLT で待ち解除し、
 * 管理ブロックを FreeQue へ返却します。カーネルが確保したバッファ領域は
 * 解放します（TA_USERBUF 指定時はユーザ領域のため解放しません）。
 *
 * @param mbfid	削除するメッセージバッファの ID
 *
 * @retval E_OK	正常終了
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象メッセージバッファが未生成
 */
SYSCALL ER tk_del_mbf( ID mbfid )
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

		/* 待ちタスクの待ち解除（E_DLT） */
		knl_wait_delete(&mbfcb->recv_queue);
		knl_wait_delete(&mbfcb->send_queue);

		/* FreeQue へ返却 */
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

/**
 * @brief 送信待ちタスクの優先度変更時の処理
 *
 * 待ちキューを新しい優先度に従って並べ替えたうえで、キュー先頭の
 * タスクのメッセージが格納可能になっていれば送信を実行して待ち解除
 * します。
 *
 * @param tcb	優先度が変更されたタスクの TCB
 * @param oldpri	変更前の優先度（負値の場合は並べ替えを行わない）
 */
LOCAL void knl_mbf_chg_pri( TCB *tcb, INT oldpri )
{
	MBFCB	*mbfcb;

	mbfcb = get_mbfcb(tcb->wid);
	if ( oldpri >= 0 ) {
		/* 待ちキューの並べ替え */
		knl_gcb_change_priority((GCB*)mbfcb, tcb);
	}

	/* 送信待ちキューの新たな先頭タスクが送信可能なら
	   そのメッセージを送信 */
	knl_mbf_wakeup(mbfcb);
}

/**
 * @brief 送信待ちタスクの待ち解除時の処理
 *
 * 待ち解除により空いたキュー位置を踏まえ、後続の送信待ちタスクの
 * メッセージ格納を試みます。
 *
 * @param tcb	待ち解除されたタスクの TCB
 */
LOCAL void knl_mbf_rel_wai( TCB *tcb )
{
	knl_mbf_chg_pri(tcb, -1);
}

/*
 * メッセージバッファ送信待ちの待ち仕様定義
 */
LOCAL CONST WSPEC knl_wspec_smbf_tfifo = { TTW_SMBF, NULL,	knl_mbf_rel_wai };
LOCAL CONST WSPEC knl_wspec_smbf_tpri  = { TTW_SMBF, knl_mbf_chg_pri,	knl_mbf_rel_wai };

/**
 * @brief メッセージバッファへの送信
 *
 * 受信待ちタスクがあればそのタスクへ直接メッセージを渡して待ち解除
 * します。受信待ちタスクがなく、自タスクより先の送信待ちタスクが存在
 * せずバッファに空きがあれば、メッセージをバッファへ格納します。
 * どちらもできない場合、タスクは tmout に従い送信待ち状態になります。
 *
 * @param mbfid	送信先メッセージバッファの ID
 * @param msg	送信するメッセージの先頭アドレス
 * @param msgsz	メッセージサイズ（バイト数、1 以上）
 * @param tmout	タイムアウト時間（TMO_POL / TMO_FEVR 指定可）
 *
 * @retval E_OK	正常終了
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象メッセージバッファが未生成
 * @retval E_PAR	パラメータ不正（msgsz <= 0 または msgsz > maxmsz）
 * @retval E_TMOUT	タイムアウト（TMO_POL 指定時はポーリング失敗）
 * @retval E_RLWAI	待ち状態の強制解除
 * @retval E_DLT	待ちの間に対象メッセージバッファが削除された
 *
 * @note タスク独立部からは tmout = TMO_POL の場合のみ呼び出せます
 *	（ディスパッチ禁止中も同様）。
 */
SYSCALL ER tk_snd_mbf( ID mbfid, CONST void *msg, INT msgsz, TMO tmout )
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
		/* 受信待ちタスクへ直接送信 */
		tcb = (TCB*)mbfcb->recv_queue.next;
		knl_memcpy(tcb->winfo.rmbf.msg, msg, (SZ)msgsz);
		*tcb->winfo.rmbf.p_msgsz = msgsz;
		knl_wait_release_ok(tcb);

	} else if ( (in_indp() || knl_gcb_top_of_wait_queue((GCB*)mbfcb, knl_ctxtsk) == knl_ctxtsk)
		  &&(knl_mbf_free(mbfcb, msgsz)) ) {
		/* メッセージバッファへ格納 */
		knl_msg_to_mbf(mbfcb, msg, msgsz);

	} else {
		ercd = E_TMOUT;
		if ( tmout != TMO_POL ) {
			/* 送信待ち状態への移行準備 */
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


LOCAL CONST WSPEC knl_wspec_rmbf       = { TTW_RMBF, NULL,	NULL	    };

/**
 * @brief メッセージバッファからのメッセージ取り出し
 *
 * リングバッファ先頭（head）のヘッダからメッセージサイズを読み取り、
 * メッセージ本体を msg へコピーします。head と空き容量（frbufsz）を
 * 更新し、バッファ末尾に達した場合は先頭へ折り返します。
 *
 * @param mbfcb	対象メッセージバッファの管理ブロック
 * @param msg	取り出したメッセージの格納先アドレス
 *
 * @return 取り出したメッセージのサイズ（バイト数）
 *
 * @note バッファが空でないことの事前確認（knl_mbf_empty）は呼び出し側の
 *	責任です。
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
		knl_memcpy(msg, &buffer[head], (SZ)remsz);
		msg = (VB*)msg + remsz;
		msgsz -= (INT)remsz;
		head = 0;
	}
	knl_memcpy(msg, &buffer[head], (SZ)msgsz);
	head += (INT)ROUNDSZ(msgsz);
	if ( head >= mbfcb->bufsz ) {
		head = 0;
	}

	mbfcb->head = head;

	return actsz;
}

/**
 * @brief メッセージバッファからの受信
 *
 * バッファにメッセージがあれば先頭のメッセージを取り出します。
 * バッファが空でも送信待ちタスクがあれば、そのタスクから直接メッセージを
 * 受け取ります。いずれの場合も、受信により生じた空きへ送信待ちタスクの
 * メッセージ格納を試みます。メッセージがない場合、タスクは tmout に
 * 従い受信待ち状態になります。
 *
 * @param mbfid	受信元メッセージバッファの ID
 * @param msg	受信メッセージの格納先アドレス
 * @param tmout	タイムアウト時間（TMO_POL / TMO_FEVR 指定可）
 *
 * @return 受信したメッセージのサイズ（正値または 0）、またはエラーコード
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象メッセージバッファが未生成
 * @retval E_TMOUT	タイムアウト（TMO_POL 指定時はポーリング失敗）
 * @retval E_RLWAI	待ち状態の強制解除
 * @retval E_DLT	待ちの間に対象メッセージバッファが削除された
 *
 * @note タスク独立部およびディスパッチ禁止中は呼び出せません。
 */
SYSCALL INT tk_rcv_mbf( ID mbfid, void *msg, TMO tmout )
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
		/* メッセージバッファから読み出し */
		rcvsz = knl_mbf_to_msg(mbfcb, msg);

		/* 送信待ちタスクのメッセージを受け入れ */
		knl_mbf_wakeup(mbfcb);

	} else if ( !isQueEmpty(&mbfcb->send_queue) ) {
		/* 送信待ちタスクから直接受信 */
		tcb = (TCB*)mbfcb->send_queue.next;
		rcvsz = tcb->winfo.smbf.msgsz;
		knl_memcpy(msg, tcb->winfo.smbf.msg, (SZ)rcvsz);
		knl_wait_release_ok(tcb);
		knl_mbf_wakeup(mbfcb);
	} else {
		ercd = E_TMOUT;
		if ( tmout != TMO_POL ) {
			/* 受信待ち状態への移行準備 */
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

#ifdef USE_FUNC_TK_REF_MBF
/**
 * @brief メッセージバッファの状態参照
 *
 * 拡張情報、受信・送信待ちタスクの ID、次に受信されるメッセージの
 * サイズ、空きバッファサイズ、最大メッセージサイズを pk_rmbf へ
 * 返します。バッファが空で送信待ちタスクもない場合、msgsz は 0 に
 * なります。
 *
 * @param mbfid	参照するメッセージバッファの ID
 * @param pk_rmbf	状態情報の格納先アドレス
 *
 * @retval E_OK	正常終了
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象メッセージバッファが未生成
 */
SYSCALL ER tk_ref_mbf( ID mbfid, T_RMBF *pk_rmbf )
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
 *	デバッガサポート機能
 */
#if USE_DBGSPT

#if USE_OBJECT_NAME
/**
 * @brief 管理ブロックからのオブジェクト名取得
 *
 * TA_DSNAME 属性付きで生成されたメッセージバッファの名前への
 * ポインタを返します。
 *
 * @param id	対象メッセージバッファの ID
 * @param name	名前へのポインタの格納先アドレス
 *
 * @retval E_OK	正常終了
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象メッセージバッファが未生成
 * @retval E_OBJ	TA_DSNAME 属性が指定されていない
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

#ifdef USE_FUNC_TD_LST_MBF
/**
 * @brief メッセージバッファ ID の一覧取得
 *
 * 生成済みメッセージバッファの ID を list へ最大 nent 個格納します。
 *
 * @param list	ID リストの格納先配列
 * @param nent	list に格納可能な最大エントリ数
 *
 * @return 生成済みメッセージバッファの総数（nent を超える場合もその総数）
 */
SYSCALL INT td_lst_mbf( ID list[], INT nent )
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
/**
 * @brief メッセージバッファの状態参照（デバッガサポート）
 *
 * tk_ref_mbf と同等の状態情報を TD_RMBF 形式で返します。
 *
 * @param mbfid	参照するメッセージバッファの ID
 * @param pk_rmbf	状態情報の格納先アドレス
 *
 * @retval E_OK	正常終了
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象メッセージバッファが未生成
 */
SYSCALL ER td_ref_mbf( ID mbfid, TD_RMBF *pk_rmbf )
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
/**
 * @brief メッセージバッファ送信待ちキューの参照
 *
 * 送信待ちキューにつながれたタスクの ID を待ち順に list へ最大
 * nent 個格納します。
 *
 * @param mbfid	参照するメッセージバッファの ID
 * @param list	タスク ID リストの格納先配列
 * @param nent	list に格納可能な最大エントリ数
 *
 * @return 送信待ちタスクの総数（正値または 0）、またはエラーコード
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象メッセージバッファが未生成
 */
SYSCALL INT td_smbf_que( ID mbfid, ID list[], INT nent )
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
/**
 * @brief メッセージバッファ受信待ちキューの参照
 *
 * 受信待ちキューにつながれたタスクの ID を待ち順に list へ最大
 * nent 個格納します。
 *
 * @param mbfid	参照するメッセージバッファの ID
 * @param list	タスク ID リストの格納先配列
 * @param nent	list に格納可能な最大エントリ数
 *
 * @return 受信待ちタスクの総数（正値または 0）、またはエラーコード
 * @retval E_ID	不正な ID
 * @retval E_NOEXS	対象メッセージバッファが未生成
 */
SYSCALL INT td_rmbf_que( ID mbfid, ID list[], INT nent )
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
#endif /* USE_MESSAGEBUFFER */
