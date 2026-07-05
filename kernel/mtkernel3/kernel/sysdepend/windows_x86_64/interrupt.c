/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Windows x86-64 ネイティブポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	interrupt.c
 *	割込み制御（Windows x86-64 ネイティブポート）
 *
 *	本ポートに実ハードウェア割込みはありません。タイマ tick は安全点で
 *	ポンプされます（dispatch.c）。ここではカーネルコア（int.c /
 *	sysinit.c）が要求する契約関数のみを提供します。
 */

#include <sys/machine.h>
#ifdef WINDOWS_X86_64

#include "kernel.h"

/*
 * 割込み管理の初期化
 *	フォルトハンドラ登録（arch/windows/x86_64/fault.c）をここで行います。
 */
IMPORT void arch_fault_init(void);

EXPORT ER knl_init_interrupt( void )
{
	arch_fault_init();
	return E_OK;
}

EXPORT ER knl_define_inthdr( INT intno, ATR intatr, FP inthdr )
{
	(void)intno; (void)intatr; (void)inthdr;
	return E_NOSPT;
}

EXPORT void knl_return_inthdr( void )
{
}

#endif /* WINDOWS_X86_64 */
