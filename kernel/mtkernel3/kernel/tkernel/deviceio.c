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
 * @file	deviceio.c
 * @brief	デバイス管理機能（入出力）
 *
 * デバイスのオープン・クローズ（tk_opn_dev, tk_cls_dev）、
 * 入出力要求の開始と完了待ち（tk_rea_dev, tk_wri_dev, tk_wai_dev や
 * 同期版の tk_srea_dev, tk_swri_dev）、サスペンド処理（tk_sus_dev）、
 * およびデバイス入出力関連の初期化・終了処理を提供します。
 */

#include "kernel.h"
#include "sysmgr.h"
#include "device.h"

#if USE_DEVICE

Noinit(EXPORT OpnCB	knl_OpnCBtbl[MAX_OPNDEV]);	/* オープン管理情報テーブル */
Noinit(EXPORT QUEUE	knl_FreeOpnCB);	/* 未使用キュー */

Noinit(EXPORT ReqCB	knl_ReqCBtbl[MAX_REQDEV]);	/* 要求管理情報テーブル */
Noinit(EXPORT QUEUE	knl_FreeReqCB);	/* 未使用キュー */

Noinit(EXPORT ResCB knl_resource_control_block);


/**
 * @brief	リソース管理情報の取得
 *
 * リソース管理情報を返します。スタートアップ関数が未実行で
 * オープンデバイス管理キューが未初期化の場合は、この時点で
 * 初期化します。
 *
 * @return	リソース管理情報へのポインタ（常に非 NULL）
 */
EXPORT ResCB* knl_GetResCB( void )
{
	LockDM();

	/* スタートアップ関数が呼ばれていなければ、この時点で初期化する */
	if ( knl_resource_control_block.openq.next == NULL ) {
		/* オープンデバイス管理キューの初期化 */
		QueInit(&(knl_resource_control_block.openq));
	}

	UnlockDM();

	return &knl_resource_control_block;
}

/**
 * @brief	デバイスディスクリプタの正当性検査
 *
 * デバイスディスクリプタ dd の範囲とオープン状態を検査し、
 * 対応するオープン管理情報を返します。mode が非 0 の場合は
 * オープンモードとのアクセス権も検査します。
 *
 * @param	dd	デバイスディスクリプタ
 * @param	mode	要求するアクセスモード（0 なら検査しない）
 * @param	p_opncb	オープン管理情報の返却先
 * @retval	E_OK	正常終了
 * @retval	E_ID	dd が範囲外、または未オープン
 * @retval	E_OACV	オープンモードがアクセス要求を許可していない
 */
EXPORT ER knl_check_devdesc( ID dd, UINT mode, OpnCB **p_opncb )
{
	OpnCB	*opncb;

	if ( dd < 1 || dd > MAX_OPNDEV ) {
		return E_ID;
	}
	opncb = OPNCB(dd);
	if ( opncb->resid == 0 ) {
		return E_ID;
	}

	if ( mode != 0 ) {
		if ( (opncb->omode & mode) == 0 ) {
			return E_OACV;
		}
	}

	*p_opncb = opncb;
	return E_OK;
}

/**
 * @brief	オープン管理ブロックの解放
 *
 * オープン管理ブロックをオープンキューとリソースキューから外します。
 * free が TRUE の場合は未使用キューへ戻します。
 *
 * @param	opncb	解放するオープン管理ブロック
 * @param	free	TRUE なら未使用キューへ返却する
 */
EXPORT void knl_delOpnCB( OpnCB *opncb, BOOL free )
{
	QueRemove(&opncb->q);
	QueRemove(&opncb->resq);

	if ( free ) {
		QueInsert(&opncb->q, &knl_FreeOpnCB);
	}
	opncb->resid = 0;
}

/**
 * @brief	要求管理ブロックの解放
 *
 * 要求管理ブロックを要求キューから外し、未使用キューへ戻します。
 *
 * @param	reqcb	解放する要求管理ブロック
 */
EXPORT void knl_delReqCB( ReqCB *reqcb )
{
	QueRemove(&reqcb->q);

	QueInsert(&reqcb->q, &knl_FreeReqCB);
	reqcb->opncb = NULL;
}

/* ------------------------------------------------------------------------ */

/**
 * @brief	デバイスのオープン状態検査
 *
 * 指定デバイスの指定サブユニットがオープンされていれば TRUE を
 * 返します。
 *
 * @param	devcb	デバイス登録情報
 * @param	unitno	サブユニット番号
 * @retval	TRUE	オープンされている
 * @retval	FALSE	オープンされていない
 */
EXPORT BOOL knl_chkopen( DevCB *devcb, INT unitno )
{
	QUEUE	*q;

	for ( q = devcb->openq.next; q != &devcb->openq; q = q->next ) {
		if ( ((OpnCB*)q)->unitno == unitno ) {
			return TRUE;
		}
	}
	return FALSE;
}


LOCAL CONST T_CSEM knl_pk_csem_DM = {
	NULL,
	TA_TFIFO | TA_FIRST,
	0,
	1,
};

/**
 * @brief	オープン管理ブロックの獲得
 *
 * 未使用キューからオープン管理ブロックを1つ取り出して初期化し、
 * デバイスのオープンキューとリソース管理情報のオープンキューへ
 * 登録します。
 *
 * @param	devcb	オープンするデバイスの登録情報
 * @param	unitno	サブユニット番号
 * @param	omode	オープンモード
 * @param	rescb	リソース管理情報
 * @return	獲得したオープン管理ブロック。空きがない場合は NULL。
 * @note	resid は 0（オープン処理未完了）のまま返します。
 */
LOCAL OpnCB* newOpnCB( DevCB *devcb, INT unitno, UINT omode, ResCB *rescb )
{
	OpnCB	*opncb;

	/* オープン管理ブロックの空きを獲得 */
	opncb = (OpnCB*)QueRemoveNext(&knl_FreeOpnCB);
	if ( opncb == NULL ) {
		return NULL; /* 空きなし */
	}

	/* オープンデバイスとして登録 */
	QueInsert(&opncb->q, &devcb->openq);
	QueInsert(&opncb->resq, &rescb->openq);

	opncb->devcb  = devcb;
	opncb->unitno = unitno;
	opncb->omode  = omode;
	QueInit(&opncb->requestq);
	opncb->waitone = 0;
	opncb->nwaireq = 0;
	opncb->abort_tskid = 0;

	opncb->resid  = 0; /* オープン処理が未完了であることを示す */

	return opncb;
}

/**
 * @brief	オープンモードの検査
 *
 * 現在のオープン状態と要求されたオープンモード omode を照合し、
 * 排他指定（TD_EXCL/TD_REXCL/TD_WEXCL）と競合しないか検査します。
 *
 * @param	devcb	デバイス登録情報
 * @param	unitno	サブユニット番号
 * @param	omode	要求されたオープンモード
 * @retval	E_OK	オープン可能
 * @retval	E_PAR	omode に読み書き（TD_UPDATE）の指定がない
 * @retval	E_BUSY	排他条件により現在オープンできない
 */
LOCAL ER chkopenmode( DevCB *devcb, INT unitno, UINT omode )
{
	QUEUE	*q;
	OpnCB	*opncb;
	INT	read, write, rexcl, wexcl;

	if ( (omode & TD_UPDATE) == 0 ) {
		return E_PAR;
	}

	/* 現在のオープン状態を検査 */
	read = write = rexcl = wexcl = 0;
	for ( q = devcb->openq.next; q != &devcb->openq; q = q->next ) {
		opncb = (OpnCB*)q;

		if ( unitno == 0 || opncb->unitno == 0 || opncb->unitno == unitno ) {
			if ( (opncb->omode & TD_READ)  != 0 ) {
				read++;
			}
			if ( (opncb->omode & TD_WRITE) != 0 ) {
				write++;
			}
			if ( (opncb->omode & (TD_EXCL|TD_REXCL)) != 0) {
				rexcl++;
			}
			if ( (opncb->omode & (TD_EXCL|TD_WEXCL)) != 0) {
				wexcl++;
			}
		}
	}

	/* オープン可能か？ */
	if ( (omode & (TD_EXCL|TD_REXCL)) != 0 && read  > 0 ) {
		return E_BUSY;
	}
	if ( (omode & (TD_EXCL|TD_WEXCL)) != 0 && write > 0 ) {
		return E_BUSY;
	}
	if ( (omode & TD_READ)  != 0 && rexcl > 0 ) {
		return E_BUSY;
	}
	if ( (omode & TD_WRITE) != 0 && wexcl > 0 ) {
		return E_BUSY;
	}

	return E_OK;
}

/**
 * @brief	デバイスのオープン
 *
 * 論理デバイス名 devnm のデバイスをオープンモード omode で
 * オープンし、デバイスディスクリプタを返します。必要に応じて
 * ドライバのオープン関数（openfn）を呼び出します。
 *
 * @param	devnm	論理デバイス名
 * @param	omode	オープンモード
 * @return	正の値ならデバイスディスクリプタ。負の値ならエラーコード。
 * @retval	E_CTX	リソース管理情報が取得できない
 * @retval	E_NOEXS	デバイスが未登録、またはサブユニット番号が範囲外
 * @retval	E_PAR	omode が不正
 * @retval	E_BUSY	排他条件により現在オープンできない
 * @retval	E_LIMIT	同時オープン数（MAX_OPNDEV）の上限超過
 * @retval	E_SYS	アボート完了確認用セマフォの生成に失敗
 * @note	同一サブユニットが既にオープン済みで、ドライバ属性に
 *		TDA_OPENREQ が指定されていない場合、openfn は
 *		呼び出しません。
 */
SYSCALL ID tk_opn_dev( CONST UB *devnm, UINT omode )
{
	OPNFN	openfn;
	void	*exinf;
	UB	pdevnm[L_DEVNM + 1];
	INT	unitno;
	ResCB	*rescb;
	DevCB	*devcb;
	OpnCB	*opncb;
	ER	ercd;
	ID	semid;

	unitno = knl_phydevnm(pdevnm, devnm);

	/* リソース管理情報の取得 */
	rescb = knl_GetResCB();
	if ( rescb == NULL ) {
		ercd = E_CTX;
		goto err_ret1;
	}

	LockDM();

	/* オープンするデバイスを検索 */
	devcb = knl_searchDevCB(pdevnm);
	if ( devcb == NULL || unitno > devcb->ddev.nsub ) {
		ercd = E_NOEXS;
		goto err_ret2;
	}

	/* オープンモードの検査 */
	ercd = chkopenmode(devcb, unitno, omode);
	if ( ercd < E_OK ) {
		goto err_ret2;
	}

	openfn = (OPNFN)devcb->ddev.openfn;
	exinf = devcb->ddev.exinf;

	/* デバイスドライバ呼び出しが必要か？ */
	if ( knl_chkopen(devcb, unitno) && (devcb->ddev.drvatr & TDA_OPENREQ) == 0 ) {
		openfn = NULL;
	}

	/* オープン管理ブロックの獲得 */
	opncb = newOpnCB(devcb, unitno, omode, rescb);
	if ( opncb == NULL ) {
		ercd = E_LIMIT;
		goto err_ret2;
	}

	semid = tk_cre_sem(&knl_pk_csem_DM);
	if ( semid < E_OK ) {
		ercd = E_SYS;
		goto err_ret2_5;
	}
	opncb->abort_semid = semid;

	UnlockDM();

	if ( openfn != NULL ) {
		/* デバイスドライバ呼び出し */
		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode++;
		ENABLE_INTERRUPT;
		ercd = (*openfn)(DEVID(devcb, unitno), omode, exinf);
		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode--;
		ENABLE_INTERRUPT;

		if ( ercd < E_OK ) {
			goto err_ret3;
		}
	}

	LockDM();
	opncb->resid = 1; /* オープン処理が完了したことを示す */
	UnlockDM();

	return DD(opncb);

err_ret3:
	LockDM();
	tk_del_sem(opncb->abort_semid);
err_ret2_5:
	knl_delOpnCB(opncb, TRUE);
err_ret2:
	UnlockDM();
err_ret1:
	return ercd;
}

/**
 * @brief	全要求のアボート
 *
 * 指定したオープン管理ブロックに対する処理中・待ち中の全要求を
 * ドライバのアボート関数（abortfn）でアボートし、その完了を
 * 待った後、残った要求をドライバの完了待ち関数（waitfn）で
 * 完了させて登録解除します。
 *
 * @param	opncb	対象のオープン管理ブロック
 */
LOCAL void abort_allrequest( OpnCB *opncb )
{
	ABTFN	abortfn;
	WAIFN	waitfn;
	void	*exinf;
	DevCB	*devcb;
	ReqCB	*reqcb;
	QUEUE	*q;

	/* 'execfn' や 'waitfn' の呼び出し中であれば、アボート要求を実行する */
	LockDM();

	devcb = opncb->devcb;
	abortfn = (ABTFN)devcb->ddev.abortfn;
	waitfn  = (WAIFN)devcb->ddev.waitfn;
	exinf   = devcb->ddev.exinf;

	opncb->abort_tskid = tk_get_tid();
	opncb->abort_cnt = 0;

	if ( opncb->nwaireq > 0 ) {
		/* 複数要求の完了待ち中 */
		reqcb = DEVREQ_REQCB(opncb->waireqlst);

		/* デバイスドライバ呼び出し */
		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode++;
		ENABLE_INTERRUPT;
		(*abortfn)(reqcb->tskid, opncb->waireqlst, opncb->nwaireq, exinf);
		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode--;
		ENABLE_INTERRUPT;

		opncb->abort_cnt++;
	} else {
		/* 要求の開始処理中、または単一要求の完了待ち中 */
		for ( q = opncb->requestq.next; q != &opncb->requestq; q = q->next ) {
			reqcb = (ReqCB*)q;
			if ( reqcb->tskid == 0 ) {
				continue;
			}

			reqcb->req.abort = TRUE;

			/* デバイスドライバ呼び出し */
			DISABLE_INTERRUPT;
			knl_ctxtsk->sysmode++;
			ENABLE_INTERRUPT;
			(*abortfn)(reqcb->tskid, &reqcb->req, 1, exinf);
			DISABLE_INTERRUPT;
			knl_ctxtsk->sysmode--;
			ENABLE_INTERRUPT;

			opncb->abort_cnt++;
		}
	}

	UnlockDM();

	if ( opncb->abort_cnt > 0 ) {
		/* アボート要求処理の完了を待つ */
		tk_wai_sem(opncb->abort_semid, 1, TMO_FEVR);
	}
	opncb->abort_tskid = 0;

	/* 残っている要求をアボートし、完了を待つ */
	LockDM();
	while ( !isQueEmpty(&opncb->requestq) ) {
		reqcb = (ReqCB*)opncb->requestq.next;
		reqcb->req.abort = TRUE;

		UnlockDM();

		/* デバイスドライバ呼び出し */
		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode++;
		ENABLE_INTERRUPT;
		(*waitfn)(&reqcb->req, 1, TMO_FEVR, exinf);
		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode--;
		ENABLE_INTERRUPT;

		LockDM();

		/* 完了した要求の登録解除 */
		knl_delReqCB(reqcb);
	}
	UnlockDM();
}

/**
 * @brief	デバイスクローズ処理
 *
 * 処理中の全要求をアボートした後、アボート完了確認用セマフォの削除、
 * オープン管理ブロックの解放を行い、必要に応じてドライバの
 * クローズ関数（closefn）を呼び出します。
 *
 * @param	opncb	クローズするオープン管理ブロック
 * @param	option	クローズオプション（TD_EJECT など）
 * @return	ドライバのクローズ関数の返値。呼び出し不要の場合は E_OK。
 * @note	同一サブユニットが他でオープン中の場合、TD_EJECT は
 *		無効化され、ドライバ属性に TDA_OPENREQ がなければ
 *		closefn は呼び出しません。
 */
EXPORT ER knl_close_device( OpnCB *opncb, UINT option )
{
	CLSFN	closefn;
	void	*exinf;
	ID	devid;
	DevCB	*devcb;
	INT	unitno;
	ER	ercd = E_OK;

	/* 処理中の全要求をアボート */
	abort_allrequest(opncb);

	LockDM();

	devcb  = opncb->devcb;
	unitno = opncb->unitno;
	closefn = (CLSFN)devcb->ddev.closefn;
	exinf = devcb->ddev.exinf;
	devid = DEVID(devcb, unitno);

	/* アボート完了確認用セマフォの削除 */
	tk_del_sem(opncb->abort_semid);

	/* オープン管理ブロックの解放 */
	knl_delOpnCB(opncb, FALSE);

	/* デバイスドライバ呼び出しが必要か？ */
	if ( knl_chkopen(devcb, unitno) ) {
		option &= ~TD_EJECT;
		if ( (devcb->ddev.drvatr & TDA_OPENREQ) == 0 ) {
			closefn = NULL;
		}
	}

	UnlockDM();

	if ( closefn != NULL ) {
		/* デバイスドライバ呼び出し */
		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode++;
		ENABLE_INTERRUPT;
		ercd = (*closefn)(devid, option, exinf);
		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode--;
		ENABLE_INTERRUPT;
	}

	LockDM();
	/* オープン管理ブロックを未使用キューへ返却 */
	QueInsert(&opncb->q, &knl_FreeOpnCB);
	UnlockDM();

	return ercd;
}

/**
 * @brief	デバイスのクローズ
 *
 * デバイスディスクリプタ dd で指定したデバイスをクローズします。
 *
 * @param	dd	デバイスディスクリプタ
 * @param	option	クローズオプション（TD_EJECT など）
 * @retval	E_OK	正常終了
 * @retval	E_ID	dd が不正
 * @return	その他、ドライバのクローズ関数の返値
 */
SYSCALL ER tk_cls_dev( ID dd, UINT option )
{
	OpnCB	*opncb;
	ER	ercd;

	LockDM();

	ercd = knl_check_devdesc(dd, 0, &opncb);
	if ( ercd < E_OK ) {
		UnlockDM();
		goto err_ret;
	}

	opncb->resid = 0; /* クローズ処理中であることを示す */

	UnlockDM();

	/* デバイスクローズ処理 */
	ercd = knl_close_device(opncb, option);

err_ret:
	return ercd;
}

/* ------------------------------------------------------------------------ */

/**
 * @brief	要求管理ブロックの獲得
 *
 * 未使用キューから要求管理ブロックを1つ取り出し、指定した
 * オープン管理ブロックの要求キューへ登録します。
 *
 * @param	opncb	要求元のオープン管理ブロック
 * @return	獲得した要求管理ブロック。空きがない場合は NULL。
 */
LOCAL ReqCB* newReqCB( OpnCB *opncb )
{
	ReqCB	*reqcb;

	/* 要求管理ブロックの空きを獲得 */
	reqcb = (ReqCB*)QueRemoveNext(&knl_FreeReqCB);
	if ( reqcb == NULL ) {
		return NULL; /* 空きなし */
	}

	/* 要求先のオープンデバイスとして登録 */
	QueInsert(&reqcb->q, &opncb->requestq);

	reqcb->opncb = opncb;

	return reqcb;
}

/**
 * @brief	デバイスへの入出力開始要求
 *
 * 要求パケットを作成してドライバの処理開始関数（execfn）を
 * 呼び出し、入出力を開始します。tk_rea_dev / tk_wri_dev の
 * 共通処理です。
 *
 * @param	dd	デバイスディスクリプタ
 * @param	start	開始位置（負値は属性データの指定）
 * @param	buf	入出力バッファ
 * @param	size	入出力サイズ
 * @param	tmout	要求受け付けのタイムアウト時間
 * @param	cmd	要求コマンド（TDC_READ または TDC_WRITE）
 * @return	正の値なら要求ID。負の値ならエラーコード。
 * @retval	E_ID	dd が不正
 * @retval	E_OACV	オープンモードがアクセス要求を許可していない
 * @retval	E_LIMIT	同時要求数（MAX_REQDEV）の上限超過
 * @return	その他、ドライバの処理開始関数の返値
 * @note	start が -0x7fffffff～-0x00010000 の範囲の場合は
 *		オープンモードの検査を行いません。
 */
EXPORT ID knl_request( ID dd, W start, void *buf, W size, TMO tmout, INT cmd )
{
	EXCFN	execfn;
	void	*exinf;
	OpnCB	*opncb;
	DevCB	*devcb;
	ReqCB	*reqcb;
	UINT	m;
	ER	ercd;

	LockDM();

	if ( start <= -0x00010000 && start >= -0x7fffffff ) {
		m = 0; /* オープンモードを無視 */
	} else {
		m = ( cmd == TDC_READ )? TD_READ: TD_WRITE;
	}
	ercd = knl_check_devdesc(dd, m, &opncb);
	if ( ercd < E_OK ) {
		goto err_ret1;
	}

	devcb = opncb->devcb;
	execfn = (EXCFN)devcb->ddev.execfn;
	exinf = devcb->ddev.exinf;

	/* 要求管理ブロックの獲得 */
	reqcb = newReqCB(opncb);
	if ( reqcb == NULL ) {
		ercd = E_LIMIT;
		goto err_ret1;
	}

	/* 要求パケットの設定 */
	reqcb->req.next   = NULL;
	reqcb->req.exinf  = NULL;
	reqcb->req.devid  = DEVID(devcb, opncb->unitno);
	reqcb->req.cmd    = cmd;
	reqcb->req.abort  = FALSE;
	reqcb->req.start  = start;
	reqcb->req.size   = size;
	reqcb->req.buf    = buf;
	reqcb->req.asize  = 0;
	reqcb->req.error  = 0;

	/* 処理中であることを示す */
	reqcb->tskid = tk_get_tid();

	UnlockDM();

	/* デバイスドライバ呼び出し */
	DISABLE_INTERRUPT;
	knl_ctxtsk->sysmode++;
	ENABLE_INTERRUPT;
	ercd = (*execfn)(&reqcb->req, tmout, exinf);
	DISABLE_INTERRUPT;
	knl_ctxtsk->sysmode--;
	ENABLE_INTERRUPT;

	LockDM();

	/* 処理中でないことを示す */
	reqcb->tskid = 0;

	/* アボート完了待ちタスクがあれば、
	   アボート完了を通知する */
	if ( opncb->abort_tskid > 0 && --opncb->abort_cnt == 0 ) {
		tk_sig_sem(opncb->abort_semid, 1);
	}

	if ( ercd < E_OK ) {
		goto err_ret2;
	}

	UnlockDM();

	return REQID(reqcb);

err_ret2:
	knl_delReqCB(reqcb);
err_ret1:
	UnlockDM();
	return ercd;
}

/**
 * @brief	デバイスからの読み込み開始
 *
 * デバイスへ読み込み要求を発行し、要求IDを返します。読み込みの
 * 完了は tk_wai_dev で待ちます。
 *
 * @param	dd	デバイスディスクリプタ
 * @param	start	読み込み開始位置（負値は属性データの指定）
 * @param	buf	読み込みバッファ
 * @param	size	読み込みサイズ
 * @param	tmout	要求受け付けのタイムアウト時間
 * @return	正の値なら要求ID。負の値ならエラーコード。
 */
SYSCALL ID tk_rea_dev( ID dd, W start, void *buf, SZ size, TMO tmout )
{
	ER	ercd;

	ercd = knl_request(dd, start, buf, size, tmout, TDC_READ);

	return ercd;
}

/**
 * @brief	デバイスからの同期読み込み
 *
 * tk_rea_dev で読み込みを開始し、tk_wai_dev で完了を待ちます。
 *
 * @param	dd	デバイスディスクリプタ
 * @param	start	読み込み開始位置（負値は属性データの指定）
 * @param	buf	読み込みバッファ
 * @param	size	読み込みサイズ
 * @param	asize	実際に読み込んだサイズの返却先
 * @retval	E_OK	正常終了
 * @return	負の値なら要求発行・完了待ち・入出力のエラーコード
 */
SYSCALL ER tk_srea_dev( ID dd, W start, void *buf, SZ size, SZ *asize )
{
	ER	ercd, ioercd;

	ercd = tk_rea_dev(dd, start, buf, size, TMO_FEVR);
	if ( ercd < E_OK ) {
		goto err_ret;
	}

	ercd = tk_wai_dev(dd, ercd, asize, &ioercd, TMO_FEVR);
	if ( ercd < E_OK ) {
		goto err_ret;
	}

	return ioercd;

err_ret:
	return ercd;
}

/**
 * @brief	デバイスへの書き込み開始
 *
 * デバイスへ書き込み要求を発行し、要求IDを返します。書き込みの
 * 完了は tk_wai_dev で待ちます。
 *
 * @param	dd	デバイスディスクリプタ
 * @param	start	書き込み開始位置（負値は属性データの指定）
 * @param	buf	書き込みデータのバッファ
 * @param	size	書き込みサイズ
 * @param	tmout	要求受け付けのタイムアウト時間
 * @return	正の値なら要求ID。負の値ならエラーコード。
 */
SYSCALL ID tk_wri_dev( ID dd, W start, CONST void *buf, SZ size, TMO tmout )
{
	ER	ercd;

	ercd = knl_request(dd, start, (void *)buf, size, tmout, TDC_WRITE);

	return ercd;
}

/**
 * @brief	デバイスへの同期書き込み
 *
 * tk_wri_dev で書き込みを開始し、tk_wai_dev で完了を待ちます。
 *
 * @param	dd	デバイスディスクリプタ
 * @param	start	書き込み開始位置（負値は属性データの指定）
 * @param	buf	書き込みデータのバッファ
 * @param	size	書き込みサイズ
 * @param	asize	実際に書き込んだサイズの返却先
 * @retval	E_OK	正常終了
 * @return	負の値なら要求発行・完了待ち・入出力のエラーコード
 */
SYSCALL ER tk_swri_dev( ID dd, W start, CONST void *buf, SZ size, SZ *asize )
{
	ER	ercd, ioercd;

	ercd = tk_wri_dev(dd, start, buf, size, TMO_FEVR);
	if ( ercd < E_OK ) {
		goto err_ret;
	}

	ercd = tk_wai_dev(dd, ercd, asize, &ioercd, TMO_FEVR);
	if ( ercd < E_OK ) {
		goto err_ret;
	}

	return ioercd;

err_ret:
	return ercd;
}

/**
 * @brief	要求IDの正当性検査
 *
 * 要求ID reqid の範囲を検査し、その要求が指定したオープン管理
 * ブロックに属することを確認して要求管理ブロックを返します。
 *
 * @param	reqid	要求ID
 * @param	opncb	要求が属するべきオープン管理ブロック
 * @return	要求管理ブロック。不正な場合は NULL。
 */
LOCAL ReqCB* knl_check_reqid( ID reqid, OpnCB *opncb )
{
	ReqCB	*reqcb;

	if ( reqid < 1 || reqid > MAX_REQDEV ) {
		return NULL;
	}
	reqcb = REQCB(reqid);
	if ( reqcb->opncb != opncb ) {
		return NULL;
	}

	return reqcb;
}

/**
 * @brief	要求完了待ち
 *
 * デバイスディスクリプタ dd に対する入出力要求の完了を待ちます。
 * reqid に要求IDを指定するとその要求の完了を、0 を指定すると
 * いずれかの要求の完了を待ちます。完了待ちはドライバの
 * 完了待ち関数（waitfn）で行います。
 *
 * @param	dd	デバイスディスクリプタ
 * @param	reqid	要求ID（0 ならいずれかの要求の完了を待つ）
 * @param	asize	実際に入出力したサイズの返却先
 * @param	ioer	入出力エラーコードの返却先
 * @param	tmout	タイムアウト時間
 * @return	正の値なら完了した要求のID。負の値ならエラーコード。
 * @retval	E_ID	dd または reqid が不正
 * @retval	E_OBJ	既に他タスクが完了待ち中、または要求が処理中
 * @retval	E_NOEXS	reqid が 0 で、完了を待つ要求が存在しない
 * @retval	E_SYS	ドライバが不正な要求番号を返した
 */
SYSCALL ID tk_wai_dev( ID dd, ID reqid, SZ *asize, ER *ioer, TMO tmout )
{
	WAIFN	waitfn;
	void	*exinf;
	OpnCB	*opncb;
	DevCB	*devcb;
	ReqCB	*reqcb;
	T_DEVREQ *devreq;
	INT	reqno, nreq;
	ID	tskid;
	ER	ercd;

	tskid = tk_get_tid();

	LockDM();

	ercd = knl_check_devdesc(dd, 0, &opncb);
	if ( ercd < E_OK ) {
		goto err_ret2;
	}

	devcb = opncb->devcb;
	waitfn = (WAIFN)devcb->ddev.waitfn;
	exinf = devcb->ddev.exinf;

	if ( reqid == 0 ) {
		/* 'dd' に対するいずれかの要求の完了を待つ場合 */
		if ( opncb->nwaireq > 0 || opncb->waitone > 0 ) {
			ercd = E_OBJ;
			goto err_ret2;
		}
		if ( isQueEmpty(&opncb->requestq) ) {
			ercd = E_NOEXS;
			goto err_ret2;
		}

		/* 完了待ち要求リストの作成 */
		reqcb = (ReqCB*)opncb->requestq.next;
		for ( nreq = 1;; nreq++ ) {
			reqcb->tskid = tskid;
			devreq = &reqcb->req;
			reqcb = (ReqCB*)reqcb->q.next;
			if ( reqcb == (ReqCB*)&opncb->requestq ) {
				break;
			}
			devreq->next = &reqcb->req;
		}
		devreq->next = NULL;
		devreq = &((ReqCB*)opncb->requestq.next)->req;

		opncb->waireqlst = devreq;
		opncb->nwaireq = nreq;
	} else {
		/* 指定した要求の完了を待つ場合 */
		reqcb = knl_check_reqid(reqid, opncb);
		if ( reqcb == NULL ) {
			ercd = E_ID;
			goto err_ret2;
		}
		if ( opncb->nwaireq > 0 || reqcb->tskid > 0 ) {
			ercd = E_OBJ;
			goto err_ret2;
		}

		/* 完了待ち要求リストの作成 */
		reqcb->tskid = tskid;
		devreq = &reqcb->req;
		devreq->next = NULL;
		nreq = 1;

		opncb->waitone++;
	}

	UnlockDM();

	/* デバイスドライバ呼び出し */
	DISABLE_INTERRUPT;
	knl_ctxtsk->sysmode++;
	ENABLE_INTERRUPT;
	reqno = (*waitfn)(devreq, nreq, tmout, exinf);
	DISABLE_INTERRUPT;
	knl_ctxtsk->sysmode--;
	ENABLE_INTERRUPT;

	if ( reqno <  E_OK ) {
		ercd = reqno;
	}
	if ( reqno >= nreq ) {
		ercd = E_SYS;
	}

	LockDM();

	/* 完了待ち状態の解除処理 */
	if ( reqid == 0 ) {
		opncb->nwaireq = 0;
	} else {
		opncb->waitone--;
	}

	/* アボート完了待ちタスクがあれば、
	   アボート完了を通知する */
	if ( opncb->abort_tskid > 0 && --opncb->abort_cnt == 0 ) {
		tk_sig_sem(opncb->abort_semid, 1);
	}

	/* 処理結果の取得 */
	while ( devreq != NULL ) {
		reqcb = DEVREQ_REQCB(devreq);
		if ( reqno-- == 0 ) {
			reqid = REQID(reqcb);
			*asize = devreq->asize;
			*ioer  = devreq->error;
		}
		reqcb->tskid = 0;
		devreq = devreq->next;
	}

	if ( ercd < E_OK ) {
		goto err_ret2;
	}

	/* 完了した要求の登録解除 */
	knl_delReqCB(REQCB(reqid));

	UnlockDM();

	return reqid;

err_ret2:
	UnlockDM();
	return ercd;
}

/* ------------------------------------------------------------------------ */

/* サスペンド禁止要求カウント */
EXPORT INT	knl_DisSusCnt = 0;

/**
 * @brief	全デバイスへのドライバ要求イベント送信
 *
 * 登録されている全デバイスのうち、disk の指定（ディスクデバイスか
 * 否か）に一致するデバイスのイベント処理関数（eventfn）を順に
 * 呼び出します。
 *
 * @param	evttyp	イベントタイプ（TDV_SUSPEND / TDV_RESUME）
 * @param	disk	TRUE ならディスクデバイスのみ、FALSE なら
 *			ディスク以外のデバイスのみを対象とする
 * @return	最後に呼び出したイベント処理関数の返値
 *		（対象がない場合は E_OK）
 */
LOCAL ER sendevt_alldevice( INT evttyp, BOOL disk )
{
	EVTFN	eventfn;
	QUEUE	*q;
	DevCB	*devcb;
	BOOL	d;
	ER	ercd = E_OK;

	for ( q = knl_UsedDevCB.next; q != &knl_UsedDevCB; q = q->next ) {
		devcb = (DevCB*)q;

		d = ( (devcb->ddev.devatr & TD_DEVTYPE) == TDK_DISK )?
							TRUE: FALSE;
		if ( disk != d ) {
			continue;
		}

		/* デバイスドライバ呼び出し */
		eventfn = (EVTFN)devcb->ddev.eventfn;
		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode++;
		ENABLE_INTERRUPT;
		ercd = (*eventfn)(evttyp, NULL, devcb->ddev.exinf);
		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode--;
		ENABLE_INTERRUPT;
	}

	return ercd;
}

/**
 * @brief	サスペンドの実行
 *
 * ディスク以外のデバイス、ディスクデバイスの順に TDV_SUSPEND
 * イベントを送信し、復帰後は逆順に TDV_RESUME イベントを
 * 送信します。サスペンド状態への遷移コードは実装位置のみ
 * 用意されています。
 *
 * @return	最後に送信したイベントの返値
 */
LOCAL ER do_suspend( void )
{
	ER	ercd;

	/* デバイスの登録・登録解除の受け付けを停止 */
	LockREG();

	/* ディスク以外のデバイスのサスペンド処理 */
	ercd = sendevt_alldevice(TDV_SUSPEND, FALSE);

	/* ディスクデバイスのサスペンド処理 */
	ercd = sendevt_alldevice(TDV_SUSPEND, TRUE);

	/* 新規要求の受け付けを停止 */
	LockDM();

	/*
	 * サスペンド状態へ遷移するコードをここに挿入する
	 */

	/*
	 * サスペンド状態からの復帰時に実行するコードをここに挿入する
	 */


	/* 要求の受け付けを再開 */
	UnlockDM();

	/* ディスクデバイスのレジューム処理 */
	ercd = sendevt_alldevice(TDV_RESUME, TRUE);

	/* ディスク以外のデバイスのレジューム処理 */
	ercd = sendevt_alldevice(TDV_RESUME, FALSE);

	/* デバイスの登録・登録解除の受け付けを再開 */
	UnlockREG();

	return ercd;
}

/**
 * @brief	サスペンド処理
 *
 * mode の指定に従い、サスペンドの実行（TD_SUSPEND）、サスペンドの
 * 禁止（TD_DISSUS）・許可（TD_ENASUS）、禁止要求カウントの取得
 * （TD_CHECK）を行います。
 *
 * @param	mode	動作モード（TD_FORCE 指定で強制サスペンド）
 * @return	正常終了時はサスペンド禁止要求カウント。
 *		負の値ならエラーコード。
 * @retval	E_CTX	リソース管理情報が取得できない
 * @retval	E_BUSY	サスペンド禁止中（TD_FORCE 指定なし）
 * @retval	E_QOVR	禁止要求カウントの上限（MAX_DISSUS）超過
 * @retval	E_PAR	mode が不正
 */
SYSCALL INT tk_sus_dev( UINT mode )
{
	ResCB	*rescb;
	BOOL	suspend = FALSE;
	ER	ercd;

	/* リソース管理情報の取得 */
	rescb = knl_GetResCB();
	if ( rescb == NULL ) {
		ercd = E_CTX;
		goto err_ret1;
	}

	LockDM();

	switch ( mode & 0xf ) {
	  case TD_SUSPEND:	/* サスペンド */
		if ( knl_DisSusCnt > 0 && (mode & TD_FORCE) == 0 ) {
			ercd = E_BUSY;
			goto err_ret2;
		}
		suspend = TRUE;
		break;

	  case TD_DISSUS:	/* サスペンド禁止 */
		if ( knl_DisSusCnt >= MAX_DISSUS ) {
			ercd = E_QOVR;
			goto err_ret2;
		}
		knl_DisSusCnt++;
		rescb->dissus++;
		break;
	  case TD_ENASUS:	/* サスペンド許可 */
		if ( rescb->dissus > 0 ) {
			rescb->dissus--;
			knl_DisSusCnt--;
		}
		break;

	  case TD_CHECK:	/* サスペンド禁止要求カウントの取得 */
		break;

	  default:
		ercd = E_PAR;
		goto err_ret2;
	}

	UnlockDM();

	if ( suspend ) {
		/* サスペンド */
		ercd = do_suspend();
		if ( ercd < E_OK ) {
			goto err_ret1;
		}
	}

	return knl_DisSusCnt;

err_ret2:
	UnlockDM();
err_ret1:
	return ercd;
}

/* ------------------------------------------------------------------------ */

/**
 * @brief	デバイス管理スタートアップ処理
 *
 * リソース管理情報のオープンデバイス管理キューと
 * サスペンド禁止要求カウントを初期化します。
 */
EXPORT void knl_devmgr_startup( void )
{
	LockDM();

	/* オープンデバイス管理キューの初期化 */
	QueInit(&(knl_resource_control_block.openq));
	knl_resource_control_block.dissus = 0;
	
	UnlockDM();

	return;
}

/**
 * @brief	デバイス管理クリーンアップ処理
 *
 * サスペンド禁止要求を解除し、オープン中の全デバイスを
 * クローズします。デバイス管理機能が一度も使用されていない
 * 場合は何もしません。
 */
EXPORT void knl_devmgr_cleanup( void )
{
	OpnCB	*opncb;

	/* 一度も使用されていない場合は何もしない */
	if ( knl_resource_control_block.openq.next == NULL ) {
		return;
	}

	LockDM();

	/* サスペンド禁止要求の解除 */
	knl_DisSusCnt -= knl_resource_control_block.dissus;
	knl_resource_control_block.dissus = 0;

	/* オープン中の全デバイスをクローズ */
	while ( !isQueEmpty(&(knl_resource_control_block.openq)) ) {
		opncb = RESQ_OPNCB(knl_resource_control_block.openq.next);

		/* クローズ処理中であることを示す */
		opncb->resid = 0;

		UnlockDM();

		/* デバイスクローズ処理 */
		knl_close_device(opncb, 0);

		LockDM();
	}
	UnlockDM();

	return;
}

/**
 * @brief	デバイス入出力関連の初期化
 *
 * オープン管理情報テーブルと要求管理情報テーブルの全エントリを
 * 未使用キューへつなぎます。
 *
 * @retval	E_OK	常に正常終了
 */
EXPORT ER knl_initDevIO( void )
{
	INT	i;

	QueInit(&knl_FreeOpnCB);
	for ( i = 0; i < MAX_OPNDEV; ++i ) {
		knl_OpnCBtbl[i].resid = 0;
		QueInsert(&knl_OpnCBtbl[i].q, &knl_FreeOpnCB);
	}

	QueInit(&knl_FreeReqCB);
	for ( i = 0; i < MAX_REQDEV; ++i ) {
		knl_ReqCBtbl[i].opncb = NULL;
		QueInsert(&knl_ReqCBtbl[i].q, &knl_FreeReqCB);
	}

	return E_OK;
}

/**
 * @brief	デバイス入出力関連の終了処理
 *
 * 現状は何も行いません。
 *
 * @retval	E_OK	常に正常終了
 */
EXPORT ER knl_finishDevIO( void )
{
	return E_OK;
}

#endif /* USE_DEVICE */
