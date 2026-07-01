/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux x86-64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_cntl.c
 *	CPU 制御（Linux x86-64 ユーザモードポート）
 *
 *	本ポートの CPU 依存グローバル変数の実体と、offset.h の
 *	オフセット定数のビルド時検証を行います。
 */

#include <sys/machine.h>
#ifdef LINUX_X86_64

#include "kernel.h"
#include "offset.h"

#include <stddef.h>	/* offsetof */

/*
 * タスク独立部（擬似割込みハンドラ実行中）ネストカウンタ
 *	dispatch.S の knl_timer_handler_startup が inc/dec し、
 *	cpu_status.h の knl_isTaskIndependent() が参照します。
 */
EXPORT W knl_taskindp = 0;

/*
 * offset.h（アセンブラ用オフセット定数）と実際の TCB レイアウトの
 * ビルド時照合。TCB のフィールド構成・config を変更してズレた場合、
 * ここでコンパイルエラーになります（実行時の暴走より先に検出）。
 */
_Static_assert( offsetof(TCB, task)    == TCB_task,
		"offset.h の TCB_task が TCB 実レイアウトと不一致" );
_Static_assert( offsetof(TCB, tskctxb) == TCB_tskctxb,
		"offset.h の TCB_tskctxb が TCB 実レイアウトと不一致" );
_Static_assert( offsetof(CTXB, ssp)    == CTXB_ssp,
		"offset.h の CTXB_ssp が CTXB 実レイアウトと不一致" );

#endif /* LINUX_X86_64 */
