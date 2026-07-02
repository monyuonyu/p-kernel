/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux AArch64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	interrupt.c
 *	割込み制御（Linux AArch64 ユーザモードポート）
 *
 *	本ポートに実ハードウェア割込みはありません。タイマ tick は
 *	SIGALRM（preempt.c）が直接 knl_timer_handler_startup を呼ぶため、
 *	割込みベクタテーブルは使用しません。ここではカーネルコア
 *	（int.c / sysinit.c）が要求する契約関数のみを提供します。
 */

#include <sys/machine.h>
#ifdef LINUX_AARCH64

#include "kernel.h"

/*
 * 割込み管理の初期化
 *	SIGSEGV 等のフォルトハンドラ登録（arch/linux/aarch64/fault.c）を
 *	ここで行います。SIGALRM の登録はタイマ起動時
 *	（sys_timer.h の knl_start_hw_timer）に行われます。
 */
IMPORT void arch_fault_init(void);

EXPORT ER knl_init_interrupt( void )
{
	arch_fault_init();
	return E_OK;
}

/*
 * 割込みハンドラの定義（tk_def_int の実体）
 *	本ポートに実割込みは無いため未サポートです。
 */
EXPORT ER knl_define_inthdr( INT intno, ATR intatr, FP inthdr )
{
	(void)intno; (void)intatr; (void)inthdr;
	return E_NOSPT;
}

/*
 * 割込みハンドラからの復帰（tk_ret_int の実体）
 *	高級言語ハンドラのみのため何もしません。
 */
EXPORT void knl_return_inthdr( void )
{
}

#endif /* LINUX_AARCH64 */
