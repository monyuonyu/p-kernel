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
 * @file	device.h
 * @brief	デバイス管理機能のカーネル内部定義
 *
 * デバイス登録管理・入出力管理のテーブル、デバイスID・デバイス
 * ディスクリプタ・要求IDの変換マクロ、およびデバイス管理内部
 * 関数の宣言を提供します。
 */

#ifndef _DEVICE_H_
#define _DEVICE_H_

/* デバッグ用に .exinf へ設定するオブジェクト名 */
#define OBJNAME_DMMBF	"DEvt"		/* イベント通知用メッセージバッファ */
#define OBJNAME_DMSEM	"DMSy"		/* 同期制御用セマフォ */
#define OBJNAME_DMLOCK	"DMLk"		/* デバイス管理用マルチロック */

IMPORT	T_IDEV		knl_DefaultIDev;

/*
 *	デバイス登録管理
 */

IMPORT DevCB knl_DevCBtbl[];	/* デバイス登録情報テーブル */
IMPORT QUEUE knl_UsedDevCB;	/* 使用中キュー */
IMPORT QUEUE knl_FreeDevCB;	/* 未使用キュー */

#define MAX_UNIT	255		/* サブユニットの最大数 */

/**
 * @brief	デバイスIDの正当性検査
 *
 * デバイスID devid の物理デバイス部分が登録可能範囲
 * （1～MAX_REGDEV）にあるか検査します。
 *
 * @param	devid	デバイスID
 * @retval	E_OK	正常（範囲内）
 * @retval	E_ID	devid が範囲外
 */
Inline ER knl_check_devid( ID devid )
{
	devid >>= 8;
	if ( devid < 1 || devid > MAX_REGDEV ) {
		return E_ID;
	}
	return E_OK;
}

/*
 * デバイス管理: 入出力
 */
IMPORT OpnCB knl_OpnCBtbl[];	/* オープン管理情報テーブル */
IMPORT QUEUE knl_FreeOpnCB;	/* 未使用キュー */

#define DD(opncb)		( (opncb) - knl_OpnCBtbl + 1 )
#define OPNCB(dd)		( knl_OpnCBtbl + ((dd) - 1) )

IMPORT ReqCB knl_ReqCBtbl[];	/* 要求管理情報テーブル */
IMPORT QUEUE knl_FreeReqCB;	/* 未使用キュー */

#define REQID(reqcb)		( (reqcb) - knl_ReqCBtbl + 1 )
#define REQCB(reqid)		( knl_ReqCBtbl + ((reqid) - 1) )

#define DEVREQ_REQCB(devreq)	((ReqCB*)((B*)(devreq) - offsetof(ReqCB, req)))

IMPORT ResCB knl_resource_control_block;


#include "limits.h"

/* サスペンド禁止要求カウント */
IMPORT	INT	knl_DisSusCnt;

/* サスペンド禁止要求カウントの最大値 */
#define MAX_DISSUS	INT_MAX


/**
 * @brief	デバイスドライバのアボート関数呼び出し
 *
 * ドライバのアボート関数（abortfn）を、実行タスクのシステムモード
 * （sysmode）を上げた状態で呼び出します。
 *
 * @param	devcb	デバイス登録情報
 * @param	tskid	アボート対象の要求を処理中のタスクID
 * @param	devreq	アボートする要求パケット（リスト）
 * @param	nreq	要求パケット数
 * @return	ドライバのアボート関数の返値
 */
Inline ER knl_call_abortfn( DevCB *devcb, ID tskid, T_DEVREQ *devreq, INT nreq )
{
	ER ercd;
	ABTFN	abortfn;

	abortfn = (ABTFN)devcb->ddev.abortfn;

	DISABLE_INTERRUPT;
	knl_ctxtsk->sysmode++;
	ENABLE_INTERRUPT;
	ercd = (*abortfn)(tskid, devreq, nreq, devcb->ddev.exinf);
	DISABLE_INTERRUPT;
	knl_ctxtsk->sysmode--;
	ENABLE_INTERRUPT;

	return ercd;
}


IMPORT ID knl_request( ID dd, W start, void *buf, W size, TMO tmout, INT cmd );
IMPORT BOOL knl_chkopen( DevCB *devcb, INT unitno );
IMPORT void knl_delReqCB( ReqCB *reqcb );
IMPORT ResCB* knl_GetResCB( void );
IMPORT void knl_delOpnCB( OpnCB *opncb, BOOL free );
IMPORT ER knl_close_device( OpnCB *opncb, UINT option );

#endif /* _DEVICE_H_ */
