/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel AArch64 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hw_setting.c
 *	ハードウェア初期化/終了処理（AArch64 ベアメタルポート）
 *
 *	システムメモリ領域は、リンカシンボル _kernel_end の直後から
 *	AARCH64_SYSTEMAREA_END（QEMU virt: 0x50000000）までを割り当てます
 *	（micro T-Kernel 2.0 ポートの cpu_init.c と同じ方式）。
 */

#include <sys/machine.h>
#ifdef AARCH64_VIRT

#include "kernel.h"

/* カーネルイメージ終端（boot/aarch64/linker.ld が定義） */
IMPORT UB _kernel_end[];

/*
 * システムメモリ領域の下端/上端
 */
EXPORT void *knl_lowmem_top;
EXPORT void *knl_lowmem_limit;

/*
 * ハードウェアの起動時初期化
 *	boot/aarch64/main_mtk3.c が knl_main() を呼ぶ前に実行します。
 */
EXPORT void knl_startup_hw( void )
{
	knl_lowmem_top   = (void *)(((unsigned long)_kernel_end + 7) & ~0x7UL);
	knl_lowmem_limit = (void *)AARCH64_SYSTEMAREA_END;
}

/*
 * ハードウェアの終了処理（ベアメタルでは wfe ループで停止）
 */
EXPORT void knl_shutdown_hw( void )
{
	__asm__ volatile ("msr daifset, #0x3" ::: "memory");
	for ( ;; ) {
		__asm__ volatile ("wfe");
	}
}

/*
 * 再起動処理（PSCI SYSTEM_RESET）
 */
EXPORT ER knl_restart_hw( W mode )
{
	(void)mode;
	/* PSCI 0.2 SYSTEM_RESET (0x84000009) — QEMU virt は hvc/smc 両対応 */
	register unsigned long x0 __asm__("x0") = 0x84000009UL;
	__asm__ volatile ("hvc #0" : "+r"(x0) : : "memory");
	for ( ;; ) {
		__asm__ volatile ("wfe");
	}
}

#endif /* AARCH64_VIRT */
