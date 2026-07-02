/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	interrupt.c
 *	割込み制御（x86 ベアメタルポート）
 *
 *	IDT/PIC の初期化は boot/x86/main_mtk3.c（knl_main 呼び出し前）で
 *	済んでおり、IRQ のディスパッチは boot/x86/idt.c の
 *	x86_irq_handlers[] が担います。micro T-Kernel 2.0 ポートでも
 *	knl_intvec/tk_def_int は実配線されていなかったため、本ポートでは
 *	未サポート（E_NOSPT）とします。
 */

#include <sys/machine.h>
#ifdef X86_PC

#include "kernel.h"

/*
 * 割込み管理の初期化（IDT/PIC は boot 側で初期化済み）
 */
EXPORT ER knl_init_interrupt( void )
{
	return E_OK;
}

/*
 * 割込みハンドラの定義（tk_def_int の実体）— 未サポート
 */
EXPORT ER knl_define_inthdr( INT intno, ATR intatr, FP inthdr )
{
	(void)intno; (void)intatr; (void)inthdr;
	return E_NOSPT;
}

/*
 * 割込みハンドラからの復帰（tk_ret_int の実体）— 何もしない
 */
EXPORT void knl_return_inthdr( void )
{
}

#endif /* X86_PC */
