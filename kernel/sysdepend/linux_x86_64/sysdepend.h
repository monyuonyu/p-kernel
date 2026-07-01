/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux x86-64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdepend.h
 *	システム依存ローカル定義（Linux x86-64 ユーザモードポート）
 */

#ifndef _SYSDEPEND_TARGET_SYSDEPEND_
#define _SYSDEPEND_TARGET_SYSDEPEND_

/*
 * ディスパッチャ エントリポイント（dispatch.S）
 *	knl_dispatch_entry       : 現タスクのコンテキストを保存して切替え
 *	knl_dispatch_to_schedtsk : 現コンテキストを破棄して切替え（強制）
 */
IMPORT void knl_dispatch_entry(void);
IMPORT void knl_dispatch_to_schedtsk(void);

/*
 * 新規タスクの初回起動点（dispatch.S）
 */
IMPORT void knl_task_entry_trampoline(void);

/*
 * SIGALRM ハンドラとカーネルタイマハンドラの橋渡し（dispatch.S）
 */
IMPORT void knl_timer_handler_startup(void);

/*
 * アイドル待ち（arch/linux/x86_64/preempt.c）
 *	実行可能タスクが無いとき、次の SIGALRM tick まで sigsuspend で
 *	眠ります（ビジーループしない）。
 */
IMPORT void knl_idle_wait(void);

/*
 * タスクコンテキストブロック
 *	保存対象はシステムスタックポインタのみ。レジスタ本体はタスク
 *	自身のスタック上のフレーム（dispatch.S 参照）に退避されます。
 */
typedef struct {
	void	*ssp;		/* システムスタックポインタ */
} CTXB;

#endif /* _SYSDEPEND_TARGET_SYSDEPEND_ */
