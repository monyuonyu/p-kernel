/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Windows x86-64 ネイティブポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_task.h
 *	タスクコンテキストの初期化（Windows Fiber ポート）
 *
 *	Linux x86-64 ポートは休止タスクのコンテキストをタスク自身の
 *	スタック上の 64 バイトフレームに構築し、dispatch.S の ret が
 *	トランポリンへ着地する方式でした。Windows ポートでは各タスクを
 *	1 本の Windows Fiber に対応させます:
 *
 *	  - Fiber がタスクのスタック・全レジスタ（XMM 含む）を所有する
 *	    ため、コア側が確保する isstack は使いません（サイズヒントの
 *	    tcb->sstksz だけを CreateFiber に渡します）。
 *	  - Fiber ハンドルは tcb->tskctxb.ssp（void* スロット）に格納。
 *	  - Fiber は最初のディスパッチ時に遅延生成します（stacd 確定後）。
 *	  - タスク再起動（tk_ext_tsk → make_dormant → 再 tk_sta_tsk）では
 *	    旧 Fiber を「墓場」へ退避し、次のディスパッチで新 Fiber を
 *	    生成します（Fiber は自身の resume 位置を書き換えられないため）。
 *
 *	実体はすべて dispatch.c にあります。
 */

#ifndef _SYSDEPEND_TARGET_CPUTASK_
#define _SYSDEPEND_TARGET_CPUTASK_

#include "sysdepend.h"

/*
 * task_manage.c が tk_ext_tsk で make_dormant 用に確保するダミー
 * スタック量。Fiber ポートでは必須ではありませんが、Linux 版と挙動を
 * 揃えるため同値を定義します（無害）。
 */
#define DORMANT_STACK_SIZE	64

/* dispatch.c 実体 */
IMPORT void knl_win_setup_context( TCB *tcb );
IMPORT void knl_win_setup_stacd( TCB *tcb, INT stacd );
IMPORT void knl_win_cleanup_context( TCB *tcb );

/*
 * 休止状態タスクのコンテキスト初期化
 *	既存 Fiber があれば墓場へ退避し、tskctxb.ssp を NULL（未生成）に
 *	戻します。実生成は初回ディスパッチまで遅延します。
 */
Inline void knl_setup_context( TCB *tcb )
{
	knl_win_setup_context(tcb);
}

/*
 * タスク起動コードの設定
 *	stacd をタスクごとの側テーブルへ退避します（Fiber エントリが読む）。
 */
Inline void knl_setup_stacd( TCB *tcb, INT stacd )
{
	knl_win_setup_stacd(tcb, stacd);
}

/*
 * タスクコンテキストの後始末（タスク削除時）
 *	Fiber が残っていれば墓場へ退避します。
 */
Inline void knl_cleanup_context( TCB *tcb )
{
	knl_win_cleanup_context(tcb);
}

#endif /* _SYSDEPEND_TARGET_CPUTASK_ */
