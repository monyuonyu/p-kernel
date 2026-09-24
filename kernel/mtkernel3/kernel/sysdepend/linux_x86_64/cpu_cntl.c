/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux x86-64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_cntl.c
 *	CPU 制御（Linux x86-64 ユーザモードポート）
 *
 *	本ポートの CPU 依存グローバル変数の実体と、offset.h の
 *	オフセット定数のビルド時検証を行います。
 */

#include <sys/machine.h>
#ifdef LINUX_X86_64

#include "kernel.h"
#include "offset.h"

#include <stddef.h>	/* offsetof */

/*
 * タスク独立部（擬似割込みハンドラ実行中）ネストカウンタ
 *	dispatch.S の knl_timer_handler_startup が inc/dec し、
 *	cpu_status.h の knl_isTaskIndependent() が参照します。
 */
EXPORT W knl_taskindp = 0;

/*
 * 強制ディスパッチ用の一時スタック
 *	tk_exd_tsk は knl_del_tsk で自分のシステムスタックを解放してから
 *	knl_force_dispatch する。dispatch.S の knl_dispatch_to_schedtsk は
 *	上流の armv7m / rxv2 と同じく、最初にここへ乗り換える。乗り換え
 *	ないと、実行可能タスクが無いときの idle が解放済みの領域の上で
 *	走る（D5-k）。
 */
EXPORT UB knl_tmp_stack[KNL_TMP_STACK_SZ] __attribute__((aligned(16)));

/*
 * idle が解放済みの Imalloc 領域の上で走っていないかの検査
 *	dispatch.S の .Lidle が knl_idle_wait の前に rsp を渡して呼ぶ。
 *	AreaQue をたどり、sp が使用中でない領域の中にあれば abort する。
 *	一時スタックへの乗り換えが外れたときに黙って走り続けないための
 *	番人（tests/host の idle_freed_stack がこれで赤くなる）。
 */
#include "../../tkernel/memory.h"

/* libc のヘッダはカーネルのヘッダと衝突する（MB_LEN_MAX）ので、
 * 使う2つだけを宣言する。 */
extern int  dprintf( int fd, const char *fmt, ... );
extern void abort( void ) __attribute__((noreturn));

EXPORT void knl_idle_sp_check( void *sp )
{
	QUEUE	*top, *aq;
	VB	*p = (VB*)sp;

	if ( knl_imacb == NULL ) {
		return;
	}
	top = &(AlignIMACB(knl_imacb)->areaque);
	for ( aq = top->next; aq != top; aq = aq->next ) {
		if ( aq->next <= aq ) {
			break;	/* 末尾の境界、または壊れたキュー */
		}
		if ( chkAreaFlag(aq, AREA_USE) ) {
			continue;
		}
		if ( p >= (VB*)(aq + 1) && p < (VB*)aq->next ) {
			dprintf(2, "[knl] FATAL: idle is running on a freed "
				"Imalloc area (sp=%p area=%p..%p)\n",
				sp, (void*)(aq + 1), (void*)aq->next);
			abort();
		}
	}
}

/*
 * offset.h（アセンブラ用オフセット定数）と実際の TCB レイアウトの
 * ビルド時照合。TCB のフィールド構成・config を変更してズレた場合、
 * ここでコンパイルエラーになります（実行時の暴走より先に検出）。
 */
_Static_assert( offsetof(TCB, task)    == TCB_task,
		"offset.h の TCB_task が TCB 実レイアウトと不一致" );
_Static_assert( offsetof(TCB, tskctxb) == TCB_tskctxb,
		"offset.h の TCB_tskctxb が TCB 実レイアウトと不一致" );
_Static_assert( offsetof(CTXB, ssp)    == CTXB_ssp,
		"offset.h の CTXB_ssp が CTXB 実レイアウトと不一致" );

#endif /* LINUX_X86_64 */
