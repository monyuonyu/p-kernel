/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel AArch64 ベアメタルポート（QEMU virt）
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_task.h
 *	タスクコンテキストの初期化（AArch64 ベアメタルポート（QEMU virt））
 *
 *	休止状態タスクのスタックフレームレイアウト
 *	（ssp = 最下位アドレス、上方向に伸びる）:
 *
 *	  ssp+  0: x19        ← stacd（knl_setup_stacd が上書き）
 *	  ssp+  8: x20        ← exinf（knl_setup_stacd が上書き）
 *	  ssp+ 16: x21           ssp+ 56: x26        ssp+ 96: （予約 = 0）
 *	  ssp+ 24: x22           ssp+ 64: x27        ssp+104: （パディング）
 *	  ssp+ 32: x23           ssp+ 72: x28
 *	  ssp+ 40: x24           ssp+ 80: x29 (fp)
 *	  ssp+ 48: x25           ssp+ 88: x30 (lr) ← 復帰先。新規タスクでは
 *	                                             トランポリン
 *	 ─── 合計 112 バイト（16 バイト整列）───
 *
 *	AAPCS64 の callee-saved レジスタは x19〜x28 + fp + lr の 12 本。
 *	ディスパッチャ（dispatch.S）は x30 スロットの値へ ret で復帰します。
 */

#ifndef _SYSDEPEND_TARGET_CPUTASK_
#define _SYSDEPEND_TARGET_CPUTASK_

#include "sysdepend.h"

/*
 * knl_setup_context が書き込むフレームサイズ
 *	task_manage.c はこの分のスタック余裕を確保して休止処理を行います。
 */
#define DORMANT_STACK_SIZE	112

/*
 * 休止状態タスクのコンテキスト初期化
 *	タスク生成時・終了時に呼ばれ、次回起動時にトランポリンへ
 *	着地するスタックフレームを構築します。
 */
Inline void knl_setup_context( TCB *tcb )
{
	/* AArch64 は sp 経由のすべてのロード/ストアで 16 バイト整列を
	 * 要求する。knl_Imalloc は 8 バイト整列しか保証しないため、
	 * スタックトップを 16 バイト境界へ切り下げてからフレームを積む。 */
	unsigned long base = ((unsigned long)tcb->isstack) & ~0xFUL;
	UD *ssp = (UD *)(base - DORMANT_STACK_SIZE);
	INT i;

	/* フレーム全体をゼロクリア（8 バイト単位 × 14 スロット） */
	for ( i = 0; i < DORMANT_STACK_SIZE / (INT)sizeof(UD); i++ ) {
		ssp[i] = 0;
	}

	/* x30 スロット（offset 88 = index 11）にトランポリン。
	 * 初回ディスパッチ時に dispatch.S の ret がここへ着地する。 */
	((void **)ssp)[11] = (void *)knl_task_entry_trampoline;

	tcb->tskctxb.ssp = ssp;
}

/*
 * タスク起動コードの設定
 *	x19 スロット（index 0）= stacd、x20 スロット（index 1）= exinf。
 *	トランポリンがこれらを x0/x1 に移してタスク関数を呼びます。
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
