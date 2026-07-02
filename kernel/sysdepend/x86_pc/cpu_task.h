/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_task.h
 *	タスクコンテキストの初期化（x86 ベアメタルポート / ILP32）
 *
 *	休止状態タスクのスタックフレームレイアウト
 *	（ssp = 最下位アドレス）:
 *
 *	  ssp+ 0: ebx = 0
 *	  ssp+ 4: esi = 0
 *	  ssp+ 8: edi = 0
 *	  ssp+12: ebp = 0
 *	  ssp+16: （予約 = 0）
 *	  ssp+20: ret  = &knl_task_entry_trampoline ← dispatch.S の ret が pop
 *	  ssp+24: &tk_ext_tsk（タスク関数の戻り先）
 *	  ssp+28: stacd（第1引数）
 *	  ssp+32: exinf（第2引数）
 *	 ─── 合計 36 バイト ───
 *
 *	ssp+24〜32 は cdecl の呼び出しフレームそのもので、トランポリンが
 *	TCB_task へ jmp した時点で task(stacd, exinf) の引数と戻り先が
 *	整っている（micro T-Kernel 2.0 ポートと同一方式）。
 */

#ifndef _SYSDEPEND_TARGET_CPUTASK_
#define _SYSDEPEND_TARGET_CPUTASK_

#include "sysdepend.h"

/*
 * knl_setup_context が書き込むフレームサイズ
 */
#define DORMANT_STACK_SIZE	36

IMPORT void tk_ext_tsk( void );

/*
 * 休止状態タスクのコンテキスト初期化
 */
Inline void knl_setup_context( TCB *tcb )
{
	/* 4 バイト境界へ切り下げ（knl_Imalloc は 8 バイト整列を保証） */
	unsigned long base = ((unsigned long)tcb->isstack) & ~0x3UL;
	UW *ssp = (UW *)(base - DORMANT_STACK_SIZE);
	INT i;

	/* フレーム全体をゼロクリア（4 バイト単位 × 9 スロット） */
	for ( i = 0; i < DORMANT_STACK_SIZE / (INT)sizeof(UW); i++ ) {
		ssp[i] = 0;
	}

	/* 復帰先（offset 20 = index 5）にトランポリン、
	 * タスク関数の戻り先（offset 24 = index 6）に tk_ext_tsk */
	ssp[5] = (UW)(unsigned long)knl_task_entry_trampoline;
	ssp[6] = (UW)(unsigned long)tk_ext_tsk;

	tcb->tskctxb.ssp = ssp;
}

/*
 * タスク起動コードの設定
 *	stacd（offset 28 = index 7）と exinf（offset 32 = index 8）を
 *	cdecl の引数スロットへ書き込みます。
 */
Inline void knl_setup_stacd( TCB *tcb, INT stacd )
{
	UW *ssp = (UW *)tcb->tskctxb.ssp;

	ssp[7] = (UW)stacd;
	ssp[8] = (UW)(unsigned long)tcb->exinf;
}

/*
 * タスクコンテキストの後始末（本ポートでは不要）
 */
Inline void knl_cleanup_context( TCB *tcb )
{
	(void)tcb;
}

#endif /* _SYSDEPEND_TARGET_CPUTASK_ */
