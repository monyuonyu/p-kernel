/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Windows x86-64 ネイティブポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hw_setting.c
 *	ハードウェア初期化/終了処理（Windows x86-64 ネイティブポート）
 *
 *	Linux 版（linux_x86_64/hw_setting.c）と同じく BSS 上の静的配列で
 *	システムメモリ領域を供給します。加えて、最初の SwitchToFiber より
 *	前に主スレッドを Fiber 化します（knl_win_fiber_boot）。
 */

#include <sys/machine.h>
#ifdef WINDOWS_X86_64

#include "kernel.h"

#include <stdlib.h>	/* _Exit */

/* 主スレッドの Fiber 化（dispatch.c） */
IMPORT void knl_win_fiber_boot(void);

/* カーネル用ヒープ領域（Linux ポートと同じ 16MB） */
#define WIN_HEAP_SIZE		(16 * 1024 * 1024)

LOCAL UB knl_win_heap[WIN_HEAP_SIZE] __attribute__((aligned(16)));

EXPORT void *knl_lowmem_top;
EXPORT void *knl_lowmem_limit;

/*
 * カーネルイメージ終端のダミーシンボル（kloader_task.c が参照）。
 * hosted では実際のイメージ転送に使われないため形式的な定義でよい。
 */
__asm__(".globl _kernel_end\n_kernel_end:\n");

/*
 * ハードウェアの起動時初期化
 *	boot/windows/x86_64/main_win.c が knl_main() を呼ぶ前に実行します。
 */
EXPORT void knl_startup_hw( void )
{
	/* 最初のディスパッチ（SwitchToFiber）より前に主スレッドを Fiber 化。 */
	knl_win_fiber_boot();

	knl_lowmem_top   = knl_win_heap;
	knl_lowmem_limit = knl_win_heap + WIN_HEAP_SIZE;
}

/*
 * ハードウェアの終了処理（全タスク終了時、inittask.c 経由で到達）
 */
EXPORT void knl_shutdown_hw( void )
{
	_Exit(0);
}

/*
 * 再起動処理
 */
IMPORT void arch_reboot(void);

EXPORT ER knl_restart_hw( W mode )
{
	(void)mode;
	arch_reboot();
	return E_NOSPT;
}

#endif /* WINDOWS_X86_64 */
