/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.03
 *
 *    Copyright (C) 2006-2021 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2021/03/31.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	dbgspt.h
 * @brief	micro T-Kernel デバッガサポート（mT-Kernel/DS）
 *
 * デバッガサポート機能（td_* API）の状態参照用データ型と
 * API 宣言を定義します。
 */

#ifndef __TK_DBGSPT_H__
#define __TK_DBGSPT_H__

#include "tkernel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* システム依存部 */
#define DBGSPT_PATH_(a)		#a
#define DBGSPT_PATH(a)		DBGSPT_PATH_(a)
#define DBGSPT_SYSDEP()		DBGSPT_PATH(sysdepend/TARGET_DIR/dbgspt.h)
#include DBGSPT_SYSDEP()

/*
 * オブジェクト名情報の種別		td_ref_dsname, td_set_dsname
 */
#define TN_TSK 0x01
#define TN_SEM 0x02
#define TN_FLG 0x03
#define TN_MBX 0x04
#define TN_MBF 0x05
#define TN_POR 0x06
#define TN_MTX 0x07
#define TN_MPL 0x08
#define TN_MPF 0x09
#define TN_CYC 0x0a
#define TN_ALM 0x0b

/*
 * セマフォ状態情報		td_ref_sem
 */
typedef	struct td_rsem {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 待ちタスクの ID */
	INT	semcnt;		/* 現在のセマフォカウント値 */
} TD_RSEM;

/*
 * イベントフラグ状態情報		td_ref_flg
 */
typedef	struct td_rflg {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 待ちタスクの ID */
	UINT	flgptn;		/* 現在のイベントフラグパターン */
} TD_RFLG;

/*
 * メールボックス状態情報		td_ref_mbx
 */
typedef	struct td_rmbx {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 待ちタスクの ID */
	T_MSG	*pk_msg;	/* 次に受信されるメッセージ */
} TD_RMBX;

/*
 * ミューテックス状態情報		td_ref_mtx
 */
typedef struct td_rmtx {
	void	*exinf;		/* 拡張情報 */
	ID	htsk;		/* ロックしているタスクの ID */
	ID	wtsk;		/* ロック待ちタスクの ID */
} TD_RMTX;

/*
 * メッセージバッファ状態情報 	td_ref_mbf
 */
typedef struct td_rmbf {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 受信待ちタスクの ID */
	ID	stsk;		/* 送信待ちタスクの ID */
	INT	msgsz;		/* 次に受信されるメッセージのサイズ（バイト） */
	W	frbufsz;	/* 空きバッファのサイズ（バイト） */
	INT	maxmsz;		/* メッセージの最大長（バイト） */
} TD_RMBF;

/*
 * ランデブポート状態情報	td_ref_por
 */
typedef struct td_rpor {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 呼出待ちタスクの ID */
	ID	atsk;		/* 受付待ちタスクの ID */
	INT	maxcmsz;	/* 呼出メッセージの最大長（バイト） */
	INT	maxrmsz;	/* 応答メッセージの最大長（バイト） */
} TD_RPOR;

/*
 * 固定長メモリプール状態情報	td_ref_mpf
 */
typedef struct td_rmpf {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 待ちタスクの ID */
	W	frbcnt;		/* 空きブロック数 */
} TD_RMPF;

/*
 * 可変長メモリプール状態情報	td_ref_mpl
 */
typedef struct td_rmpl {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 待ちタスクの ID */
	W	frsz;		/* 空き領域の合計サイズ（バイト） */
	W	maxsz;		/* 最大の連続空き領域のサイズ
				   （バイト） */
} TD_RMPL;

/*
 * 周期ハンドラ状態情報	td_ref_cyc
 */
typedef struct td_rcyc {
	void	*exinf;		/* 拡張情報 */
	RELTIM	lfttim;		/* 次のハンドラ起動までの残り時間 */
	UINT	cycstat;	/* 周期ハンドラの動作状態 */
} TD_RCYC;

/*
 * アラームハンドラ状態情報	td_ref_alm
 */
typedef struct td_ralm {
	void	*exinf;		/* 拡張情報 */
	RELTIM	lfttim;		/* ハンドラ起動までの残り時間 */
	UINT	almstat;	/* アラームハンドラの動作状態 */
} TD_RALM;

/*
 * サブシステム状態情報		td_ref_ssy
 */
typedef struct td_rssy {
	PRI	ssypri;		/* サブシステム優先度 */
	W	resblksz;	/* リソース管理ブロックのサイズ（バイト） */
} TD_RSSY;

/*
 * タスク状態情報		td_ref_tsk
 */
typedef	struct td_rtsk {
	void	*exinf;		/* 拡張情報 */
	PRI	tskpri;		/* 現在の優先度 */
	PRI	tskbpri;	/* ベース優先度 */
	UINT	tskstat;	/* タスク状態 */
	UW	tskwait;	/* 待ち要因 */
	ID	wid;		/* 待ち対象オブジェクトの ID */
	INT	wupcnt;		/* 起床要求のキューイング数 */
	INT	suscnt;		/* 強制待ち（SUSPEND）要求のネスト数 */
	FP	task;		/* タスクの起動アドレス */
	W	stksz;		/* スタックサイズ（バイト） */
	void	*istack;		/* スタックポインタの初期値 */
} TD_RTSK;

/*
 * タスク統計情報		td_inf_tsk
 */
typedef struct td_itsk {
	RELTIM	stime;		/* 累積システム実行時間
				   （ミリ秒） */
	RELTIM	utime;		/* 累積ユーザ実行時間
				   （ミリ秒） */
} TD_ITSK;

/*
 * システム状態情報		td_ref_sys
 */
typedef struct td_rsys {
	UINT	sysstat;	/* システム状態 */
	ID	runtskid;	/* 実行状態のタスクの ID */
	ID	schedtskid;	/* 本来実行状態であるべき
				   タスクの ID */
} TD_RSYS;

/*
 * システムコール／拡張 SVC トレース定義 	td_hok_svc
 */
typedef struct td_hsvc {
	FP	enter;		/* 呼出前のフックルーチン */
	FP	leave;		/* 呼出後のフックルーチン */
} TD_HSVC;

/*
 * タスクディスパッチトレース定義		td_hok_dsp
 */
typedef struct td_hdsp {
	FP	exec;		/* 実行開始時のフックルーチン */
	FP	stop;		/* 実行停止時のフックルーチン */
} TD_HDSP;

/*
 * 例外／割込みトレース定義			td_hok_int
 */
typedef struct td_hint {
	FP	enter;		/* ハンドラ呼出前のフックルーチン */
	FP	leave;		/* ハンドラ呼出後のフックルーチン */
} TD_HINT;

/* ------------------------------------------------------------------------ */

/*
 * インタフェースライブラリ自動生成用の定義（mktdsvc）
 */
/*** DEFINE_TDSVC ***/

/* [BEGIN SYSCALLS] */

/* 各オブジェクトの使用状況の参照 */
IMPORT INT td_lst_tsk( ID list[], INT nent );
IMPORT INT td_lst_sem( ID list[], INT nent );
IMPORT INT td_lst_flg( ID list[], INT nent );
IMPORT INT td_lst_mbx( ID list[], INT nent );
IMPORT INT td_lst_mtx( ID list[], INT nent );
IMPORT INT td_lst_mbf( ID list[], INT nent );
IMPORT INT td_lst_por( ID list[], INT nent );
IMPORT INT td_lst_mpf( ID list[], INT nent );
IMPORT INT td_lst_mpl( ID list[], INT nent );
IMPORT INT td_lst_cyc( ID list[], INT nent );
IMPORT INT td_lst_alm( ID list[], INT nent );
IMPORT INT td_lst_ssy( ID list[], INT nent );

/* 各オブジェクトの状態参照 */
IMPORT ER td_ref_sem( ID semid, TD_RSEM *rsem );
IMPORT ER td_ref_flg( ID flgid, TD_RFLG *rflg );
IMPORT ER td_ref_mbx( ID mbxid, TD_RMBX *rmbx );
IMPORT ER td_ref_mtx( ID mtxid, TD_RMTX *rmtx );
IMPORT ER td_ref_mbf( ID mbfid, TD_RMBF *rmbf );
IMPORT ER td_ref_por( ID porid, TD_RPOR *rpor );
IMPORT ER td_ref_mpf( ID mpfid, TD_RMPF *rmpf );
IMPORT ER td_ref_mpl( ID mplid, TD_RMPL *rmpl );
IMPORT ER td_ref_cyc( ID cycid, TD_RCYC *rcyc );
IMPORT ER td_ref_alm( ID almid, TD_RALM *ralm );
IMPORT ER td_ref_ssy( ID ssid, TD_RSSY *rssy );

/* タスクの状態参照 */
IMPORT ER td_ref_tsk( ID tskid, TD_RTSK *rtsk );
IMPORT ER td_inf_tsk( ID tskid, TD_ITSK *itsk, BOOL clr );

#if TK_SUPPORT_REGOPS
IMPORT ER td_get_reg( ID tskid, T_REGS *regs, T_EIT *eit, T_CREGS *cregs );
IMPORT ER td_set_reg( ID tskid, CONST T_REGS *regs, CONST T_EIT *eit, CONST T_CREGS *cregs );
#endif  /* TK_SUPPORT_REGOPS */

/* システムの状態参照 */
IMPORT ER td_ref_sys( TD_RSYS *rsys );
IMPORT ER td_get_tim( SYSTIM *tim, UW *ofs );
IMPORT ER td_get_otm( SYSTIM *tim, UW *ofs );

/* 実行可能キュー（ready queue）の参照 */
IMPORT INT td_rdy_que( PRI pri, ID list[], INT nent );

/* 待ちキューの参照 */
IMPORT INT td_sem_que( ID semid, ID list[], INT nent );
IMPORT INT td_flg_que( ID flgid, ID list[], INT nent );
IMPORT INT td_mbx_que( ID mbxid, ID list[], INT nent );
IMPORT INT td_mtx_que( ID mtxid, ID list[], INT nent );
IMPORT INT td_smbf_que( ID mbfid, ID list[], INT nent );
IMPORT INT td_rmbf_que( ID mbfid, ID list[], INT nent );
IMPORT INT td_cal_que( ID porid, ID list[], INT nent );
IMPORT INT td_acp_que( ID porid, ID list[], INT nent );
IMPORT INT td_mpf_que( ID mpfid, ID list[], INT nent );
IMPORT INT td_mpl_que( ID mplid, ID list[], INT nent );

/* 実行トレース */
IMPORT ER td_hok_svc( CONST TD_HSVC *hsvc );
IMPORT ER td_hok_dsp( CONST TD_HDSP *hdsp );
IMPORT ER td_hok_int( CONST TD_HINT *hint );

/* オブジェクト名 */
IMPORT ER td_ref_dsname( UINT type, ID id, UB *dsname );
IMPORT ER td_set_dsname( UINT type, ID id, CONST UB *dsname );

/* [END SYSCALLS] */

#ifdef __cplusplus
}
#endif
#endif /* __TK_DBGSPT_H__ */
