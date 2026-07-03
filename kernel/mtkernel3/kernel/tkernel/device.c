/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.02
 *
 *    Copyright (C) 2006-2020 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2020/10/21.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	device.c
 * @brief	デバイス管理機能（登録管理）
 *
 * デバイスの登録・登録解除（tk_def_dev）、デバイス名・デバイス情報の
 * 取得（tk_get_dev, tk_ref_dev, tk_oref_dev, tk_lst_dev）、ドライバへの
 * 要求イベント送信（tk_evt_dev）、およびデバイス管理機能全体の
 * 初期化・終了処理を提供します。
 */

#include "kernel.h"
#include "sysmgr.h"
#include "device.h"

#if USE_DEVICE

/* デバイス管理排他制御用ロック */
Noinit(EXPORT	FastMLock	knl_DevMgrLock);

/* デバイス初期設定情報 */
Noinit(EXPORT	T_IDEV		knl_DefaultIDev);

/* ------------------------------------------------------------------------ */
/*
 *	デバイス登録管理
 */

Noinit(EXPORT	DevCB		knl_DevCBtbl[MAX_REGDEV]);	/* デバイス登録情報テーブル */
Noinit(EXPORT	QUEUE		knl_UsedDevCB);	/* 使用中キュー */
Noinit(EXPORT	QUEUE		knl_FreeDevCB);	/* 未使用キュー */


/**
 * @brief	登録デバイスの検索
 *
 * 使用中キューから物理デバイス名 devnm に一致するデバイス登録情報を
 * 検索します。
 *
 * @param	devnm	物理デバイス名
 * @return	見つかったデバイス登録情報。未登録の場合は NULL。
 * @note	呼び出し元でデバイス管理ロック（LockDM）を獲得しておく
 *		必要があります。
 */
EXPORT DevCB* knl_searchDevCB( CONST UB *devnm )
{
	QUEUE	*q;
	DevCB	*devcb;

	for ( q = knl_UsedDevCB.next; q != &knl_UsedDevCB; q = q->next ) {
		devcb = (DevCB*)q;

		if ( devcb->devnm[0] == devnm[0] && knl_strcmp((char*)devcb->devnm, (char*)devnm) == 0 ) {
			return devcb; /* 発見 */
		}
	}

	return NULL;
}

/**
 * @brief	新規登録用 DevCB の獲得
 *
 * 未使用キューからデバイス登録情報を1つ取り出し、デバイス名を設定して
 * 使用中キューへ登録します。
 *
 * @param	devnm	物理デバイス名
 * @return	獲得したデバイス登録情報。空きがない場合は NULL。
 */
LOCAL DevCB* newDevCB( CONST UB *devnm )
{
	DevCB	*devcb;

	devcb = (DevCB*)QueRemoveNext(&knl_FreeDevCB);
	if ( devcb == NULL ) {
		return NULL; /* 空きなし */
	}

	knl_strncpy((char*)devcb->devnm, (char*)devnm, L_DEVNM+1);
	QueInit(&devcb->openq);

	QueInsert(&devcb->q, &knl_UsedDevCB);

	return devcb;
}

/**
 * @brief	DevCB の解放
 *
 * デバイス登録情報を使用中キューから外し、未使用キューへ戻します。
 *
 * @param	devcb	解放するデバイス登録情報
 */
LOCAL void delDevCB( DevCB *devcb )
{
	QueRemove(&devcb->q);
	QueInsert(&devcb->q, &knl_FreeDevCB);
	devcb->devnm[0] = '\0';
}

/**
 * @brief	デバイスの登録
 *
 * 物理デバイス名 devnm のデバイスを登録します。既に登録済みの場合は
 * 登録情報を更新し、pk_ddev が NULL の場合は登録を解除します。
 *
 * @param	devnm	物理デバイス名
 * @param	pk_ddev	デバイス登録情報（NULL なら登録解除）
 * @param	pk_idev	デバイス初期設定情報の返却先（NULL 可）
 * @return	正の値ならデバイスID。負の値ならエラーコード。
 * @retval	E_PAR	デバイス名長やサブユニット数が不正
 * @retval	E_NOEXS	登録解除対象のデバイスが未登録
 * @retval	E_LIMIT	登録可能デバイス数（MAX_REGDEV）の上限超過
 * @retval	E_BUSY	登録解除対象のデバイスがオープン中
 */
SYSCALL ID tk_def_dev( CONST UB *devnm, CONST T_DDEV *pk_ddev, T_IDEV *pk_idev )
{
	DevCB	*devcb;
	INT	len;
	ER	ercd;

	LockREG();

	len = knl_strlen((char*)devnm);
	if ( len == 0 || len > L_DEVNM ) {
		ercd = E_PAR;
		goto err_ret1;
	}

	if ( pk_ddev != NULL ) {
		if ( pk_ddev->nsub < 0 || pk_ddev->nsub > MAX_UNIT ) {
			ercd = E_PAR;
			goto err_ret1;
		}

		/* 論理デバイス名の長さが文字数制限を
		   超えないことを確認 */
		if ( pk_ddev->nsub > 0   ) {
			++len;
		}
		if ( pk_ddev->nsub > 10  ) {
			++len;
		}
		if ( pk_ddev->nsub > 100 ) {
			++len;
		}
		if ( len > L_DEVNM ) {
			ercd = E_PAR;
			goto err_ret1;
		}
	}

	LockDM();

	/* 'devnm' デバイスが登録済みかどうか検索 */
	devcb = knl_searchDevCB(devnm);
	if ( devcb == NULL ) {
		if ( pk_ddev == NULL ) {
			ercd = E_NOEXS;
			goto err_ret2;
		}

		/* 未登録のため、新規登録用の 'devcb' を
		   獲得する */
		devcb = newDevCB(devnm);
		if ( devcb == NULL ) {
			ercd = E_LIMIT;
			goto err_ret2;
		}
	}

	if ( pk_ddev != NULL ) {
		/* デバイス登録情報の設定・更新 */
		devcb->ddev = *pk_ddev;

		if ( pk_idev != NULL ) {
			/* デバイス初期設定情報 */
			*pk_idev = knl_DefaultIDev;
		}
	} else {
		if ( !isQueEmpty(&devcb->openq) ) {
			/* 使用中（オープン中） */
			ercd = E_BUSY;
			goto err_ret2;
		}

		/* デバイスの登録解除 */
		delDevCB(devcb);
	}

	UnlockDM();
	UnlockREG();

	return DID(devcb);

err_ret2:
	UnlockDM();
err_ret1:
	UnlockREG();
	return ercd;
}

/**
 * @brief	デバイス初期設定情報の参照
 *
 * デバイス初期設定情報（デフォルト値）を pk_idev に返します。
 *
 * @param	pk_idev	デバイス初期設定情報の返却先
 * @retval	E_OK	常に正常終了
 */
SYSCALL ER tk_ref_idv( T_IDEV *pk_idev )
{
	LockDM();
	*pk_idev = knl_DefaultIDev;
	UnlockDM();

	return E_OK;
}

/* ------------------------------------------------------------------------ */

/**
 * @brief	物理デバイス名の取得
 *
 * 論理デバイス名（ldevnm）からサブユニット番号（戻り値）と
 * 物理デバイス名（pdevnm）を取り出します。
 *
 * @param	pdevnm	物理デバイス名の格納先
 * @param	ldevnm	論理デバイス名
 * @return	サブユニット番号 + 1。サブユニット指定がない場合は 0。
 */
EXPORT INT knl_phydevnm( UB *pdevnm, CONST UB *ldevnm )
{
	UB	c;
	INT	unitno;

	while ( (c = *ldevnm) != '\0' ) {
		if ( c >= '0' && c <= '9' ) {
			break;
		}
		*pdevnm++ = c;
		ldevnm++;
	}
	*pdevnm = '\0';

	unitno = 0;
	if (c != '\0') {
		while ( (c = *ldevnm) != '\0' ) {
			unitno = unitno * 10 + (c - '0');
			ldevnm++;
		}
		++unitno;
	}

	return unitno;
}

/**
 * @brief	論理デバイス名の取得
 *
 * 物理デバイス名（pdevnm）とサブユニット番号（unitno）から
 * 論理デバイス名（ldevnm）を作成します。
 *
 * @param	ldevnm	論理デバイス名の格納先
 * @param	pdevnm	物理デバイス名
 * @param	unitno	サブユニット番号 + 1（0 ならサブユニットなし）
 */
LOCAL void logdevnm( UB *ldevnm, UB *pdevnm, INT unitno )
{
	UB	unostr[12], *cp;

	knl_strcpy((char*)ldevnm, (char*)pdevnm);
	if ( unitno > 0 ) {
		cp = &unostr[11];
		*cp = '\0';
		while (*ldevnm != '\0') {
			++ldevnm;
		}
		--unitno;
		do {
			*(--cp) = (UB)('0' + (unitno % 10));
			unitno /= 10;
		} while (unitno);
		knl_strcat((char*)ldevnm, (char*)cp);
	}
}

/**
 * @brief	デバイス名の取得
 *
 * デバイスID（devid）から論理デバイス名を取得して devnm に返します。
 *
 * @param	devid	デバイスID
 * @param	devnm	論理デバイス名の格納先
 * @return	正の値なら物理デバイスのデバイスID。負の値ならエラーコード。
 * @retval	E_ID	devid が不正
 * @retval	E_NOEXS	デバイスが未登録、またはサブユニット番号が範囲外
 */
SYSCALL ID tk_get_dev( ID devid, UB *devnm )
{
	DevCB	*devcb;
	ER	ercd;

	ercd = knl_check_devid(devid);
	if ( ercd < E_OK ) {
		goto err_ret1;
	}

	LockDM();

	devcb = DEVCB(devid);
	if ( (devcb->devnm[0] == '\0')||(UNITNO(devid) > devcb->ddev.nsub) ) {
		ercd = E_NOEXS;
		goto err_ret2;
	}

	logdevnm(devnm, devcb->devnm, UNITNO(devid));

	UnlockDM();

	return DID(devcb);

err_ret2:
	UnlockDM();
err_ret1:
	return ercd;
}

/**
 * @brief	デバイス情報の取得（デバイス名指定）
 *
 * 論理デバイス名 devnm で指定したデバイスの情報を pk_rdev に返します。
 *
 * @param	devnm	論理デバイス名
 * @param	pk_rdev	デバイス情報の返却先（NULL 可）
 * @return	正の値ならデバイスID。負の値ならエラーコード。
 * @retval	E_NOEXS	デバイスが未登録、またはサブユニット番号が範囲外
 */
SYSCALL ID tk_ref_dev( CONST UB *devnm, T_RDEV *pk_rdev )
{
	UB	pdevnm[L_DEVNM + 1];
	DevCB	*devcb;
	INT	unitno;
	ER	ercd;

	unitno = knl_phydevnm(pdevnm, devnm);

	LockDM();

	devcb = knl_searchDevCB(pdevnm);
	if ( devcb == NULL || unitno > devcb->ddev.nsub ) {
		ercd = E_NOEXS;
		goto err_ret2;
	}

	if ( pk_rdev != NULL ) {
		pk_rdev->devatr = devcb->ddev.devatr;
		pk_rdev->blksz  = devcb->ddev.blksz;
		pk_rdev->nsub   = devcb->ddev.nsub;
		pk_rdev->subno  = unitno;
	}

	UnlockDM();

	return DEVID(devcb, unitno);

err_ret2:
	UnlockDM();
	return ercd;
}

/**
 * @brief	デバイス情報の取得（デバイスディスクリプタ指定）
 *
 * デバイスディスクリプタ dd で指定したデバイスの情報を pk_rdev に
 * 返します。
 *
 * @param	dd	デバイスディスクリプタ
 * @param	pk_rdev	デバイス情報の返却先（NULL 可）
 * @return	正の値ならデバイスID。負の値ならエラーコード。
 * @retval	E_ID	dd が不正
 */
SYSCALL ID tk_oref_dev( ID dd, T_RDEV *pk_rdev )
{
	OpnCB	*opncb;
	DevCB	*devcb;
	INT	unitno;
	ER	ercd;

	LockDM();

	ercd = knl_check_devdesc(dd, 0, &opncb);
	if ( ercd < E_OK ) {
		goto err_ret2;
	}

	devcb  = opncb->devcb;
	unitno = opncb->unitno;

	if ( pk_rdev != NULL ) {
		pk_rdev->devatr = devcb->ddev.devatr;
		pk_rdev->blksz  = devcb->ddev.blksz;
		pk_rdev->nsub   = devcb->ddev.nsub;
		pk_rdev->subno  = unitno;
	}

	UnlockDM();

	return DEVID(devcb, unitno);

err_ret2:
	UnlockDM();
	return ercd;
}

/**
 * @brief	登録デバイス一覧の取得
 *
 * 登録されているデバイスの情報を、登録順で start 番目から最大 ndev 個
 * pk_ldev に格納します。
 *
 * @param	pk_ldev	デバイス情報配列の格納先
 * @param	start	取得開始位置（0 起点）
 * @param	ndev	取得する最大個数
 * @return	正の値なら start 番目以降の残り登録デバイス数。
 *		負の値ならエラーコード。
 * @retval	E_PAR	start または ndev が負
 * @retval	E_NOEXS	start が登録デバイス数以上
 */
SYSCALL INT tk_lst_dev( T_LDEV *pk_ldev, INT start, INT ndev )
{
	DevCB	*devcb;
	QUEUE	*q;
	INT	n, end;
	ER	ercd;

	if ( start < 0 || ndev < 0 ) {
		ercd = E_PAR;
		goto err_ret;
	}
	LockDM();

	end = start + ndev;
	n = 0;
	for ( q = knl_UsedDevCB.next; q != &knl_UsedDevCB; q = q->next ) {
		if ( n >= start && n < end ) {
			devcb = (DevCB*)q;
			pk_ldev->devatr = devcb->ddev.devatr;
			pk_ldev->blksz  = devcb->ddev.blksz;
			pk_ldev->nsub   = devcb->ddev.nsub;
			knl_strncpy((char*)pk_ldev->devnm, (char*)devcb->devnm, L_DEVNM);
			pk_ldev++;
		}
		n++;
	}

	UnlockDM();

	if ( start >= n ) {
		ercd = E_NOEXS;
		goto err_ret;
	}

	return n - start;

err_ret:
	return ercd;
}

/* ------------------------------------------------------------------------ */

/**
 * @brief	ドライバ要求イベントの送信
 *
 * devid で指定したデバイスのドライバのイベント処理関数（eventfn）を
 * 呼び出し、イベントを通知します。
 *
 * @param	devid	デバイスID
 * @param	evttyp	イベントタイプ（0 以上）
 * @param	evtinf	イベント情報
 * @return	ドライバのイベント処理関数の返値。負の値ならエラーコード。
 * @retval	E_ID	devid が不正
 * @retval	E_PAR	evttyp が負
 * @retval	E_NOEXS	デバイスが未登録、またはサブユニット番号が範囲外
 * @note	ドライバ呼び出し中は実行タスクのシステムモード
 *		（sysmode）を上げて実行します。
 */
SYSCALL INT tk_evt_dev( ID devid, INT evttyp, void *evtinf )
{
	DevCB	*devcb;
	EVTFN	eventfn;
	void	*exinf;
	ER	ercd;

	ercd = knl_check_devid(devid);
	if ( ercd < E_OK ) {
		goto err_ret1;
	}
	if ( evttyp < 0 ) {
		ercd = E_PAR;
		goto err_ret1;
	}

	LockDM();

	devcb = DEVCB(devid);
	if ( (devcb->devnm[0] == '\0')||(UNITNO(devid) > devcb->ddev.nsub) ) {
		ercd = E_NOEXS;
		goto err_ret2;
	}

	eventfn = (EVTFN)devcb->ddev.eventfn;
	exinf = devcb->ddev.exinf;

	UnlockDM();

	/* デバイスドライバ呼び出し */
	DISABLE_INTERRUPT;
	knl_ctxtsk->sysmode++;
	ENABLE_INTERRUPT;
	ercd = (*eventfn)(evttyp, evtinf, exinf);
	DISABLE_INTERRUPT;
	knl_ctxtsk->sysmode--;
	ENABLE_INTERRUPT;

	return ercd;

err_ret2:
	UnlockDM();
err_ret1:
	return ercd;
}

/* ------------------------------------------------------------------------ */

/**
 * @brief	デバイス登録情報テーブルの初期化
 *
 * デバイス登録情報テーブルの全エントリを未使用キューへつなぎます。
 *
 * @retval	E_OK	常に正常終了
 */
LOCAL ER initDevCB( void )
{
	DevCB	*devcb;
	INT	num = MAX_REGDEV;

	QueInit(&knl_UsedDevCB);
	QueInit(&knl_FreeDevCB);

	devcb = knl_DevCBtbl;
	while ( num-- > 0 ) {
		QueInsert(&devcb->q, &knl_FreeDevCB);
		devcb->devnm[0] = '\0';
		devcb++;
	}

	return E_OK;
}

/**
 * @brief	デバイス初期設定情報の初期化
 *
 * イベント通知用メッセージバッファを生成し、その ID を
 * デバイス初期設定情報（knl_DefaultIDev）に設定します。
 *
 * @retval	E_OK	正常終了
 * @return	負の値ならメッセージバッファ生成エラー（tk_cre_mbf の返値）
 */
LOCAL ER initIDev( void )
{
	ER	ercd;

#if DEVT_MBFSZ0 >= 0
	T_CMBF	cmbf;

	/* イベント通知用メッセージバッファの生成 */
	knl_strncpy((char*)&cmbf.exinf, (char*)OBJNAME_DMMBF, sizeof(cmbf.exinf));
	cmbf.mbfatr = TA_TFIFO;
	cmbf.bufsz  = DEVT_MBFSZ0;
	cmbf.maxmsz = DEVT_MBFSZ1;
	ercd = tk_cre_mbf(&cmbf);
	if ( ercd < E_OK ) {
		knl_DefaultIDev.evtmbfid = 0;
		goto err_ret;
	}
#else	/* イベント通知用メッセージバッファを使用しない */
	ercd = E_OK;
#endif
	knl_DefaultIDev.evtmbfid = ercd;

#if DEVT_MBFSZ0 >= 0
err_ret:
#endif
	return ercd;
}

/**
 * @brief	デバイス管理機能の初期化
 *
 * デバイス管理排他制御用ロック、デバイス登録情報テーブル、
 * デバイス入出力関連、デバイス初期設定情報を順に初期化し、
 * デバイス管理スタートアップ処理を実行します。
 *
 * @retval	E_OK	正常終了
 * @return	負の値なら初期化エラー。エラー時は終了処理
 *		（knl_finish_devmgr）を呼び出してから返ります。
 */
EXPORT ER knl_initialize_devmgr( void )
{
	ER	ercd;

	/* デバイス管理排他制御用ロックの生成 */
	ercd = CreateMLock(&knl_DevMgrLock, (UB*)OBJNAME_DMLOCK);
	if ( ercd < E_OK ) {
		goto err_ret;
	}

	/* デバイス登録情報テーブルの生成 */
	ercd = initDevCB();
	if ( ercd < E_OK ) {
		goto err_ret;
	}

	/* デバイス入出力関連の初期化 */
	ercd = knl_initDevIO();
	if ( ercd < E_OK ) {
		goto err_ret;
	}

	/* デバイス初期設定情報の初期化 */
	ercd = initIDev();
	if ( ercd < E_OK ) {
		goto err_ret;
	}

	knl_devmgr_startup();

	return E_OK;

err_ret:
	knl_finish_devmgr();
	return ercd;
}

/**
 * @brief	デバイス初期設定情報の登録解除
 *
 * イベント通知用メッセージバッファを削除します。
 *
 * @retval	E_OK	正常終了
 * @return	負の値ならメッセージバッファ削除エラー（tk_del_mbf の返値）
 */
LOCAL ER delIDev( void )
{
	ER	ercd = E_OK;

#if DEVT_MBFSZ0 >= 0
	/* イベント通知用メッセージバッファの削除 */
	if ( knl_DefaultIDev.evtmbfid > 0 ) {
		ercd = tk_del_mbf(knl_DefaultIDev.evtmbfid);
		knl_DefaultIDev.evtmbfid = 0;
	}


#endif /* DEVT_MBFSZ0 >= 0 */

	return ercd;
}

/**
 * @brief	デバイス管理機能の終了処理
 *
 * デバイス管理クリーンアップ処理を実行した後、デバイス初期設定情報の
 * 登録解除、デバイス入出力関連の終了処理、デバイス管理排他制御用
 * ロックの削除を行います。
 *
 * @return	knl_finishDevIO の返値（現状は常に E_OK）
 */
EXPORT ER knl_finish_devmgr( void )
{
	ER	ercd;

	knl_devmgr_cleanup();

	/* デバイス初期設定情報の登録解除 */
	ercd = delIDev();

	/* デバイス入出力関連の終了処理 */
	ercd = knl_finishDevIO();

	/* デバイス管理排他制御用ロックの削除 */
	DeleteMLock(&knl_DevMgrLock);

	return ercd;
}

#endif /* USE_DEVICE */
