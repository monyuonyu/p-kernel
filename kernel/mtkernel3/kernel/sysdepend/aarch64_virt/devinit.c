/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel AArch64 ベアメタルポート（QEMU virt）
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	devinit.c
 *	デバイス初期化処理（AArch64 ベアメタルポート（QEMU virt））
 *
 *	本ポートはカーネルのデバイス管理機能を使用しません
 *	（ネットワーク・仮想 FS 等は arch/ 層が POSIX API で直接
 *	実装しています）。契約関数はすべて成功を返すだけです。
 */

#include <sys/machine.h>
#ifdef AARCH64_VIRT

#include "kernel.h"

/*
 * カーネル起動前のデバイス初期化（sysinit.c から呼ばれる）
 */
EXPORT ER knl_init_device( void )
{
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

#endif /* AARCH64_VIRT */
