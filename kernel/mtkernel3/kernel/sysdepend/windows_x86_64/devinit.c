/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Windows x86-64 ネイティブポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	devinit.c
 *	デバイス初期化処理（Windows x86-64 ネイティブポート）
 *
 *	Linux 版と同じく、カーネルのデバイス管理機能は使用しません
 *	（ネットワーク等は arch/ 層が Win32/Winsock で直接実装）。
 *	契約関数はすべて成功を返します。
 */

#include <sys/machine.h>
#ifdef WINDOWS_X86_64

#include "kernel.h"

EXPORT ER knl_init_device( void )
{
	return E_OK;
}

EXPORT ER knl_start_device( void )
{
	return E_OK;
}

EXPORT ER knl_finish_device( void )
{
	return E_OK;
}

#endif /* WINDOWS_X86_64 */
