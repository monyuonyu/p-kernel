/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdepend.h
 *	システム依存ローカル定義（x86 ベアメタルポート）
 */

#ifndef _SYSDEPEND_TARGET_SYSDEPEND_
#define _SYSDEPEND_TARGET_SYSDEPEND_

/*
 * ディスパッチャ エントリポイント（dispatch.S）
 */
IMPORT void knl_dispatch_entry(void);
IMPORT void knl_dispatch_to_schedtsk(void);

/*
 * 新規タスクの初回起動点（dispatch.S）
 */
IMPORT void knl_task_entry_trampoline(void);

/*
 * PIT 割込み（IRQ0）とカーネルタイマハンドラの橋渡し（dispatch.S）
 */
IMPORT void knl_timer_handler_startup(void);

/*
 * タスクコンテキストブロック
 *	保存対象はシステムスタックポインタのみ。レジスタ本体は
 *	タスク自身のスタック上のフレーム（dispatch.S 参照）に退避。
 */
typedef struct {
	void	*ssp;		/* システムスタックポインタ */
} CTXB;

#endif /* _SYSDEPEND_TARGET_SYSDEPEND_ */
