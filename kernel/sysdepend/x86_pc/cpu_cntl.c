/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_cntl.c
 *	CPU 制御（x86 ベアメタルポート）
 *
 *	CPU 依存グローバル変数の実体、offset.h のビルド時検証、
 *	ディスパッチ前の TCB poison チェックを提供します。
 */

#include <sys/machine.h>
#ifdef X86_PC

#include "kernel.h"
#include "../../tkernel/task.h"	/* TSTAT（TS_NONEXIST）の定義 */
#include "offset.h"

#include <stddef.h>	/* offsetof */

/*
 * タスク独立部（割込みハンドラ実行中）ネストカウンタ
 */
EXPORT W knl_taskindp = 0;

/*
 * offset.h（アセンブラ用オフセット定数）と実際の TCB レイアウトの
 * ビルド時照合。ズレはコンパイルエラーとして検出されます。
 */
_Static_assert( offsetof(TCB, task)    == TCB_task,
		"offset.h の TCB_task が TCB 実レイアウトと不一致" );
_Static_assert( offsetof(TCB, tskctxb) == TCB_tskctxb,
		"offset.h の TCB_tskctxb が TCB 実レイアウトと不一致" );
_Static_assert( offsetof(TCB, isstack) == TCB_isstack,
		"offset.h の TCB_isstack が TCB 実レイアウトと不一致" );
_Static_assert( offsetof(CTXB, ssp)    == CTXB_ssp,
		"offset.h の CTXB_ssp が CTXB 実レイアウトと不一致" );

/*
 * 早期出力（boot/x86 のシリアル直叩き。sio_send_frame でも可） */
IMPORT void sio_send_frame(const UB *buf, INT size);

/*
 * ディスパッチ直前の TCB poison チェック（dispatch.S から呼ばれる）
 *	tk_del_tsk で FreeQue に返却された TCB は state == TS_NONEXIST。
 *	kill/heal churn がそのような TCB を knl_schedtsk に残した場合、
 *	そのまま切り替えると再利用済みメモリへ ret して garbage-PC #PF に
 *	なる。ここで決定的・grep 可能な halt に変える。
 */
EXPORT void knl_dispatch_poison_check( TCB *tcb )
{
	static const char msg[] = "\r\n[dispatch] POISON: freed TCB in schedtsk — halt\r\n";

	if ( tcb->state != TS_NONEXIST ) {
		return;			/* 正常 — 何もしない */
	}

	sio_send_frame((const UB *)msg, (INT)sizeof(msg) - 1);
	for ( ;; ) {
		__asm__ volatile ("cli; hlt");
	}
}

#endif /* X86_PC */
