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

/*
 * 割込みの出口での遅延ディスパッチ（RNG0-BUSY-TASK-STALLS-DISPATCH の修正、
 * 2026-09-28。gap-ledger・ROADMAP 0-1）
 *	boot/x86/idt.c の irq_handler の最後（EOI の後、iretq の前）から呼ばれる。
 *	割込みの中の END_CRITICAL_SECTION は、タスク独立部なのでディスパッチ
 *	しない（μT-Kernel の決まりどおり）。そのため、カーネルを呼ばずに回り
 *	続けるタスクがあると、時限待ちが明けた高い優先度のタスクへ永久に
 *	切り替わらなかった。ここで、いちばん外側の割込みからタスクへ戻る直前に
 *	切り替える。ほかの μT-Kernel のポートが割込みの出口（ret_int・PendSV）で
 *	していることと同じ。
 *	割込みフレームは割り込まれたタスクのスタック（ring3 なら TSS.RSP0 の
 *	カーネルスタック）にあるので、knl_dispatch_entry はそれごとタスクの
 *	休止フレームとして残す。再開するとここへ戻り、irq_call32_stub から
 *	iretq で割り込まれた場所へ帰る。
 *	条件: 割込みの入れ子の外側（knl_taskindp == 0）、ディスパッチ禁止でない
 *	（tk_dis_dsp 中や、idle の hlt を含むディスパッチャの中ではない）、
 *	タスクを割り込んだ（ctxtsk != NULL）、切り替えが要る。
 *	割込みゲートなので IF=0 で来る。knl_dispatch_entry は再開時に sti して
 *	戻るので、iretq までの残りを割込み禁止に戻す。
 *	-DPK_RNG0_NO_EXIT_DISPATCH は修正前の振る舞い（試験の陰性コントロール用）。
 */
EXPORT void knl_irq_exit_dispatch( void )
{
#ifndef PK_RNG0_NO_EXIT_DISPATCH
	if ( knl_taskindp == 0 && !knl_dispatch_disabled
	  && knl_ctxtsk != NULL && knl_ctxtsk != knl_schedtsk ) {
		knl_dispatch();
		__asm__ volatile ("cli" ::: "memory");
	}
#endif
}

#endif /* X86_PC */
