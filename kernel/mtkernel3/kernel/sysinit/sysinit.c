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
 * @file	sysinit.c
 * @brief	micro T-Kernel の起動と終了
 *
 * リセット後に呼ばれるカーネルエントリ（main / knl_main）と、
 * 初期タスクからのカーネル終了処理（knl_tkernel_exit）を提供します。
 */
#include "kernel.h"
#include <tm/tmonitor.h>

/**
 * @brief	micro T-Kernel の起動
 *
 * カーネル開始前の初期化シーケンスを実行します。割込み禁止状態で
 * T-Monitor 互換ライブラリ・内部メモリ割り当て（Imalloc）・デバイス・
 * 割込み・カーネルオブジェクトを順に初期化し、システムタイマを起動
 * した後、初期タスクを生成・起動して knl_force_dispatch() で初期
 * タスクへ制御を移します。
 *
 * @return 正常時は初期タスクへディスパッチするため戻りません。
 *	初期化に失敗した場合はエラーメッセージを出力して無限ループ
 *	します（USE_SHUTDOWN 時はハードウェア依存の終了処理を実行）。
 *
 * @note ADD_PREFIX_KNL_TO_GLOBAL_NAME 定義時は knl_main という名前で
 *	エクスポートされます。
 */
#ifndef ADD_PREFIX_KNL_TO_GLOBAL_NAME
EXPORT INT main( void )
#else
EXPORT INT knl_main( void )
#endif	/* ADD_PREFIX_KNL_TO_GLOBAL_NAME */
{
	ER	ercd;

	DISABLE_INTERRUPT;

#if USE_TMONITOR
	/* T-Monitor 互換ライブラリの初期化 */
	libtm_init();
#endif

#if USE_IMALLOC
	/* 内部メモリ割り当て（Imalloc）の初期化 */
	ercd = knl_init_Imalloc();
	if ( ercd < E_OK ) {
		SYSTEM_MESSAGE("!ERROR! init_Imalloc\n");
		goto err_ret;
	}
#endif /* USE_IMALLOC */

	/* micro T-Kernel 開始前のデバイス初期化 */
	ercd = knl_init_device();
	if ( ercd < E_OK ) {
		SYSTEM_MESSAGE("!ERROR! init_device\n");
		goto err_ret;
	}

	/* 割込みの初期化 */
	ercd = knl_init_interrupt();
	if ( ercd < E_OK ) {
		SYSTEM_MESSAGE("!ERROR! init_initialize\n");
		goto err_ret;
	}

	/* カーネルオブジェクトの初期化 */
	ercd = knl_init_object();
	if ( ercd < E_OK ) {
		SYSTEM_MESSAGE("!ERROR! kernel object initialize\n");
		goto err_ret1;
	}

	/* システムタイマの起動 */
	ercd = knl_timer_startup();
	if ( ercd < E_OK ) {
		SYSTEM_MESSAGE("!ERROR! System timer startup\n");
		goto err_ret1;
	}

	/* 初期タスクの生成と起動 */
	ercd = tk_cre_tsk((CONST T_CTSK *)&knl_init_ctsk);
	if ( ercd >= E_OK ) {
		ercd = tk_sta_tsk((ID)ercd, 0);
		if ( ercd >= E_OK ) {
			knl_force_dispatch();
			/**** 初期タスクを開始 ****/
			/**** ここには戻らない ****/
		} else {
			SYSTEM_MESSAGE("!ERROR! Initial Task can not start\n");
		}
	} else {
		SYSTEM_MESSAGE("!ERROR! Initial Task can not creat\n");
	}

	/* 以降はエラー処理 */

#if USE_SHUTDOWN
	knl_timer_shutdown();	/* システムタイマの停止 */
err_ret1:
	knl_shutdown_hw();	/* ハードウェア依存の終了処理 */
	/**** ここには戻らない ****/
#else
err_ret1:
#endif /* USE_SHUTDOWN */

err_ret:
	while(1);
	return 0;
}

/**
 * @brief	初期タスクからの micro T-Kernel の終了
 *
 * システムタイマを停止し、ハードウェア依存の終了処理を実行します。
 *
 * @note この関数からは戻りません。USE_SHUTDOWN 有効時のみ定義されます。
 */
#if USE_SHUTDOWN
EXPORT void knl_tkernel_exit( void )
{
	knl_timer_shutdown();	/* システムタイマの停止 */
	knl_shutdown_hw();	/* ハードウェア依存の終了処理 */
	/**** ここには戻らない ****/

	while(1);
}
#endif /* USE_SHUTDOWN */
