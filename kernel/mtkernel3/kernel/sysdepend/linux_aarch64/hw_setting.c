/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux AArch64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hw_setting.c
 *	ハードウェア初期化/終了処理（Linux AArch64 ユーザモードポート）
 *
 *	ベアメタルポートの reset_main 相当の処理を担います。
 *	システムメモリ領域（Imalloc が管理する knl_lowmem_top 〜
 *	knl_lowmem_limit）は、リンカスクリプトではなく BSS 上の
 *	静的配列で供給します。
 */

#include <sys/machine.h>
#ifdef LINUX_AARCH64

#include "kernel.h"

#include <stdlib.h>	/* _Exit */

/*
 * カーネル用ヒープ領域
 *	micro T-Kernel 2.0 ポート（arch/linux/aarch64/cpu_init.c）と同じ
 *	16MB。タスクスタック・カーネルオブジェクト等はすべてここから
 *	knl_Imalloc で確保されます。16 バイト整列にしておくことで、
 *	タスクスタックの ABI 整列（cpu_task.h 参照）が単純になります。
 */
#define LINUX_HEAP_SIZE		(16 * 1024 * 1024)

LOCAL UB knl_linux_heap[LINUX_HEAP_SIZE] __attribute__((aligned(16)));

/*
 * システムメモリ領域の下端/上端
 *	ベアメタルポートでは reset_main.c が定義する変数。本ポートでは
 *	ここに実体を置き、knl_startup_hw() で値を設定します。
 */
EXPORT void *knl_lowmem_top;
EXPORT void *knl_lowmem_limit;

/*
 * カーネルイメージ終端のダミーシンボル
 *	ベアメタルではリンカスクリプトが定義する。kloader_task.c
 *	（自己複製ローダ）が参照するが、hosted では実際のイメージ
 *	転送に使われないため形式的な定義でよい
 *	（micro T-Kernel 2.0 ポートの cpu_init.c と同じ扱い）。
 */
__asm__(".globl _kernel_end\n_kernel_end:\n");

/*
 * ハードウェアの起動時初期化
 *	boot/linux_x86_64/main_mtk3.c が knl_main() を呼ぶ前に実行します。
 *	システムメモリ領域の設定のみを行います（シグナル・タイマの
 *	初期化は knl_init_interrupt / knl_start_hw_timer が担当）。
 */
EXPORT void knl_startup_hw( void )
{
	knl_lowmem_top   = knl_linux_heap;
	knl_lowmem_limit = knl_linux_heap + LINUX_HEAP_SIZE;
}

/*
 * ハードウェアの終了処理
 *	ホストプロセスを終了させます（tk_exd_tsk による全タスク終了時、
 *	inittask.c → knl_tkernel_exit 経由で到達）。
 */
EXPORT void knl_shutdown_hw( void )
{
	_Exit(0);
}

/*
 * 再起動処理
 *	ホスト側の再起動機構（arch_reboot）があればそれを使い、
 *	無ければ終了コード付きでプロセスを終えます。
 */
IMPORT void arch_reboot(void);

EXPORT ER knl_restart_hw( W mode )
{
	(void)mode;
	arch_reboot();
	return E_NOSPT;	/* arch_reboot が戻ってきた場合 */
}

#endif /* LINUX_AARCH64 */
