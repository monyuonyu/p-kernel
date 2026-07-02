/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	devinit.c
 *	デバイス初期化処理（x86 ベアメタルポート）
 *
 *	カーネルのデバイス管理機能（USE_DEVICE）は使用しません。
 *	サブシステム互換層（p-kernel 拡張、subsystem.c）の初期化を
 *	ここで行います — fs/net/blk サブシステムの tk_def_ssy より
 *	前に SSYCB テーブルを初期化しておく必要があるためです。
 */

#include <sys/machine.h>
#ifdef X86_PC

#include "kernel.h"
#include "../../tkernel/subsystem.h"

/*
 * カーネル起動前のデバイス初期化（sysinit.c から呼ばれる）
 */
EXPORT ER knl_init_device( void )
{
#if USE_SUBSYSTEM
	knl_subsystem_initialize();
#endif
	return E_OK;
}

/*
 * デバイスドライバの起動（初期タスク開始後、inittask.c から呼ばれる）
 */
EXPORT ER knl_start_device( void )
{
	return E_OK;
}

/*
 * デバイスドライバの終了（システムシャットダウン時）
 */
EXPORT ER knl_finish_device( void )
{
	return E_OK;
}

#endif /* X86_PC */
