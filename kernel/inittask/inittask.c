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
 * @file	inittask.c
 * @brief	初期タスクの定義
 *
 * カーネル起動時に生成される初期タスクの生成情報（knl_init_ctsk）と、
 * その本体である init_task_main() を定義します。初期タスクは
 * サブシステム・デバイスドライバを起動した後、ユーザプログラム
 * usermain() を呼び出し、その復帰値に従ってシステムを停止します。
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <sys/inittask.h>

#include "kernel.h"

#if !USE_IMALLOC
INT	init_task_stack[INITTASK_STKSZ/sizeof(INT)];
#endif

typedef INT	(*MAIN_FP)(INT, UB **);

/*
 * 初期タスク生成パラメータ
 */

LOCAL void init_task_main(void);

EXPORT const T_CTSK knl_init_ctsk = {
	(void *)INITTASK_EXINF,		/* 拡張情報 */
	INITTASK_TSKATR,		/* タスク属性 */
	(FP)&init_task_main,		/* タスク起動アドレス */
	INITTASK_ITSKPRI,		/* 起動時優先度 */
	INITTASK_STKSZ,			/* スタックサイズ */
#if USE_OBJECT_NAME
	INITTASK_DSNAME,		/* オブジェクト名 */
#endif
	INITTASK_STACK,			/* スタック領域先頭アドレス */
};

/* --------------------------------------------------------------- */
/**
 * @brief	システムの起動
 *
 * 各サブシステムと各デバイスドライバを起動します。
 * USE_DEVICE 有効時はデバイス管理機能を初期化した後、システム依存の
 * デバイス起動処理（knl_start_device）を実行します。
 *
 * @retval E_OK	正常終了
 * @return 初期化・起動処理が返したエラーコード。
 */
LOCAL ER start_system( void )
{
	ER	ercd;

#if USE_DEVICE
	/* デバイス管理機能の初期化 */
	ercd = knl_initialize_devmgr();
	if ( ercd < E_OK ) return ercd;
#endif

	/* システム依存の起動シーケンス */
	ercd = knl_start_device();

	return ercd;
}

/**
 * @brief	システムの停止
 *
 * プラットフォーム依存の終了処理を実行し、システムを停止します。
 * この関数からは戻りません。
 *
 * @param fin	停止方法の指定
 *		-  0 : 電源オフ
 *		- -1 : リセット後に再起動（Reset → Boot → Start）
 *		- -2 : 高速再起動（Start）
 *		- -3 : 通常再起動（Boot → Start）
 *
 * @note fin の各値が常にサポートされるとは限りません。
 *	USE_SHUTDOWN 無効時は割込みを禁止して無限ループします。
 */
LOCAL void shutdown_system( INT fin )
{
#if USE_SHUTDOWN
	/* プラットフォーム依存の終了シーケンス */
	knl_finish_device();

	/* シャットダウンメッセージの出力 */
	if ( fin >= 0 ) {
		SYSTEM_MESSAGE("\n<< SYSTEM SHUTDOWN >>\n");
	}

	if ( fin < 0 ) {
		/* 再起動シーケンス（プラットフォーム依存） */
		knl_restart_hw(fin);
	}

	knl_tkernel_exit();		/* システムの停止 */
#else
	DISABLE_INTERRUPT;
	for(;;) {
		;
	}
#endif /* USE_SHUTDOWN */
}


/**
 * @brief	初期タスク本体
 *
 * サブシステム・デバイスドライバを起動し、USE_USERINIT 有効時は
 * ユーザ定義初期化（RI_USERINIT）を実行した後、usermain() を呼び
 * 出します。usermain() の復帰後（または起動失敗時）は
 * shutdown_system() でシステムを停止するため、この関数からは
 * 戻りません。
 *
 * @note usermain() の復帰値が shutdown_system() の fin に渡ります。
 *	ユーザ定義初期化が 0 以下を返した場合 usermain() は呼ばれません。
 */
LOCAL void init_task_main(void)
{
	INT	fin = 1;
	ER	ercd;

	ercd = start_system();		/* サブシステム・デバイスドライバの起動 */
	if(ercd  >= E_OK) {

#if (USE_SYSTEM_MESSAGE && USE_TMONITOR)
		tm_printf((UB*)"\n\nmicroT-Kernel Version %x.%02x\n\n", VER_MAJOR, VER_MINOR);
#endif

#if USE_USERINIT
		/* ユーザ定義初期化シーケンスの実行 */
		fin = (*(MAIN_FP)RI_USERINIT)(0, NULL);
#endif
		if ( fin > 0 ) {
			fin = usermain();	/* ユーザメインプログラム */
		}
#if USE_USERINIT
		/* ユーザ定義終了シーケンスの実行 */
		(*(MAIN_FP)RI_USERINIT)(-1, NULL);
#endif

	} else {
		SYSTEM_MESSAGE("!ERROR! Init Task start\n");	/* 起動失敗メッセージ */
	}

	shutdown_system(fin);	/* ここには戻らない */
}
