/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	offset.h
 *	TCB 内フィールドオフセット定義（アセンブラ用）
 *	（x86 ベアメタルポート / ILP32）
 *
 *	μT-Kernel 3.0 の TCB（kernel/knlinc/kernel.h）を -m32（ILP32）で
 *	コンパイルしたときのオフセットです。値は cpu_cntl.c の
 *	_Static_assert（offsetof による検証）でビルド時に必ず照合されます。
 *
 *	  offset  0: QUEUE  tskque   （ポインタ×2 = 8 バイト）
 *	  offset  8: ID     tskid
 *	  offset 12: void  *exinf
 *	  offset 16: ATR    tskatr
 *	  offset 20: FP     task     ← TCB_task
 *	  offset 24: CTXB   tskctxb  ← TCB_SSP
 */

#ifndef _SYSDEPEND_TARGET_OFFSET_
#define _SYSDEPEND_TARGET_OFFSET_

#define TCB_task	20	/* タスク起動アドレス（FP task） */
#define TCB_tskctxb	24	/* タスクコンテキストブロック */
#define CTXB_ssp	0	/* CTXB 内のシステムスタックポインタ */

#define TCB_SSP		( TCB_tskctxb + CTXB_ssp )

/*
 * タスクのシステムスタック初期値（void *isstack）のオフセット
 *	ディスパッチャが TSS.RSP0（ring3→ring0 遷移時のカーネルスタック）
 *	を毎ディスパッチ更新するために参照する。config 依存フィールド
 *	（WINFO 等）より後ろにあるため値は config に敏感 —
 *	cpu_cntl.c の _Static_assert で必ず照合される。
 */
#define TCB_isstack	100

#endif /* _SYSDEPEND_TARGET_OFFSET_ */
