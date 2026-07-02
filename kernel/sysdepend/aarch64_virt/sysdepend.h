/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel AArch64 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdepend.h
 *	システム依存ローカル定義（AArch64 ベアメタルポート）
 *
 *	ディスパッチャ・EL1 例外ベクタ・IRQ 入口は既存の
 *	arch/aarch64/cpu_support.S を再利用します（TCB オフセットは
 *	本ポートの offset.h がインクルードパス優先で差し込まれる）。
 */

#ifndef _SYSDEPEND_TARGET_SYSDEPEND_
#define _SYSDEPEND_TARGET_SYSDEPEND_

/*
 * ディスパッチャ エントリポイント（arch/aarch64/cpu_support.S）
 */
IMPORT void knl_dispatch_entry(void);
IMPORT void knl_dispatch_to_schedtsk(void);

/*
 * 新規タスクの初回起動点（arch/aarch64/cpu_support.S）
 */
IMPORT void knl_task_entry_trampoline(void);

/*
 * タイマ割込みとカーネルタイマハンドラの橋渡し
 * （arch/aarch64/cpu_support.S — knl_taskindp を増減して呼ぶ）
 */
IMPORT void knl_timer_handler_startup(void);

/*
 * 割込みベクタテーブル（interrupt.c）
 *	cpu_support.S の IRQ 入口が GICC_IAR の INTID で引く。
 */
IMPORT FP knl_intvec[N_INTVEC];

/*
 * タスクコンテキストブロック
 */
typedef struct {
	void	*ssp;		/* システムスタックポインタ */
} CTXB;

#endif /* _SYSDEPEND_TARGET_SYSDEPEND_ */
