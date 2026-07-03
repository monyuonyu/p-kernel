/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hw_setting.c
 *	ハードウェア初期化/終了処理（x86 ベアメタルポート）
 *
 *	システムメモリ領域（Imalloc が管理する knl_lowmem_top 〜
 *	knl_lowmem_limit）は、リンカシンボル _kernel_end の直後から
 *	64MB（X86_SYSTEMAREA_END）までを割り当てます
 *	（micro T-Kernel 2.0 ポートの cpu_init.c と同じ方式）。
 */

#include <sys/machine.h>
#ifdef X86_PC

#include "kernel.h"

/* カーネルイメージ終端（boot/x86/kernel.ld が定義） */
IMPORT UB _kernel_end[];

/*
 * システムメモリ領域の下端/上端
 */
EXPORT void *knl_lowmem_top;
EXPORT void *knl_lowmem_limit;

/*
 * ハードウェアの起動時初期化
 *	boot/x86/main_mtk3.c が knl_main() を呼ぶ前に実行します。
 */
EXPORT void knl_startup_hw( void )
{
	knl_lowmem_top   = (void *)(((unsigned long)_kernel_end + 3) & ~0x3UL);
	knl_lowmem_limit = (void *)X86_SYSTEMAREA_END;
}

/*
 * ハードウェアの終了処理
 *	ベアメタルでは戻る先が無いため halt ループします。
 */
EXPORT void knl_shutdown_hw( void )
{
	for ( ;; ) {
		__asm__ volatile ("cli; hlt");
	}
}

/*
 * 再起動処理（キーボードコントローラ経由の CPU リセット）
 */
EXPORT ER knl_restart_hw( W mode )
{
	(void)mode;
	__asm__ volatile ("outb %0, %1" : : "a"((UB)0xFE), "Nd"((UH)0x64));
	for ( ;; ) {
		__asm__ volatile ("hlt");
	}
}

#endif /* X86_PC */
