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
 * @file	config.h
 * @brief	ユーザコンフィグレーション定義
 *
 * カーネルオブジェクトの最大数、システム機能の選択、
 * API パラメータチェックの有無など、システム構成を定義します。
 */

#ifndef __TK_CONFIG__
#define __TK_CONFIG__

/*---------------------------------------------------------------------- */
/*  ターゲット名
	システムのターゲット名を定義する。開発環境側でターゲット名を
	定義してもよい。
 */
//#define _IOTE_M367_
//#define _IOTE_RX231_
//#define _IOTE_STM32L4_
//#define _IOTE_RZA2M_

/*---------------------------------------------------------------------- */
/* SYSCONF : micro T-Kernel システムコンフィグレーション
 */

#define	CNF_SYSTEMAREA_TOP	0	/* 0: システム既定のアドレスを使用 */
#define CNF_SYSTEMAREA_END	0	/* 0: システム既定のアドレスを使用 */

#define	CNF_MAX_TSKPRI		32	/* タスクの最大優先度 */

#define CNF_TIMER_PERIOD	10	/* システムタイマの周期 */

/* カーネルオブジェクトの最大数 */
#define CNF_MAX_TSKID		32	/* タスク */
#define CNF_MAX_SEMID		16	/* セマフォ */
#define CNF_MAX_FLGID		16	/* イベントフラグ */
#define CNF_MAX_MBXID		8	/* メールボックス */
#define CNF_MAX_MTXID		4	/* ミューテックス */
#define CNF_MAX_MBFID		8	/* メッセージバッファ */
#define CNF_MAX_MPLID		4	/* 可変長メモリプール */
#define CNF_MAX_MPFID		8	/* 固定長メモリプール */
#define CNF_MAX_CYCID		4	/* 周期ハンドラ */
#define CNF_MAX_ALMID		8	/* アラームハンドラ */

/* デバイス管理の設定 */
#define CNF_MAX_REGDEV		(8)	/* 最大登録デバイス数 */
#define CNF_MAX_OPNDEV		(16)	/* 最大オープンデバイス数 */
#define CNF_MAX_REQDEV		(16)	/* 最大要求デバイス数 */
#define CNF_DEVT_MBFSZ0		(-1)	/* イベント通知用メッセージバッファのサイズ */
#define CNF_DEVT_MBFSZ1		(-1)	/* イベント通知用メッセージの最大サイズ */

/* バージョン番号 */
#define CNF_VER_MAKER		0
#define CNF_VER_PRID		0
#define CNF_VER_PRVER		3
#define CNF_VER_PRNO1		0
#define CNF_VER_PRNO2		0
#define CNF_VER_PRNO3		0
#define CNF_VER_PRNO4		0


/*---------------------------------------------------------------------- */
/* 旧バージョン互換 API サポート
 *      micro T-Kernel 2.0 API サポート（ランデブ）
 */
#define USE_LEGACY_API		(0)	/* 1: 有効  0: 無効 */
#define CNF_MAX_PORID		(0)	/* ランデブポートの最大数 */


/*---------------------------------------------------------------------- */
/* スタックサイズ定義
 */
#define CNF_EXC_STACK_SIZE	(2048)	/* 例外スタックのサイズ */
#define	CNF_TMP_STACK_SIZE	(256)	/* 一時スタックのサイズ */


/*---------------------------------------------------------------------- */
/* システム機能の選択
 *  1: 機能を使用する  0: 使用しない
 */
#define USE_NOINIT		(0)	/* 非初期化（noinit）セクションの使用（bss ゼロクリアの対象外領域） */
#define USE_IMALLOC		(1)	/* 動的メモリ割り当ての使用 */
#define USE_SHUTDOWN		(1)	/* システムシャットダウンの使用 */
#define USE_STATIC_IVT		(0)	/* 静的割込みベクタテーブルの使用 */


/*---------------------------------------------------------------------- */
/* API パラメータのチェック
 *   1: チェックする  0: チェックしない
 */
#define CHK_NOSPT		(1)	/* 未サポート機能のチェック（E_NOSPT） */
#define CHK_RSATR		(1)	/* 予約属性エラーのチェック（E_RSATR） */
#define CHK_PAR			(1)	/* パラメータのチェック（E_PAR） */
#define CHK_ID			(1)	/* オブジェクト ID 範囲のチェック（E_ID） */
#define CHK_OACV		(1)	/* オブジェクトアクセス権違反のチェック（E_OACV） */
#define CHK_CTX			(1)	/* タスク独立部実行中かどうかのチェック（E_CTX） */
#define CHK_CTX1		(1)	/* ディスパッチ禁止状態のチェック */
#define CHK_CTX2		(1)	/* タスク独立部のチェック */
#define CHK_SELF		(1)	/* 自タスク指定のチェック（E_OBJ） */

#define	CHK_TKERNEL_CONST	(1)	/* const 型パラメータのチェック */

/*---------------------------------------------------------------------- */
/* ユーザ初期化プログラム（UserInit）
 *
 */
#define	USE_USERINIT		(0)	/*  1: UserInit を使用する  0: 使用しない */
#define RI_USERINIT		(0)	/* UserInit の開始アドレス */


/*---------------------------------------------------------------------- */
/* デバッガサポート機能
 *   1: 有効  0: 無効
 */
#define USE_DBGSPT		(1)	/* mT-Kernel/DS の使用 */
#define USE_OBJECT_NAME		(0)	/* DS オブジェクト名の使用 */

#define OBJECT_NAME_LENGTH	(8)	/* DS オブジェクト名の長さ */

/*---------------------------------------------------------------------- */
/* T-Monitor 互換 API ライブラリと端末へのメッセージ出力の使用
 *  1: 有効  0: 無効
 */
#define	USE_TMONITOR		(1)	/* T-Monitor API */
#define USE_SYSTEM_MESSAGE	(1)	/* システムメッセージ */
#define USE_EXCEPTION_DBG_MSG	(1)	/* 例外デバッグメッセージ */
#define USE_TASK_DBG_MSG	(0)	/* タスクデバッグメッセージ */

/*---------------------------------------------------------------------- */
/* コプロセッサの使用
 *  1: 有効  0: 無効
 */
#define	USE_FPU			(0)	/* FPU の使用 */
#define	USE_DSP			(0)	/* DSP の使用 */

/*---------------------------------------------------------------------- */
/* 物理タイマの使用
 *  1: 有効  0: 無効
 */
#define USE_PTMR		(1)	/* 物理タイマの使用 */

/*---------------------------------------------------------------------- */
/* サンプルデバイスドライバの使用
 *  1: 有効  0: 無効
 */
#define USE_SDEV_DRV		(0)	/* サンプルデバイスドライバの使用 */

/*---------------------------------------------------------------------- */
/*
 *	標準 C インクルードファイルの使用
 */
#define USE_STDINC_STDDEF	(1)	/* <stddef.h> を使用 */

#define USE_STDINC_STDINT	(1) /* <stdint.h> を使用 */

/*---------------------------------------------------------------------- */
/*
 *	ターゲット別オーバライド（p-kernel 追加）
 *
 *	Linux x86-64 ユーザモードポートでは、p-kernel の分散レイヤーが
 *	要求するオブジェクト数（micro T-Kernel 2.0 ポートの
 *	arch/linux/include/utk_config_depend.h と同値）に引き上げる。
 */
#ifdef _LINUX_X86_64_

#undef  CNF_MAX_TSKID
#define CNF_MAX_TSKID		128	/* タスク */
#undef  CNF_MAX_SEMID
#define CNF_MAX_SEMID		256	/* セマフォ（kdds が handle ごとに使用） */
#undef  CNF_MAX_MPLID
#define CNF_MAX_MPLID		2	/* 可変長メモリプール */
#undef  CNF_MAX_CYCID
#define CNF_MAX_CYCID		8	/* 周期ハンドラ */

/* デバッガサポートは未使用（td_* API の呼び出し元なし） */
#undef  USE_DBGSPT
#define USE_DBGSPT		(0)

/* 物理タイマは未使用（tick は SIGALRM が供給） */
#undef  USE_PTMR
#define USE_PTMR		(0)

/* 例外デバッグメッセージはホスト側 fault.c が担当 */
#undef  USE_EXCEPTION_DBG_MSG
#define USE_EXCEPTION_DBG_MSG	(0)

#endif /* _LINUX_X86_64_ */

/*---------------------------------------------------------------------- */
/*
 *	使用機能の定義
 */
#include "config_func.h"

/* Linux x86-64 ターゲット: config_func.h の既定値の上書き（p-kernel 追加） */
#ifdef _LINUX_X86_64_

/* カーネルのデバイス管理は使用しない（arch/ 層が POSIX で代替） */
#undef  USE_DEVICE
#define USE_DEVICE		(0)

#endif /* _LINUX_X86_64_ */

#endif /* __TK_CONFIG__ */
