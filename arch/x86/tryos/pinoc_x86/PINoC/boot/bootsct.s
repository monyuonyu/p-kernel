	.file	"bootsct.c"
#APP
	.code16gcc
	movw	$0x0400, 	%ax		
	movw	%ax,		%ss		
	movl	$0x0,		%eax	
	movl	%eax,		%esp	
	movl	%eax,		%ebp	
	
#NO_APP
	.text
.globl main16
	.type	main16, @function
main16:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$8, %esp
	call	io_load16
	call	io_debug16
#APP
# 26 "bootsct.c" 1
	popl	%ebx
# 0 "" 2
# 35 "bootsct.c" 1
	jmp	0x5000
# 0 "" 2
#NO_APP
	leave
	ret
	.size	main16, .-main16
	.ident	"GCC: (Ubuntu/Linaro 4.5.2-8ubuntu4) 4.5.2"
	.section	.note.GNU-stack,"",@progbits
