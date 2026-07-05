/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Windows x86-64 ネイティブポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_cntl.c
 *	CPU 制御（Windows x86-64 ネイティブポート）
 *
 *	CPU 依存グローバル変数の実体を置きます。Linux 版と異なり、Fiber
 *	ポートはアセンブラのオフセット定数（offset.h）を使わないため、
 *	その照合はありません。
 */

#include <sys/machine.h>
#ifdef WINDOWS_X86_64

#include "kernel.h"

/*
 * タスク独立部（擬似割込みハンドラ実行中）ネストカウンタ
 *	dispatch.c の knl_timer_handler_startup が inc/dec し、
 *	cpu_status.h の knl_isTaskIndependent() が参照します。
 */
EXPORT W knl_taskindp = 0;

#endif /* WINDOWS_X86_64 */
