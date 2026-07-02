/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux AArch64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	offset.h
 *	TCB 内フィールドオフセット定義（アセンブラ用）
 *	（Linux AArch64 ユーザモードポート / LP64）
 *
 *	μT-Kernel 3.0 の TCB（kernel/knlinc/kernel.h）を LP64 で
 *	コンパイルしたときのオフセットです。値は手計算ではなく、
 *	cpu_cntl.c の _Static_assert（offsetof による検証）でビルド時に
 *	必ず照合されます。TCB のフィールド構成や config を変更した
 *	場合はビルドエラーで不一致が検出されます。
 *
 *	  offset  0: QUEUE  tskque   （ポインタ×2 = 16 バイト）
 *	  offset 16: ID     tskid
 *	  offset 24: void  *exinf
 *	  offset 32: ATR    tskatr
 *	  offset 40: FP     task     ← TCB_task（トランポリンが参照）
 *	  offset 48: CTXB   tskctxb  ← TCB_SSP（ディスパッチャが参照）
 */

#ifndef _SYSDEPEND_TARGET_OFFSET_
#define _SYSDEPEND_TARGET_OFFSET_

#define TCB_task	40	/* タスク起動アドレス（FP task） */
#define TCB_tskctxb	48	/* タスクコンテキストブロック */
#define CTXB_ssp	0	/* CTXB 内のシステムスタックポインタ */

#define TCB_SSP		( TCB_tskctxb + CTXB_ssp )

#endif /* _SYSDEPEND_TARGET_OFFSET_ */
