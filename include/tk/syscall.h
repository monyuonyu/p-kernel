/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.07.B0
 *
 *    Copyright (C) 2006-2023 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2023/11.
 *
 *----------------------------------------------------------------------
 */

/**
 *	@file	syscall.h
 *	@brief	μT-Kernel システムコール定義
 *
 *	tk_* システムコールのプロトタイプ宣言と、各オブジェクトの生成情報・
 *	状態情報などのパケット構造体、および関連する定数を定義します。
 */

#ifndef __TK_SYSCALL_H__
#define __TK_SYSCALL_H__

/* タスク生成 */
#define TSK_SELF	0		/* 自タスクの指定 */
#define TPRI_INI	0		/* タスク起動時の優先度を指定 */
#define TPRI_RUN	0		/* 実行中の最高優先度を指定 */

#define TA_ASM		0x00000000UL	/* アセンブリ言語で記述 */
#define TA_HLNG		0x00000001UL	/* 高級言語で記述 */
#define TA_USERBUF	0x00000020UL	/* ユーザバッファの指定 */
#define TA_DSNAME	0x00000040UL	/* オブジェクト名を使用 */

#define TA_RNG0		0x00000000UL	/* 保護レベル0で実行 */
#define TA_RNG1		0x00000100UL	/* 保護レベル1で実行 */
#define TA_RNG2		0x00000200UL	/* 保護レベル2で実行 */
#define TA_RNG3		0x00000300UL	/* 保護レベル3で実行 */

#define TA_COP0		0x00001000U	/* コプロセッサ(ID=0)を使用 */
#define TA_COP1		0x00002000U	/* コプロセッサ(ID=1)を使用 */
#define TA_COP2		0x00004000U	/* コプロセッサ(ID=2)を使用 */
#define TA_COP3		0x00008000U	/* コプロセッサ(ID=3)を使用 */

/* タスク状態 tskstat */
#define TTS_RUN		0x00000001U	/* 実行状態（RUN） */
#define TTS_RDY		0x00000002U	/* 実行可能状態（READY） */
#define TTS_WAI		0x00000004U	/* 待ち状態（WAIT） */
#define TTS_SUS		0x00000008U	/* 強制待ち状態（SUSPEND） */
#define TTS_WAS		0x0000000cU	/* 二重待ち状態（WAIT-SUSPEND） */
#define TTS_DMT		0x00000010U	/* 休止状態（DORMANT） */
#define TTS_NODISWAI	0x00000080U	/* 待ち禁止拒否状態 */

/* 待ち要因 tskwait */
#define TTW_SLP		0x00000001UL	/* 起床待ちによる待ち */
#define TTW_DLY		0x00000002UL	/* タスク遅延による待ち */
#define TTW_SEM		0x00000004UL	/* セマフォ待ち */
#define TTW_FLG		0x00000008UL	/* イベントフラグ待ち */
#define TTW_MBX		0x00000040UL	/* メールボックス待ち */
#define TTW_MTX		0x00000080UL	/* ミューテックス待ち */
#define TTW_SMBF	0x00000100UL	/* メッセージバッファ送信待ち */
#define TTW_RMBF	0x00000200UL	/* メッセージバッファ受信待ち */
#define TTW_CAL		0x00000400UL	/* ランデブ呼出待ち */
#define TTW_ACP		0x00000800UL	/* ランデブ受付待ち */
#define TTW_RDV		0x00001000UL	/* ランデブ終了待ち */
#define TTW_MPF		0x00002000UL	/* 固定長メモリプール待ち */
#define TTW_MPL		0x00004000UL	/* 可変長メモリプール待ち */

/* セマフォ生成 */
#define TA_TFIFO	0x00000000UL	/* 待ちタスクをFIFOで管理 */
#define TA_TPRI		0x00000001UL	/* 待ちタスクを優先度順で管理 */
#define TA_FIRST	0x00000000UL	/* 待ち行列先頭のタスクを優先 */
#define TA_CNT		0x00000002UL	/* 要求数の少ないタスクを優先 */
#define TA_DSNAME	0x00000040UL	/* オブジェクト名を使用 */

/* ミューテックス */
#define TA_TFIFO	0x00000000UL	/* 待ちタスクをFIFOで管理 */
#define TA_TPRI		0x00000001UL	/* 待ちタスクを優先度順で管理 */
#define TA_INHERIT	0x00000002UL	/* 優先度継承プロトコル */
#define TA_CEILING	0x00000003UL	/* 上限優先度プロトコル */
#define TA_DSNAME	0x00000040UL	/* オブジェクト名を使用 */

/* イベントフラグ */
#define TA_TFIFO	0x00000000UL	/* 待ちタスクをFIFOで管理 */
#define TA_TPRI		0x00000001UL	/* 待ちタスクを優先度順で管理 */
#define TA_WSGL		0x00000000UL	/* 複数タスクの待ちを禁止 */
#define TA_WMUL		0x00000008UL	/* 複数タスクの待ちを許可 */
#define TA_DSNAME	0x00000040UL	/* オブジェクト名を使用 */

/* イベントフラグ待ちモード */
#define TWF_ANDW	0x00000000U	/* AND待ち */
#define TWF_ORW		0x00000001U	/* OR待ち */
#define TWF_CLR		0x00000010U	/* 全ビットクリアの指定 */
#define TWF_BITCLR	0x00000020U	/* 条件ビットのみクリアの指定 */

/* メールボックス */
#define TA_TFIFO	0x00000000UL	/* 待ちタスクをFIFOで管理 */
#define TA_TPRI		0x00000001UL	/* 待ちタスクを優先度順で管理 */
#define TA_MFIFO	0x00000000UL	/* メッセージをFIFOで管理 */
#define TA_MPRI		0x00000002UL	/* メッセージを優先度順で管理 */
#define TA_DSNAME	0x00000040UL	/* オブジェクト名を使用 */

/* メッセージバッファ */
#define TA_TFIFO	0x00000000UL	/* 待ちタスクをFIFOで管理 */
#define TA_TPRI		0x00000001UL	/* 待ちタスクを優先度順で管理 */
#define TA_USERBUF	0x00000020UL	/* ユーザバッファの指定 */
#define TA_DSNAME	0x00000040UL	/* オブジェクト名を使用 */

/* ランデブ */
#define TA_TFIFO	0x00000000UL	/* 待ちタスクをFIFOで管理 */
#define TA_TPRI		0x00000001UL	/* 待ちタスクを優先度順で管理 */
#define TA_DSNAME	0x00000040UL	/* オブジェクト名を使用 */

/* ハンドラ */
#define TA_ASM		0x00000000UL	/* アセンブリ言語で記述 */
#define TA_HLNG		0x00000001UL	/* 高級言語で記述 */

/* 可変長メモリプール */
#define TA_TFIFO	0x00000000UL	/* 待ちタスクをFIFOで管理 */
#define TA_TPRI		0x00000001UL	/* 待ちタスクを優先度順で管理 */
#define TA_USERBUF	0x00000020UL	/* ユーザバッファの指定 */
#define TA_DSNAME	0x00000040UL	/* オブジェクト名を使用 */
#define TA_RNG0		0x00000000UL	/* 保護レベル0 */
#define TA_RNG1		0x00000100UL	/* 保護レベル1 */
#define TA_RNG2		0x00000200UL	/* 保護レベル2 */
#define TA_RNG3		0x00000300UL	/* 保護レベル3 */

/* 固定長メモリプール */
#define TA_TFIFO	0x00000000UL	/* 待ちタスクをFIFOで管理 */
#define TA_TPRI		0x00000001UL	/* 待ちタスクを優先度順で管理 */
#define TA_USERBUF	0x00000020UL	/* ユーザバッファの指定 */
#define TA_DSNAME	0x00000040UL	/* オブジェクト名を使用 */
#define TA_RNG0		0x00000000UL	/* 保護レベル0 */
#define TA_RNG1		0x00000100UL	/* 保護レベル1 */
#define TA_RNG2		0x00000200UL	/* 保護レベル2 */
#define TA_RNG3		0x00000300UL	/* 保護レベル3 */

/* 周期ハンドラ */
#define TA_ASM		0x00000000UL	/* アセンブリ言語で記述 */
#define TA_HLNG		0x00000001UL	/* 高級言語で記述 */
#define TA_STA		0x00000002UL	/* 周期ハンドラを動作状態で生成 */
#define TA_PHS		0x00000004UL	/* 周期ハンドラの位相を保存 */
#define TA_DSNAME	0x00000040UL	/* オブジェクト名を使用 */

#define TCYC_STP	0x00U		/* 周期ハンドラが動作していない */
#define TCYC_STA	0x01U		/* 周期ハンドラが動作している */

/* アラームハンドラ */
#define TA_ASM		0x00000000UL	/* アセンブリ言語で記述 */
#define TA_HLNG		0x00000001UL	/* 高級言語で記述 */
#define TA_DSNAME	0x00000040UL	/* オブジェクト名を使用 */

#define TALM_STP	0x00U		/* アラームハンドラが動作していない */
#define TALM_STA	0x01U		/* アラームハンドラが動作している */

/* システム状態 */
#define TSS_TSK		0x00U	/* タスク部（コンテキスト）の実行中 */
#define TSS_DDSP	0x01U	/* ディスパッチ禁止中 */
#define TSS_DINT	0x02U	/* 割込み禁止中 */
#define TSS_INDP	0x04U	/* タスク独立部の実行中 */
#define TSS_QTSK	0x08U	/* 準タスク部の実行中 */

/* 省電力モード */
#define TPW_DOSUSPEND	1	/* サスペンド状態への移行 */
#define TPW_DISLOWPOW	2	/* 省電力モード切替の禁止 */
#define TPW_ENALOWPOW	3	/* 省電力モード切替の許可 */

/*
 * タスク生成情報 		tk_cre_tsk
 */
typedef struct t_ctsk {
	void	*exinf;		/* 拡張情報 */
	ATR	tskatr;		/* タスク属性 */
	FP	task;		/* タスク起動アドレス */
	PRI	itskpri;	/* タスク起動時優先度 */
	SZ	stksz;		/* ユーザスタックサイズ（バイト数） */
#if USE_OBJECT_NAME
	UB	dsname[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
	void	*bufptr;	/* ユーザバッファ */
} T_CTSK;

/*
 * タスク状態情報 		tk_ref_tsk
 */
typedef	struct t_rtsk {
	void	*exinf;		/* 拡張情報 */
	PRI	tskpri;		/* 現在の優先度 */
	PRI	tskbpri;	/* ベース優先度 */
	UINT	tskstat;	/* タスク状態 */
	UW	tskwait;	/* 待ち要因 */
	ID	wid;		/* 待ち対象のオブジェクトID */
	INT	wupcnt;		/* 起床要求キューイング数 */
	INT	suscnt;		/* 強制待ち（SUSPEND）要求のネスト数 */
} T_RTSK;

/*
 * セマフォ生成情報		tk_cre_sem
 */
typedef	struct t_csem {
	void	*exinf;		/* 拡張情報 */
	ATR	sematr;		/* セマフォ属性 */
	INT	isemcnt;	/* セマフォの初期カウント値 */
	INT	maxsem;		/* セマフォの最大カウント値 */
#if USE_OBJECT_NAME
	UB	dsname[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} T_CSEM;

/*
 * セマフォ状態情報		tk_ref_sem
 */
typedef	struct t_rsem {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 待ちタスクのID */
	INT	semcnt;		/* 現在のセマフォカウント値 */
} T_RSEM;

/*
 * ミューテックス生成情報		tk_cre_mtx
 */
typedef struct t_cmtx {
	void	*exinf;		/* 拡張情報 */
	ATR	mtxatr;		/* ミューテックス属性 */
	PRI	ceilpri;	/* ミューテックスの上限優先度 */
#if USE_OBJECT_NAME
	UB	dsname[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} T_CMTX;

/*
 * ミューテックス状態情報		tk_ref_mtx
 */
typedef struct t_rmtx {
	void	*exinf;		/* 拡張情報 */
	ID	htsk;		/* ロックしているタスクのID */
	ID	wtsk;		/* ロック待ちタスクのID */
} T_RMTX;

/*
 * イベントフラグ生成情報	tk_cre_flg
 */
typedef	struct t_cflg {
	void	*exinf;		/* 拡張情報 */
	ATR	flgatr;		/* イベントフラグ属性 */
	UINT	iflgptn;	/* イベントフラグの初期値 */
#if USE_OBJECT_NAME
	UB	dsname[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} T_CFLG;

/*
 * イベントフラグ状態情報		tk_ref_flg
 */
typedef	struct t_rflg {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 待ちタスクのID */
	UINT	flgptn;		/* 現在のイベントフラグパターン */
} T_RFLG;

/*
 * メールボックス生成情報	tk_cre_mbx
 */
typedef	struct t_cmbx {
	void	*exinf;		/* 拡張情報 */
	ATR	mbxatr;		/* メールボックス属性 */
#if USE_OBJECT_NAME
	UB	dsname[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} T_CMBX;

/*
 * メールボックスのメッセージヘッダ
 */
typedef struct t_msg {
	void	*msgque[1];	/* メッセージキュー用領域 */
} T_MSG;

typedef struct t_msg_pri {
	T_MSG	msgque;		/* メッセージキュー用領域 */
	PRI	msgpri;		/* メッセージ優先度 */
} T_MSG_PRI;

/*
 * メールボックス状態情報		tk_ref_mbx
 */
typedef	struct t_rmbx {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 待ちタスクのID */
	T_MSG	*pk_msg;	/* 次に受信されるメッセージ */
} T_RMBX;

/*
 * メッセージバッファ生成情報	tk_cre_mbf
 */
typedef	struct t_cmbf {
	void	*exinf;		/* 拡張情報 */
	ATR	mbfatr;		/* メッセージバッファ属性 */
	SZ	bufsz;		/* メッセージバッファのサイズ（バイト数） */
	INT	maxmsz;		/* メッセージの最大長（バイト数） */
#if USE_OBJECT_NAME
	UB	dsname[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
	void	*bufptr;		/* ユーザバッファ */
} T_CMBF;

/*
 * メッセージバッファ状態情報 	tk_ref_mbf
 */
typedef struct t_rmbf {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 受信待ちタスクのID */
	ID	stsk;		/* 送信待ちタスクのID */
	INT	msgsz;		/* 次に受信されるメッセージのサイズ（バイト数） */
	SZ	frbufsz;	/* 空きバッファのサイズ（バイト数） */
	INT	maxmsz;		/* メッセージの最大長（バイト数） */
} T_RMBF;

/*
 * ランデブポート生成情報	tk_cre_por
 */
typedef	struct t_cpor {
	void	*exinf;		/* 拡張情報 */
	ATR	poratr;		/* ランデブポート属性 */
	INT	maxcmsz;	/* 呼出メッセージの最大長（バイト数） */
	INT	maxrmsz;	/* 返答メッセージの最大長（バイト数） */
#if USE_OBJECT_NAME
	UB	dsname[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} T_CPOR;

/*
 * ランデブポート状態情報	tk_ref_por
 */
typedef struct t_rpor {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 呼出待ちタスクのID */
	ID	atsk;		/* 受付待ちタスクのID */
	INT	maxcmsz;	/* 呼出メッセージの最大長（バイト数） */
	INT	maxrmsz;	/* 返答メッセージの最大長（バイト数） */
} T_RPOR;

/*
 * 割込みハンドラ定義情報	tk_def_int
 */
typedef struct t_dint {
	ATR	intatr;		/* 割込みハンドラ属性 */
	FP	inthdr;		/* 割込みハンドラアドレス */
} T_DINT;

/*
 * 可変長メモリプール生成情報	tk_cre_mpl
 */
typedef	struct t_cmpl {
	void	*exinf;		/* 拡張情報 */
	ATR	mplatr;		/* メモリプール属性 */
	SZ	mplsz;		/* メモリプール全体のサイズ（バイト数） */
#if USE_OBJECT_NAME
	UB	dsname[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
	void	*bufptr;		/* ユーザバッファ */
} T_CMPL;

/*
 * 可変長メモリプール状態情報	tk_ref_mpl
 */
typedef struct t_rmpl {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 待ちタスクのID */
	SZ	frsz;		/* 空き領域の合計サイズ（バイト数） */
	SZ	maxsz;		/* 最大の連続空き領域のサイズ（バイト数） */
} T_RMPL;

/*
 * 固定長メモリプール生成情報	tk_cre_mpf
 */
typedef	struct t_cmpf {
	void	*exinf;		/* 拡張情報 */
	ATR	mpfatr;		/* メモリプール属性 */
	SZ	mpfcnt;		/* メモリプール全体のブロック数 */
	SZ	blfsz;		/* 固定長メモリブロックのサイズ（バイト数） */
#if USE_OBJECT_NAME
	UB	dsname[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
	void	*bufptr;		/* ユーザバッファ */
} T_CMPF;

/*
 * 固定長メモリプール状態情報	tk_ref_mpf
 */
typedef struct t_rmpf {
	void	*exinf;		/* 拡張情報 */
	ID	wtsk;		/* 待ちタスクのID */
	SZ	frbcnt;		/* 空きブロック数 */
} T_RMPF;

/*
 * 周期ハンドラ生成情報 	tk_cre_cyc
 */
typedef struct t_ccyc {
	void	*exinf;		/* 拡張情報 */
	ATR	cycatr;		/* 周期ハンドラ属性 */
	FP	cychdr;		/* 周期ハンドラアドレス */
	RELTIM	cyctim;		/* 起動周期の時間間隔 */
	RELTIM	cycphs;		/* 起動位相 */
#if USE_OBJECT_NAME
	UB	dsname[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} T_CCYC;

/*
 * 周期ハンドラ状態情報	tk_ref_cyc
 */
typedef struct t_rcyc {
	void	*exinf;		/* 拡張情報 */
	RELTIM	lfttim;		/* 次のハンドラ起動までの残り時間 */
	UINT	cycstat;	/* 周期ハンドラの動作状態 */
} T_RCYC;

/*
 * アラームハンドラ生成情報		tk_cre_alm
 */
typedef struct t_calm {
	void	*exinf;		/* 拡張情報 */
	ATR	almatr;		/* アラームハンドラ属性 */
	FP	almhdr;		/* アラームハンドラアドレス */
#if USE_OBJECT_NAME
	UB	dsname[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} T_CALM;

/*
 * アラームハンドラ状態情報	tk_ref_alm
 */
typedef struct t_ralm {
	void	*exinf;		/* 拡張情報 */
	RELTIM	lfttim;		/* ハンドラ起動までの残り時間 */
	UINT	almstat;	/* アラームハンドラの動作状態 */
} T_RALM;

/*
 * バージョン情報		tk_ref_ver
 */
typedef struct t_rver {
	UH	maker;		/* OSの製造元 */
	UH	prid;		/* OSの識別番号 */
	UH	spver;		/* 仕様書バージョン */
	UH	prver;		/* OSの製品バージョン */
	UH	prno[4];	/* 製品番号・製品管理情報 */
} T_RVER;

/*
 * システム状態情報		tk_ref_sys
 */
typedef struct t_rsys {
	UINT	sysstat;	/* システム状態 */
	ID	runtskid;	/* 実行状態にあるタスクのID */
	ID	schedtskid;	/* 本来実行状態とすべきタスクのID */
} T_RSYS;

/*
 * サブシステム定義情報 		tk_def_ssy
 */
typedef struct t_dssy {
	ATR	ssyatr;		/* サブシステム属性 */
	PRI	ssypri;		/* サブシステム優先度 */
	FP	svchdr;		/* 拡張SVCハンドラアドレス */
	FP	breakfn;	/* ブレーク関数アドレス */
	FP	eventfn;	/* イベント関数アドレス */
} T_DSSY;

/*
 * サブシステム状態情報		tk_ref_ssy
 */
typedef struct t_rssy {
	PRI	ssypri;		/* サブシステム優先度 */
} T_RSSY;

/* ------------------------------------------------------------------------ */

/*
 * デバイス管理
 */

#define L_DEVNM		8	/* デバイス名の長さ */

/*
 * デバイス属性（ATR）
 *
 *	IIII IIII IIII IIII PRxx xxxx KKKK KKKK
 *
 *	上位16ビットはデバイス依存属性であり、各デバイスごとに定義します。
 *	下位16ビットは標準属性であり、以下のように定義します。
 */
#define TD_PROTECT	0x8000U		/* P: 書込み禁止 */
#define TD_REMOVABLE	0x4000U		/* R: メディアの取り外しが可能 */

#define TD_DEVKIND	0x00ffU		/* K: デバイス/メディアの種別 */
#define TD_DEVTYPE	0x00f0U		/*    デバイスタイプ */

/* デバイスタイプ */
#define TDK_UNDEF	0x0000U		/* 未定義・不明 */
#define TDK_DISK	0x0010U		/* ディスクデバイス */

/* ディスクタイプ */
#define TDK_DISK_UNDEF	0x0010U		/* その他のディスク */
#define TDK_DISK_RAM	0x0011U		/* RAMディスク（主メモリを使用） */
#define TDK_DISK_ROM	0x0012U		/* ROMディスク（主メモリを使用） */
#define TDK_DISK_FLA	0x0013U		/* フラッシュROMその他のシリコンディスク */
#define TDK_DISK_FD	0x0014U		/* フロッピーディスク */
#define TDK_DISK_HD	0x0015U		/* ハードディスク */
#define TDK_DISK_CDROM	0x0016U		/* CD-ROM */

/*
 * デバイスオープンモード
 */
#define TD_READ		0x0001U		/* 読込み専用 */
#define TD_WRITE	0x0002U		/* 書込み専用 */
#define TD_UPDATE	0x0003U		/* 読み書き両用 */
#define TD_EXCL		0x0100U		/* 排他 */
#define TD_WEXCL	0x0200U		/* 排他書込み */
#define TD_REXCL	0x0400U		/* 排他読込み */

/*
 * デバイスクローズオプション
 */
#define TD_EJECT	0x0001U		/* メディアの排出 */

/*
 * サスペンドモード
 */
#define TD_SUSPEND	0x0001U		/* サスペンド */
#define TD_DISSUS	0x0002U		/* サスペンド禁止 */
#define TD_ENASUS	0x0003U		/* サスペンド許可 */
#define TD_CHECK	0x0004U		/* サスペンド禁止要求数の取得 */
#define TD_FORCE	0x8000U		/* 強制サスペンドの指定 */

/*
 * デバイス情報
 */
typedef struct t_rdev {
	ATR	devatr;		/* デバイス属性 */
	W	blksz;		/* 固有データのブロックサイズ（-1: 不明） */
	INT	nsub;		/* サブユニット数 */
	INT	subno;		/* 0: 物理デバイス、1〜nsub: サブユニット番号+1 */
} T_RDEV;

/*
 * 登録デバイス情報
 */
typedef struct t_ldev {
	ATR	devatr;		/* デバイス属性 */
	W	blksz;		/* 固有データのブロックサイズ（-1: 不明） */
	INT	nsub;		/* サブユニット数 */
	UB	devnm[L_DEVNM];	/* 物理デバイス名 */
} T_LDEV;

/*
 * 共通属性データ番号
 *	RW: 読込み（tk_rea_dev）・書込み（tk_wri_dev）可能
 *	R-: 読込み（tk_rea_dev）のみ可能
 */
#define TDN_EVENT	(-1)	/* RW: イベント通知用メッセージバッファID */
#define TDN_DISKINFO	(-2)	/* R-: ディスク情報 */
#define TDN_DISPSPEC	(-3)	/* R-: 表示デバイス仕様 */
#define TDN_PCMCIAINFO	(-4)	/* R-: PCカード情報 */

/*
 * デバイスイベントタイプ
 */
typedef	enum tdevttyp {
	TDE_unknown	= 0,		/* 未定義 */
	TDE_MOUNT	= 0x01,		/* メディアの挿入 */
	TDE_EJECT	= 0x02,		/* メディアの排出 */
	TDE_ILLMOUNT	= 0x03,		/* メディアの不正挿入 */
	TDE_ILLEJECT	= 0x04,		/* メディアの不正排出 */
	TDE_REMOUNT	= 0x05,		/* メディアの再挿入 */
	TDE_CARDBATLOW	= 0x06,		/* カード電池の残量低下 */
	TDE_CARDBATFAIL	= 0x07,		/* カード電池の異常 */
	TDE_REQEJECT	= 0x08,		/* メディアの排出要求 */
	TDE_PDBUT	= 0x11,		/* PDボタン状態の変化 */
	TDE_PDMOVE	= 0x12,		/* PD位置の移動 */
	TDE_PDSTATE	= 0x13,		/* PD状態の変化 */
	TDE_PDEXT	= 0x14,		/* PD拡張イベント */
	TDE_KEYDOWN	= 0x21,		/* キーの押下 */
	TDE_KEYUP	= 0x22,		/* キーの解放 */
	TDE_KEYMETA	= 0x23,		/* メタキー状態の変化 */
	TDE_POWEROFF	= 0x31,		/* 電源スイッチオフ */
	TDE_POWERLOW	= 0x32,		/* 電源の残量低下 */
	TDE_POWERFAIL	= 0x33,		/* 電源の異常 */
	TDE_POWERSUS	= 0x34,		/* 自動サスペンド */
	TDE_POWERUPTM	= 0x35,		/* 時計の更新 */
	TDE_CKPWON	= 0x41		/* 自動電源オンの通知 */
} TDEvtTyp;

/*
 * デバイスイベントメッセージ形式
 */
typedef struct t_devevt {
	TDEvtTyp	evttyp;		/* イベントタイプ */
	/* 以下にイベントタイプごとの情報が付加される */
} T_DEVEVT;

/*
 * デバイスID付きデバイスイベントメッセージ形式
 */
typedef struct t_devevt_id {
	TDEvtTyp	evttyp;		/* イベントタイプ */
	ID		devid;		/* デバイスID */
	/* 以下にイベントタイプごとの情報が付加される */
} T_DEVEVT_ID;

/* ------------------------------------------------------------------------ */

/*
 * デバイス登録情報
 */
typedef struct t_ddev {
	void	*exinf;		/* 拡張情報 */
	ATR	drvatr;		/* ドライバ属性 */
	ATR	devatr;		/* デバイス属性 */
	INT	nsub;		/* サブユニット数 */
	W	blksz;		/* 固有データのブロックサイズ（-1: 不明） */
	FP	openfn;		/* オープン関数 */
	FP	closefn;	/* クローズ関数 */
	FP	execfn;		/* 処理開始関数 */
	FP	waitfn;		/* 完了待ち関数 */
	FP	abortfn;	/* 中止処理関数 */
	FP	eventfn;	/* イベント関数 */
} T_DDEV;

/*
 * オープン関数:
 *	ER  openfn( ID devid, UINT omode, void *exinf )
 * クローズ関数:
 *	ER  closefn( ID devid, UINT option, void *exinf )
 * 処理開始関数:
 *	ER  execfn( T_DEVREQ *devreq, TMO tmout, void *exinf )
 * 完了待ち関数:
 *	INT waitfn( T_DEVREQ *devreq, INT nreq, TMO tmout, void *exinf )
 * 中止処理関数:
 *	ER  abortfn( ID tskid, T_DEVREQ *devreq, INT nreq, void *exinf )
 * イベント関数:
 *	INT eventfn( INT evttyp, void *evtinf, void *exinf )
 */

/*
 * ドライバ属性
 */
#define TDA_OPENREQ	0x0001U	/* オープン/クローズのたびに毎回通知 */

/*
 * デバイス初期設定情報
 */
typedef struct t_idev {
	ID	evtmbfid;	/* イベント通知用メッセージバッファID */
} T_IDEV;

/*
 * デバイス要求パケット
 *	 I: 入力パラメータ
 *	 O: 出力パラメータ
 */
typedef struct t_devreq {
	struct t_devreq	*next;	/* I: 要求パケットへのリンク（NULL: 終端） */
	void	*exinf;		/* X: 拡張情報 */
	ID	devid;		/* I: 対象デバイスID */
	INT	cmd:4;		/* I: 要求コマンド */
	BOOL	abort:1;	/* I: 中止要求の実行中はTRUE */
	W	start;		/* I: 開始データ番号 */
	W	size;		/* I: 要求サイズ */
	void	*buf;		/* I: 入出力バッファアドレス */
	W	asize;		/* O: 結果サイズ */
	ER	error;		/* O: 結果エラー */
} T_DEVREQ;

/*
 * 要求コマンド
 */
#define TDC_READ	1	/* 読込み要求 */
#define TDC_WRITE	2	/* 書込み要求 */

/*
 * ドライバ要求イベント
 */
#define TDV_SUSPEND	(-1)	/* サスペンド */
#define TDV_RESUME	(-2)	/* リジューム */
#define TDV_CARDEVT	1	/* PCカードイベント（カードマネージャ参照） */
#define TDV_USBEVT	2	/* USBイベント（USBマネージャ参照） */

/*
 * システムコールのプロトタイプ宣言
 */
IMPORT ID tk_cre_tsk( CONST T_CTSK *pk_ctsk );  /* タスクの生成 */
IMPORT ER tk_del_tsk( ID tskid );  /* タスクの削除 */
IMPORT ER tk_sta_tsk( ID tskid, INT stacd );  /* タスクの起動 */
IMPORT void tk_ext_tsk( void );  /* 自タスクの終了 */
IMPORT void tk_exd_tsk( void );  /* 自タスクの終了と削除 */
IMPORT ER tk_ter_tsk( ID tskid );  /* 他タスクの強制終了 */
IMPORT ER tk_dis_dsp( void );  /* ディスパッチの禁止 */
IMPORT ER tk_ena_dsp( void );  /* ディスパッチの許可 */
IMPORT ER tk_chg_pri( ID tskid, PRI tskpri );  /* タスク優先度の変更 */
IMPORT ER tk_rot_rdq( PRI tskpri );  /* タスク優先順位の回転 */
IMPORT ER tk_rel_wai( ID tskid );  /* 他タスクの待ち状態解除 */
IMPORT ID tk_get_tid( void );  /* 実行状態タスクのID取得 */
IMPORT ER tk_ref_tsk( ID tskid, T_RTSK *pk_rtsk );  /* タスク状態の参照 */
IMPORT ER tk_sus_tsk( ID tskid );  /* タスクの強制待ち */
IMPORT ER tk_rsm_tsk( ID tskid );  /* 強制待ち状態からの再開 */
IMPORT ER tk_frsm_tsk( ID tskid );  /* 強制待ち状態からの強制再開 */
IMPORT ER tk_slp_tsk( TMO tmout );  /* 自タスクの起床待ち */
IMPORT ER tk_wup_tsk( ID tskid );  /* 他タスクの起床 */
IMPORT INT tk_can_wup( ID tskid );  /* タスクの起床要求の無効化 */
IMPORT ER tk_dly_tsk( RELTIM dlytim );  /* 自タスクの遅延 */

#if TK_SUPPORT_REGOPS
IMPORT ER tk_get_reg( ID tskid, T_REGS *pk_regs, T_EIT *pk_eit, T_CREGS *pk_cregs );  /* タスクレジスタの取得 */
IMPORT ER tk_set_reg( ID tskid, CONST T_REGS *pk_regs, CONST T_EIT *pk_eit, CONST T_CREGS *pk_cregs );  /* タスクレジスタの設定 */
#endif /* TK_SUPPORT_REGOPS */

#if NUM_COPROCESSOR > 0
IMPORT ER tk_get_cpr( ID tskid, INT copno, T_COPREGS *pk_copregs);  /* コプロセッサレジスタの取得 */
IMPORT ER tk_set_cpr(ID tskid, INT copno, CONST T_COPREGS *pk_copregs);  /* コプロセッサレジスタの設定 */
#endif

IMPORT ID tk_cre_sem( CONST T_CSEM *pk_csem );  /* セマフォの生成 */
IMPORT ER tk_del_sem( ID semid );  /* セマフォの削除 */
IMPORT ER tk_sig_sem( ID semid, INT cnt );  /* セマフォ資源の返却 */
IMPORT ER tk_wai_sem( ID semid, INT cnt, TMO tmout );  /* セマフォ資源の獲得 */
IMPORT ER tk_ref_sem( ID semid, T_RSEM *pk_rsem );  /* セマフォ状態の参照 */

IMPORT ID tk_cre_mtx( CONST T_CMTX *pk_cmtx );  /* ミューテックスの生成 */
IMPORT ER tk_del_mtx( ID mtxid );  /* ミューテックスの削除 */
IMPORT ER tk_loc_mtx( ID mtxid, TMO tmout );  /* ミューテックスのロック */
IMPORT ER tk_unl_mtx( ID mtxid );  /* ミューテックスのアンロック */
IMPORT ER tk_ref_mtx( ID mtxid, T_RMTX *pk_rmtx );  /* ミューテックス状態の参照 */

IMPORT ID tk_cre_flg( CONST T_CFLG *pk_cflg );  /* イベントフラグの生成 */
IMPORT ER tk_del_flg( ID flgid );  /* イベントフラグの削除 */
IMPORT ER tk_set_flg( ID flgid, UINT setptn );  /* イベントフラグのセット */
IMPORT ER tk_clr_flg( ID flgid, UINT clrptn );  /* イベントフラグのクリア */
IMPORT ER tk_wai_flg( ID flgid, UINT waiptn, UINT wfmode, UINT *p_flgptn, TMO tmout );  /* イベントフラグ待ち */
IMPORT ER tk_ref_flg( ID flgid, T_RFLG *pk_rflg );  /* イベントフラグ状態の参照 */

IMPORT ID tk_cre_mbx( CONST T_CMBX* pk_cmbx );  /* メールボックスの生成 */
IMPORT ER tk_del_mbx( ID mbxid );  /* メールボックスの削除 */
IMPORT ER tk_snd_mbx( ID mbxid, T_MSG *pk_msg );  /* メールボックスへの送信 */
IMPORT ER tk_rcv_mbx( ID mbxid, T_MSG **ppk_msg, TMO tmout );  /* メールボックスからの受信 */
IMPORT ER tk_ref_mbx( ID mbxid, T_RMBX *pk_rmbx );  /* メールボックス状態の参照 */
IMPORT ID tk_cre_mbf( CONST T_CMBF *pk_cmbf );  /* メッセージバッファの生成 */
IMPORT ER tk_del_mbf( ID mbfid );  /* メッセージバッファの削除 */
IMPORT ER tk_snd_mbf( ID mbfid, CONST void *msg, INT msgsz, TMO tmout );  /* メッセージバッファへの送信 */
IMPORT INT tk_rcv_mbf( ID mbfid, void *msg, TMO tmout );  /* メッセージバッファからの受信 */
IMPORT ER tk_ref_mbf( ID mbfid, T_RMBF *pk_rmbf );  /* メッセージバッファ状態の参照 */

IMPORT ID tk_cre_por( CONST T_CPOR *pk_cpor );  /* ランデブポートの生成 */
IMPORT ER tk_del_por( ID porid );  /* ランデブポートの削除 */
IMPORT INT tk_cal_por( ID porid, UINT calptn, void *msg, INT cmsgsz, TMO tmout );  /* ランデブの呼出 */
IMPORT INT tk_acp_por( ID porid, UINT acpptn, RNO *p_rdvno, void *msg, TMO tmout );  /* ランデブの受付 */
IMPORT ER tk_fwd_por( ID porid, UINT calptn, RNO rdvno, CONST void *msg, INT cmsgsz );  /* ランデブの回送 */
IMPORT ER tk_rpl_rdv( RNO rdvno, CONST void *msg, INT rmsgsz );  /* ランデブへの返答 */
IMPORT ER tk_ref_por( ID porid, T_RPOR *pk_rpor );  /* ランデブポート状態の参照 */

IMPORT ER tk_def_int( UINT intno, CONST T_DINT *pk_dint );  /* 割込みハンドラの定義 */
IMPORT void tk_ret_int( void );  /* 割込みハンドラからの復帰 */

IMPORT ID tk_cre_mpl( CONST T_CMPL *pk_cmpl );  /* 可変長メモリプールの生成 */
IMPORT ER tk_del_mpl( ID mplid );  /* 可変長メモリプールの削除 */
IMPORT ER tk_get_mpl( ID mplid, SZ blksz, void **p_blk, TMO tmout );  /* 可変長メモリブロックの獲得 */
IMPORT ER tk_rel_mpl( ID mplid, void *blk );  /* 可変長メモリブロックの返却 */
IMPORT ER tk_ref_mpl( ID mplid, T_RMPL *pk_rmpl );  /* 可変長メモリプール状態の参照 */

IMPORT ID tk_cre_mpf( CONST T_CMPF *pk_cmpf );  /* 固定長メモリプールの生成 */
IMPORT ER tk_del_mpf( ID mpfid );  /* 固定長メモリプールの削除 */
IMPORT ER tk_get_mpf( ID mpfid, void **p_blf, TMO tmout );  /* 固定長メモリブロックの獲得 */
IMPORT ER tk_rel_mpf( ID mpfid, void *blf );  /* 固定長メモリブロックの返却 */
IMPORT ER tk_ref_mpf( ID mpfid, T_RMPF *pk_rmpf );  /* 固定長メモリプール状態の参照 */

IMPORT ER tk_set_utc( CONST SYSTIM *pk_tim );  /* システム時刻（UTC）の設定 */
IMPORT ER tk_get_utc( SYSTIM *pk_tim );  /* システム時刻（UTC）の取得 */
IMPORT ER tk_set_tim( CONST SYSTIM *pk_tim );  /* システム時刻の設定 */
IMPORT ER tk_get_tim( SYSTIM *pk_tim );  /* システム時刻の取得 */
IMPORT ER tk_get_otm( SYSTIM *pk_tim );  /* システム稼働時間の取得 */

IMPORT ID tk_cre_cyc( CONST T_CCYC *pk_ccyc );  /* 周期ハンドラの生成 */
IMPORT ER tk_del_cyc( ID cycid );  /* 周期ハンドラの削除 */
IMPORT ER tk_sta_cyc( ID cycid );  /* 周期ハンドラの動作開始 */
IMPORT ER tk_stp_cyc( ID cycid );  /* 周期ハンドラの動作停止 */
IMPORT ER tk_ref_cyc( ID cycid, T_RCYC *pk_rcyc );  /* 周期ハンドラ状態の参照 */

IMPORT ID tk_cre_alm( CONST T_CALM *pk_calm );  /* アラームハンドラの生成 */
IMPORT ER tk_del_alm( ID almid );  /* アラームハンドラの削除 */
IMPORT ER tk_sta_alm( ID almid, RELTIM almtim );  /* アラームハンドラの動作開始 */
IMPORT ER tk_stp_alm( ID almid );  /* アラームハンドラの動作停止 */
IMPORT ER tk_ref_alm( ID almid, T_RALM *pk_ralm );  /* アラームハンドラ状態の参照 */

IMPORT ER tk_ref_sys( T_RSYS *pk_rsys );  /* システム状態の参照 */
IMPORT ER tk_set_pow( UINT powmode);  /* 省電力モードの設定 */
IMPORT ER tk_ref_ver( T_RVER *pk_rver );  /* バージョン情報の参照 */

IMPORT ER tk_def_ssy( ID ssid, CONST T_DSSY *pk_dssy );  /* サブシステムの定義 */
IMPORT ER tk_ref_ssy( ID ssid, T_RSSY *pk_rssy );  /* サブシステム状態の参照 */

IMPORT ID tk_opn_dev( CONST UB *devnm, UINT omode );  /* デバイスのオープン */
IMPORT ER tk_cls_dev( ID dd, UINT option );  /* デバイスのクローズ */
IMPORT ID tk_rea_dev( ID dd, W start, void *buf, SZ size, TMO tmout );  /* デバイスの読込み開始（非同期） */
IMPORT ER tk_srea_dev( ID dd, W start, void *buf, SZ size, SZ *asize );  /* デバイスの同期読込み */
IMPORT ID tk_wri_dev( ID dd, W start, CONST void *buf, SZ size, TMO tmout );  /* デバイスの書込み開始（非同期） */
IMPORT ER tk_swri_dev( ID dd, W start, CONST void *buf, SZ size, SZ *asize );  /* デバイスの同期書込み */
IMPORT ID tk_wai_dev( ID dd, ID reqid, SZ *asize, ER *ioer, TMO tmout );  /* デバイス要求の完了待ち */
IMPORT INT tk_sus_dev( UINT mode );  /* サスペンド状態の制御 */
IMPORT ID tk_get_dev( ID devid, UB *devnm );  /* デバイス名の取得 */
IMPORT ID tk_ref_dev( CONST UB *devnm, T_RDEV *pk_rdev );  /* デバイス情報の参照（デバイス名指定） */
IMPORT ID tk_oref_dev( ID dd, T_RDEV *pk_rdev );  /* デバイス情報の参照（デバイスディスクリプタ指定） */
IMPORT INT tk_lst_dev( T_LDEV *pk_ldev, INT start, INT ndev );  /* 登録デバイスの一覧取得 */
IMPORT INT tk_evt_dev( ID devid, INT evttyp, void *evtinf );  /* デバイスへのイベント送信 */
IMPORT ID tk_def_dev( CONST UB *devnm, CONST T_DDEV *pk_ddev, T_IDEV *pk_idev );  /* デバイスの登録 */
IMPORT ER tk_ref_idv( T_IDEV *pk_idev );  /* デバイス初期設定情報の参照 */

#endif /* __TK_SYSCALL_H__ */
