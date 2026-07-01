/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux x86-64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_task.h
 *	タスクコンテキストの初期化（Linux x86-64 ユーザモードポート）
 *
 *	休止状態タスクのスタックフレームレイアウト
 *	（ssp = 最下位アドレス、上方向に伸びる）:
 *
 *	  ssp+ 0: r12        ← stacd（knl_setup_stacd が上書き）
 *	  ssp+ 8: r13        ← exinf（knl_setup_stacd が上書き）
 *	  ssp+16: r14 = 0
 *	  ssp+24: r15 = 0
 *	  ssp+32: rbx = 0
 *	  ssp+40: rbp = 0
 *	  ssp+48: （予約 = 0）
 *	  ssp+56: rip = &knl_task_entry_trampoline  ← dispatch.S の ret が pop
 *	 ─── 合計 64 バイト（16 バイト整列）───
 *
 *	System V AMD64 ABI では callee-saved レジスタは
 *	r12/r13/r14/r15/rbx/rbp の 6 本のみです。offset+56 の
 *	「戻りアドレス」スロットをディスパッチャの ret が pop し、
 *	新規タスクではトランポリンへ着地します。
 */

#ifndef _SYSDEPEND_TARGET_CPUTASK_
#define _SYSDEPEND_TARGET_CPUTASK_

#include "sysdepend.h"

/*
 * knl_setup_context が書き込むフレームサイズ
 *	task_manage.c はこの分のスタック余裕を確保して休止処理を行います。
 */
#define DORMANT_STACK_SIZE	64

/*
 * 休止状態タスクのコンテキスト初期化
 *	タスク生成時・終了時に呼ばれ、次回起動時にトランポリンへ
 *	着地するスタックフレームを構築します。
 */
Inline void knl_setup_context( TCB *tcb )
{
	/* スタックトップを 16 バイト境界へ切り下げる。
	 * knl_Imalloc は 8 バイト整列しか保証しないため、そのまま
	 * (isstack - 64) とすると 8 mod 16 の位置になり得る。
	 * System V AMD64 ABI は call 直前に 16 バイト整列を要求する
	 * ので、XMM 引数を使う関数を呼ぶタスクはこれが無いと落ちる。 */
	unsigned long base = ((unsigned long)tcb->isstack) & ~0xFUL;
	UD *ssp = (UD *)(base - DORMANT_STACK_SIZE);
	INT i;

	/* フレーム全体をゼロクリア（8 バイト単位 × 8 スロット） */
	for ( i = 0; i < DORMANT_STACK_SIZE / (INT)sizeof(UD); i++ ) {
		ssp[i] = 0;
	}

	/* 「戻りアドレス」スロット（offset 56 = index 7）にトランポリン。
	 * 初回ディスパッチ時に dispatch.S の ret がこれを pop する。 */
	((void **)ssp)[7] = (void *)knl_task_entry_trampoline;

	tcb->tskctxb.ssp = ssp;
}

/*
 * タスク起動コードの設定
 *	r12 スロット（index 0）= stacd、r13 スロット（index 1）= exinf。
 *	トランポリンがこれらを rdi/rsi に移してタスク関数を呼びます。
 */
Inline void knl_setup_stacd( TCB *tcb, INT stacd )
{
	void **ssp = (void **)tcb->tskctxb.ssp;

	ssp[0] = (void *)(long)stacd;
	ssp[1] = tcb->exinf;
}

/*
 * タスクコンテキストの後始末（本ポートでは不要）
 */
Inline void knl_cleanup_context( TCB *tcb )
{
	(void)tcb;
}

#endif /* _SYSDEPEND_TARGET_CPUTASK_ */
