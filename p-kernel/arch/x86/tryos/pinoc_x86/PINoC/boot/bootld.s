	.file	"bootld.c"
	.comm	EAX,4,4
	.comm	ECX,4,4
	.comm	EDX,4,4
	.comm	ESI,4,4
	.comm	EDI,4,4
	.comm	graphicmode,16,4
#APP
	.code16gcc
#NO_APP
	.text
.globl main16
	.type	main16, @function
main16:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
	call	io_A20GATE
	movl	$36872, %eax
	movl	$65535, (%eax)
	movl	$13605376, 4(%eax)
	movl	$36880, %eax
	movl	$65535, (%eax)
	movl	$13603328, 4(%eax)
	movl	$36888, %eax
	movl	$65535, (%eax)
	movl	$13605376, 4(%eax)
	movl	$36864, 4(%esp)
	movl	$255, (%esp)
	call	LGDT16
	movl	$34048, 4(%esp)
	movl	$255, (%esp)
	call	LIDT16
	movl	$753664, %eax
	movb	$65, (%eax)
	movl	$753665, %eax
	movb	$4, (%eax)
	movl	$753666, %eax
	movb	$66, (%eax)
	movl	$753667, %eax
	movb	$4, (%eax)
	call	io_PROTECTMODE
	leave
	ret
	.size	main16, .-main16
	.ident	"GCC: (Ubuntu/Linaro 4.5.2-8ubuntu4) 4.5.2"
	.section	.note.GNU-stack,"",@progbits
