	.h8300h
	.section .text
	.global _start
.type _start,@function
_start:
	mov.l	#_b_stack, sp
	jsr		@_init
