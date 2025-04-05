
	.h8300h
		.section .text

		.global	_dispatch
		.type 	_dispatch,@function
_dispatch:
	mov.l	@er0,er7	/* 引数のスタックポインタをセット (関数呼び出しではer0に現在のスタックポインタが引数として贈られる)*/
	mov.l	@er7+,er0
	mov.l	@er7+,er1
	mov.l	@er7+,er2
	mov.l	@er7+,er3
	mov.l	@er7+,er4
	mov.l	@er7+,er5
	mov.l	@er7+,er6
	rte					/* 復帰 */
