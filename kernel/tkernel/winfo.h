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
 * @file	winfo.h
 * @brief	同期・通信オブジェクト用待ち情報の定義
 *
 * 各同期・通信オブジェクト（セマフォ、イベントフラグ、メールボックス、
 * メッセージバッファ、ランデブ、メモリプール）の待ち情報構造体と、
 * タスク制御ブロックに置かれる待ち情報共用体（WINFO）、
 * 待ち仕様構造体（WSPEC）を定義します。
 */

#ifndef _WINFO_
#define _WINFO_

/*
 * セマフォ待ち (TTW_SEM)
 */
typedef struct {
	INT	cnt;		/* 要求資源数 */
} WINFO_SEM;

/*
 * イベントフラグ待ち (TTW_FLG)
 */
typedef struct {
	UINT	waiptn;		/* 待ちビットパターン */
	UINT	wfmode;		/* 待ちモード */
	UINT	*p_flgptn;	/* 待ち解除時のビットパターンを
				   格納するアドレス */
} WINFO_FLG;

/*
 * メールボックス待ち (TTW_MBX)
 */
typedef struct {
	T_MSG	**ppk_msg;	/* メッセージパケットの先頭を
				   格納するアドレス */
} WINFO_MBX;

/*
 * メッセージバッファ受信／送信待ち (TTW_RMBF, TTW_SMBF)
 */
typedef struct {
	void	*msg;		/* 受信メッセージを格納するアドレス */
	INT	*p_msgsz;	/* 受信メッセージサイズを格納するアドレス */
} WINFO_RMBF;

typedef struct {
	CONST void *msg;	/* 送信メッセージの先頭アドレス */
	INT	msgsz;		/* 送信メッセージサイズ */
} WINFO_SMBF;

/*
 * ランデブ呼出し／受付け／終了待ち (TTW_CAL, TTW_ACP, TTW_RDV)
 */
typedef struct {
	UINT	calptn;		/* 呼出し側の選択条件を示す
				   ビットパターン */
	void	*msg;		/* メッセージを格納するアドレス */
	INT	cmsgsz;		/* 呼出しメッセージサイズ */
	INT	*p_rmsgsz;	/* 応答メッセージサイズを格納するアドレス */
} WINFO_CAL;

typedef struct {
	UINT	acpptn;		/* 受付け側の選択条件を示す
				   ビットパターン */
	void	*msg;		/* 呼出しメッセージを格納するアドレス */
	RNO	*p_rdvno;	/* ランデブ番号を格納するアドレス */
	INT	*p_cmsgsz;	/* 呼出しメッセージサイズを格納するアドレス */
} WINFO_ACP;

typedef struct {
	RNO	rdvno;		/* ランデブ番号 */
	void	*msg;		/* メッセージを格納するアドレス */
	INT	maxrmsz;	/* 応答メッセージの最大長 */
	INT	*p_rmsgsz;	/* 応答メッセージサイズを格納するアドレス */
} WINFO_RDV;

/*
 * 可変長メモリプール待ち (TTW_MPL)
 */
typedef struct {
	W	blksz;		/* メモリブロックサイズ */
	void	**p_blk;		/* メモリブロックの先頭を
				   格納するアドレス */
} WINFO_MPL;

/*
 * 固定長メモリプール待ち (TTW_MPF)
 */
typedef struct {
	void	**p_blf;		/* メモリブロックの先頭を
				   格納するアドレス */
} WINFO_MPF;

/*
 * タスク制御ブロック内の待ち情報の定義
 */
typedef union {
#if USE_SEMAPHORE
	WINFO_SEM	sem;
#endif
#if USE_EVENTFLAG
	WINFO_FLG	flg;
#endif
#if USE_MAILBOX
	WINFO_MBX	mbx;
#endif
#if USE_MESSAGEBUFFER
	WINFO_RMBF	rmbf;
	WINFO_SMBF	smbf;
#endif
#if USE_LEGACY_API && USE_RENDEZVOUS
	WINFO_CAL	cal;
	WINFO_ACP	acp;
	WINFO_RDV	rdv;
#endif
#if USE_MEMORYPOOL
	WINFO_MPL	mpl;
#endif
#if USE_FIX_MEMORYPOOL
	WINFO_MPF	mpf;
#endif
} WINFO;

/*
 * 待ち仕様構造体の定義
 */
typedef struct {
	UW	tskwait;			/* 待ち要因 */
	void	(*chg_pri_hook)(TCB *, INT);	/* タスク優先度変更時の
						   処理 */
	void	(*rel_wai_hook)(TCB *);		/* タスク待ち解除時の
						   処理 */
} WSPEC;

#endif /* _WINFO_ */
