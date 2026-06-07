#include "intr.h"

	.h8300h
		.section .text

/* mov.l は32bit mov.w は16bitである */

/* GCCにおいて、										 */
/* er0 er1 er2 は不揮発性レジスタ							 */
/* er3 er4 er5 er7 は揮発性レジスタ						 */
/* 他のコンパイラでの安全を考えてすべて退避とする					 */

/* ソフトエラー */
		.global	_intr_softerr
		.type 	_intr_softerr,@function
_intr_softerr:
	mov.l	er6,@-er7					/* 汎用レジスタの退避 */
	mov.l	er5,@-er7
	mov.l	er4,@-er7
	mov.l	er3,@-er7
	mov.l	er2,@-er7
	mov.l	er1,@-er7
	mov.l	er0,@-er7

	mov.l	er7,er1						/* 第二引数にスタックポインタ	 	 */
	mov.l	#_i_stack,sp				/* 割り込みスタック 				 */
	mov.l	er1,@-er7					/* 古いスタックポインタを積む		 */
	mov.w	#SOFTVEC_TYPE_SOFTERR,r0	/* 第一引数に割り込みタイプを指定	 */
	jsr		@_interrupt					/* interrupt() 関数呼び出し	 */
	mov.l	@er7+,er1					/* 古いスタックポインタを復元		 */
	mov.l	er1,er7

	mov.l	@er7+,er0					/* 汎用レジスタの復帰 */
	mov.l	@er7+,er1
	mov.l	@er7+,er2
	mov.l	@er7+,er3
	mov.l	@er7+,er4
	mov.l	@er7+,er5
	mov.l	@er7+,er6
	rte									/* 復帰 */

/* システムコール */
		.global	_intr_syscall
		.type	_intr_syscall,@function
_intr_syscall:
	mov.l	er6,@-er7
	mov.l	er5,@-er7
	mov.l	er4,@-er7
	mov.l	er3,@-er7
	mov.l	er2,@-er7
	mov.l	er1,@-er7
	mov.l	er0,@-er7

	mov.l	er7,er1
	mov.l	#_i_stack,sp
	mov.l	er1,@-er7
	mov.w	#SOFTVEC_TYPE_SYSTEM,r0
	jsr		@_interrupt
	mov.l	@er7+,er1
	mov.l	er1,er7

	mov.l	@er7+,er0
	mov.l	@er7+,er1
	mov.l	@er7+,er2
	mov.l	@er7+,er3
	mov.l	@er7+,er4
	mov.l	@er7+,er5
	mov.l	@er7+,er6
	rte

/* シリアル割り込み */
		.global	_intr_serial
		.type	_intr_serial,@function
_intr_serial:
	mov.l	er6,@-er7
	mov.l	er5,@-er7
	mov.l	er4,@-er7
	mov.l	er3,@-er7
	mov.l	er2,@-er7
	mov.l	er1,@-er7
	mov.l	er0,@-er7

	mov.l	er7,er1
	mov.l	#_i_stack,sp
	mov.l	er1,@-er7
	mov.w	#SOFTVEC_TYPE_SERIAL,r0
	jsr		@_interrupt
	mov.l	@er7+,er1
	mov.l	er1,er7

	mov.l	@er7+,er0
	mov.l	@er7+,er1
	mov.l	@er7+,er2
	mov.l	@er7+,er3
	mov.l	@er7+,er4
	mov.l	@er7+,er5
	mov.l	@er7+,er6
	rte


