/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel AArch64 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_cntl.c
 *	CPU 制御（AArch64 ベアメタルポート）
 *
 *	CPU 依存グローバル変数の実体と、offset.h のビルド時検証を
 *	提供します。ディスパッチャ本体は arch/aarch64/cpu_support.S。
 */

#include <sys/machine.h>
#ifdef AARCH64_VIRT

#include "kernel.h"
#include "offset.h"

#include <stddef.h>	/* offsetof */

/*
 * タスク独立部（割込みハンドラ実行中）ネストカウンタ
 *	cpu_support.S の knl_timer_handler_startup が増減します。
 */
EXPORT W knl_taskindp = 0;

/*
 * offset.h（アセンブラ用オフセット定数）と実際の TCB レイアウトの
 * ビルド時照合。
 */
_Static_assert( offsetof(TCB, task)    == TCB_task,
		"offset.h の TCB_task が TCB 実レイアウトと不一致" );
_Static_assert( offsetof(TCB, tskctxb) == TCB_tskctxb,
		"offset.h の TCB_tskctxb が TCB 実レイアウトと不一致" );
_Static_assert( offsetof(CTXB, ssp)    == CTXB_ssp,
		"offset.h の CTXB_ssp が CTXB 実レイアウトと不一致" );

#endif /* AARCH64_VIRT */
