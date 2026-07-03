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
 * @file	tkinit.c
 * @brief	μT-Kernel オブジェクトの初期化
 *
 * カーネル起動時に各カーネルオブジェクト管理機能を
 * 一括して初期化する処理を提供します。
 */

#include "kernel.h"
#include "timer.h"

/**
 * @brief 各カーネルオブジェクトの初期化
 *
 * タスク管理を初期化した後、コンフィギュレーションで有効化されている
 * 各機能（セマフォ、イベントフラグ、メールボックス、メッセージバッファ、
 * ランデブ、ミューテックス、可変長・固定長メモリプール、周期ハンドラ、
 * アラームハンドラ）を順に初期化します。
 *
 * @retval E_OK	正常終了
 * @return いずれかの初期化が失敗した場合はそのエラーコードを返します。
 */
EXPORT ER knl_init_object( void)
{	
	ER	ercd;

	ercd = knl_task_initialize();
	if(ercd < E_OK) return ercd;

#if USE_SEMAPHORE
	ercd = knl_semaphore_initialize();
	if(ercd < E_OK) return ercd;
#endif
#if USE_EVENTFLAG
	ercd = knl_eventflag_initialize();
	if(ercd < E_OK) return ercd;
#endif
#if USE_MAILBOX
	ercd = knl_mailbox_initialize();
	if(ercd < E_OK) return ercd;
#endif
#if USE_MESSAGEBUFFER
	ercd = knl_messagebuffer_initialize();
	if(ercd < E_OK) return ercd;
#endif
#if USE_LEGACY_API && USE_RENDEZVOUS
	ercd = knl_rendezvous_initialize();
	if(ercd < E_OK) return ercd;
#endif
#if USE_MUTEX
	ercd = knl_mutex_initialize();
	if(ercd < E_OK) return ercd;
#endif
#if USE_MEMORYPOOL
	ercd = knl_memorypool_initialize();
	if(ercd < E_OK) return ercd;
#endif
#if USE_FIX_MEMORYPOOL
	ercd = knl_fix_memorypool_initialize();
	if(ercd < E_OK) return ercd;
#endif
#if USE_CYCLICHANDLER
	ercd = knl_cyclichandler_initialize();
	if(ercd < E_OK) return ercd;
#endif
#if USE_ALARMHANDLER
	ercd = knl_alarmhandler_initialize();
	if(ercd < E_OK) return ercd;
#endif

	return E_OK;
}
