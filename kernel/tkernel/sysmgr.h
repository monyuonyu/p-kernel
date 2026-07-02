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
 * @file	sysmgr.h
 * @brief	micro T-Kernel/SM（システム管理機能）の定義
 *
 * デバイス管理機能で用いるデバイス登録情報・オープン管理情報・
 * 要求管理情報などの構造体、排他制御用ロックマクロ、および
 * デバイス管理内部関数の宣言を提供します。
 */

#ifndef _SYSMGR_
#define _SYSMGR_

#include <tk/tkernel.h>
#include <sys/queue.h>

#include "kernel.h"


/* ------------------------------------------------------------------------ */
/*
 *	デバイス管理機能
 */

/*
 * デバイス管理排他制御用ロック
 */
IMPORT FastMLock	knl_DevMgrLock;
#define LockDM()	MLock(&knl_DevMgrLock, 0)
#define UnlockDM()	MUnlock(&knl_DevMgrLock, 0)

/*
 * デバイス登録排他制御用ロック
 */
#define LockREG()	MLock(&knl_DevMgrLock, 1)
#define UnlockREG()	MUnlock(&knl_DevMgrLock, 1)

/*
 * デバイス登録情報
 */
typedef struct DeviceControlBlock {
	QUEUE	q;
	UB	devnm[L_DEVNM+1];	/* デバイス名 */
	T_DDEV	ddev;			/* 登録情報 */
	QUEUE	openq;			/* オープンデバイス管理キュー */
} DevCB;

IMPORT	DevCB		knl_DevCBtbl[];	/* デバイス登録情報テーブル */
IMPORT	QUEUE		knl_UsedDevCB;	/* 使用中キュー */

#define DID(devcb)		( ((devcb) - knl_DevCBtbl + 1) << 8 )
#define DEVID(devcb, unitno)	( DID(devcb) + (unitno) )
#define DEVCB(devid)		( knl_DevCBtbl + (((devid) >> 8) - 1) )
#define UNITNO(devid)		( (devid) & 0xff )

/*
 * オープン管理情報
 */
typedef struct OpenControlBlock {
	QUEUE		q;
	QUEUE		resq;		/* リソース管理からの接続用 */
	ID		resid;		/* セクションリソースID */
	DevCB		*devcb;		/* 対象デバイス */
	INT		unitno;		/* サブユニット番号
					   （0: 物理デバイス） */
	UINT		omode;		/* オープンモード */
	QUEUE		requestq;	/* 要求管理キュー */
	UH		waitone;	/* 個別要求待ちの数 */
	T_DEVREQ	*waireqlst;	/* 複数要求待ちのリスト */
	INT		nwaireq;	/* 複数要求待ちの数 */
	ID		abort_tskid;	/* アボート完了待ちタスク */
	INT		abort_cnt;	/* アボート完了待ち要求の数 */
	ID		abort_semid; /* アボート完了待ち用セマフォ */
} OpnCB;

#define RESQ_OPNCB(rq)		( (OpnCB*)((B*)(rq) - offsetof(OpnCB, resq)) )

/*
 * 要求管理情報
 */
typedef struct RequestControlBlock {
	QUEUE		q;
	OpnCB		*opncb;		/* オープンデバイス */
	ID		tskid;		/* 処理中タスク */
	T_DEVREQ	req;		/* 要求パケット */
} ReqCB;

/*
 * リソース管理情報
 */
typedef struct ResourceControlBlock {
	QUEUE		openq;		/* オープンデバイス管理キュー */
	INT		dissus;		/* サスペンド禁止要求カウント */
} ResCB;

/*
 * 要求処理関数の型
 */

typedef ER  (*OPNFN)( ID devid, UINT omode, void *exinf );
typedef ER  (*ABTFN)( ID tskid, T_DEVREQ *devreq, INT nreq, void *exinf );
typedef INT (*WAIFN)( T_DEVREQ *devreq, INT nreq, TMO tmout, void *exinf );
typedef INT (*EVTFN)( INT evttyp, void *evtinf, void *exinf );
typedef ER  (*CLSFN)( ID devid, UINT option, void *exinf );
typedef ER  (*EXCFN)( T_DEVREQ *devreq, TMO tmout, void *exinf );

/* ------------------------------------------------------------------------ */

#define IMPORT_DEFINE	1
#if IMPORT_DEFINE
/* device.c */
IMPORT	FastMLock	knl_DevMgrLock;
IMPORT	DevCB		knl_DevCBtbl[];
IMPORT	QUEUE		knl_UsedDevCB;
IMPORT	DevCB*		knl_searchDevCB( CONST UB *devnm );
IMPORT	INT			knl_phydevnm( UB *pdevnm, CONST UB *ldevnm );
IMPORT	ER			knl_initialize_devmgr( void );
IMPORT	ER			knl_finish_devmgr( void );
/* deviceio.c */
IMPORT ER knl_check_devdesc( ID dd, UINT mode, OpnCB **p_opncb );
IMPORT void knl_devmgr_startup( void );
IMPORT void knl_devmgr_cleanup( void );
IMPORT ER knl_initDevIO( void );
IMPORT ER knl_finishDevIO( void );

#endif

#endif /* _SYSMGR_ */
