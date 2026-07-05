/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Windows x86-64 ネイティブポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdepend.h
 *	システム依存ローカル定義（Windows x86-64 ネイティブポート）
 */

#ifndef _SYSDEPEND_TARGET_SYSDEPEND_
#define _SYSDEPEND_TARGET_SYSDEPEND_

/*
 * ディスパッチャ エントリポイント（dispatch.c, Fiber ベース）
 *	knl_dispatch_entry       : 現タスクのコンテキストを保存して切替え
 *	knl_dispatch_to_schedtsk : 現コンテキストを破棄して切替え（強制）
 */
IMPORT void knl_dispatch_entry(void);
IMPORT void knl_dispatch_to_schedtsk(void);

/*
 * 新規タスクの初回起動点（dispatch.c の Fiber エントリ）
 *	Linux 版はアセンブラのトランポリンだが、Fiber ポートでは
 *	CreateFiber に渡す C 関数が同じ役割を果たす。互換のためシンボルを
 *	残す（実体は dispatch.c）。
 */
IMPORT void knl_task_entry_trampoline(void);

/*
 * タイマ tick の駆動（dispatch.c）
 *	Linux 版は SIGALRM ハンドラから呼ばれるが、Windows v1 は協調
 *	スケジューラのため、安全点（アイドル/ディスパッチ）から
 *	QueryPerformanceCounter に追従して呼ばれる。
 */
IMPORT void knl_timer_handler_startup(void);

/*
 * アイドル待ち（dispatch.c）
 *	実行可能タスクが無いとき、短く Sleep して経過分の tick をポンプし、
 *	schedtsk を再確認します（ビジーループしない）。
 */
IMPORT void knl_idle_wait(void);

/*
 * タスクコンテキストブロック
 *	Fiber ポートでは保存対象は Fiber ハンドル（LPVOID）のみ。レジスタ
 *	本体・スタックは Windows Fiber が管理します。ptr スロットに Fiber
 *	ハンドルを格納します（Linux 版の ssp スロットを流用）。
 */
typedef struct {
	void	*ssp;		/* Fiber ハンドル（CreateFiber の戻り値） */
} CTXB;

#endif /* _SYSDEPEND_TARGET_SYSDEPEND_ */
