	.file	"main.c"
	.comm	EAX,4,4
	.comm	ECX,4,4
	.comm	EDX,4,4
	.comm	ESI,4,4
	.comm	EDI,4,4
	.comm	graphicmode,16,4
#APP
	.code32
	jmp kernel_ent
#NO_APP
	.text
	.globl	main32
	.type	main32, @function
main32:
.LFB0:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$24, %esp
#APP
# 47 "main.c" 1
	kernel_ent: 			
	movl	$0x40000,	%esp 
	
# 0 "" 2
#NO_APP
	call	io_fin32
#APP
# 58 "main.c" 1
	movw	$0x10,		%ax		
	movw	%ax,		%ds		
	
# 0 "" 2
# 64 "main.c" 1
	movw	$0x10,		%ax		
	movw	%ax,		%ss		
	
# 0 "" 2
#NO_APP
	movl	$753668, %eax
	movb	$67, (%eax)
	movl	$753669, %eax
	movb	$4, (%eax)
	movl	$38144, %eax
	movb	$-12, (%eax)
	movl	$38145, %eax
	movw	$-533, (%eax)
	movl	$34048, -12(%ebp)
	call	io_fin32
	movl	$0, -16(%ebp)
	jmp	.L2
.L3:
	movl	-16(%ebp), %eax
	sall	$3, %eax
	addl	-12(%ebp), %eax
	movl	$int_keybord, %edx
	movw	%dx, (%eax)
	movl	-16(%ebp), %eax
	sall	$3, %eax
	addl	-12(%ebp), %eax
	movw	$8, 2(%eax)
	movl	-16(%ebp), %eax
	sall	$3, %eax
	addl	-12(%ebp), %eax
	movb	$0, 4(%eax)
	movl	-16(%ebp), %eax
	sall	$3, %eax
	addl	-12(%ebp), %eax
	movzbl	5(%eax), %edx
	andl	$-16, %edx
	orl	$14, %edx
	movb	%dl, 5(%eax)
	movl	-16(%ebp), %eax
	sall	$3, %eax
	addl	-12(%ebp), %eax
	movzbl	5(%eax), %edx
	andl	$-17, %edx
	movb	%dl, 5(%eax)
	movl	-16(%ebp), %eax
	sall	$3, %eax
	addl	-12(%ebp), %eax
	movzbl	5(%eax), %edx
	andl	$-97, %edx
	movb	%dl, 5(%eax)
	movl	-16(%ebp), %eax
	sall	$3, %eax
	addl	-12(%ebp), %eax
	movzbl	5(%eax), %edx
	orl	$-128, %edx
	movb	%dl, 5(%eax)
	movl	-16(%ebp), %eax
	sall	$3, %eax
	addl	-12(%ebp), %eax
	movl	$int_keybord, %edx
	shrl	$16, %edx
	movw	%dx, 6(%eax)
	addl	$1, -16(%ebp)
.L2:
	cmpl	$255, -16(%ebp)
	jle	.L3
	call	io_fin32
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE0:
	.size	main32, .-main32
	.globl	test
	.type	test, @function
test:
.LFB1:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	pushl	%edi
	pushl	%ebx
	subl	$288, %esp
	movl	%gs:20, %eax
	movl	%eax, -12(%ebp)
	xorl	%eax, %eax
	leal	-267(%ebp), %edx
	movl	$255, %ebx
	.cfi_offset 3, -16
	.cfi_offset 7, -12
	movl	$0, %eax
	movl	%edx, %ecx
	andl	$1, %ecx
	testl	%ecx, %ecx
	je	.L5
	movb	%al, (%edx)
	addl	$1, %edx
	subl	$1, %ebx
.L5:
	movl	%edx, %ecx
	andl	$2, %ecx
	testl	%ecx, %ecx
	je	.L6
	movw	%ax, (%edx)
	addl	$2, %edx
	subl	$2, %ebx
.L6:
	movl	%ebx, %ecx
	shrl	$2, %ecx
	movl	%edx, %edi
	rep stosl
	movl	%edi, %edx
	movl	%ebx, %ecx
	andl	$2, %ecx
	testl	%ecx, %ecx
	je	.L7
	movw	%ax, (%edx)
	addl	$2, %edx
.L7:
	movl	%ebx, %ecx
	andl	$1, %ecx
	testl	%ecx, %ecx
	je	.L8
	movb	%al, (%edx)
	addl	$1, %edx
.L8:
	movb	$65, -237(%ebp)
	movb	$66, -219(%ebp)
	movb	$67, -221(%ebp)
	movl	$97, 4(%esp)
	movl	$32, (%esp)
	call	io_out8
	movl	$96, (%esp)
	call	io_in8
	movb	%al, -268(%ebp)
	movl	$753670, %eax
	movsbl	-268(%ebp), %edx
	movzbl	-267(%ebp,%edx), %edx
	movb	%dl, (%eax)
	movl	$753671, %eax
	movb	$4, (%eax)
	movl	-12(%ebp), %eax
	xorl	%gs:20, %eax
	je	.L9
	call	__stack_chk_fail
.L9:
	addl	$288, %esp
	popl	%ebx
	.cfi_restore 3
	popl	%edi
	.cfi_restore 7
	popl	%ebp
	.cfi_def_cfa 4, 4
	.cfi_restore 5
	ret
	.cfi_endproc
.LFE1:
	.size	test, .-test
#APP
	int_damy1:		
	movb $'1', (0xb8008) 
	movb $0x04, (0xb8009) 
	popl %eax 
	iret 
	
	int_damy2:		
	movb $'2', (0xb8008) 
	movb $0x04, (0xb8009) 
	popl %eax 
	iret 
	
	int_damy3:		
	movb $'3', (0xb8008) 
	movb $0x04, (0xb8009) 
	popl %eax 
	iret 
	
	int_damy4:		
	movb $'4', (0xb8008) 
	movb $0x04, (0xb8009) 
	popl %eax 
	iret 
	
	int_damy5:		
	movb $'5', (0xb8008) 
	movb $0x04, (0xb8009) 
	popl %eax 
	iret 
	
	int_damy6:		
	movb $'6', (0xb8008) 
	movb $0x04, (0xb8009) 
	popl %eax 
	iret 
	
	int_damy7:		
	movb $'7', (0xb8008) 
	movb $0x04, (0xb8009) 
	popl %eax 
	iret 
	
	int_damy8:		
	movb $'8', (0xb8008) 
	movb $0x04, (0xb8009) 
	popl %eax 
	iret 
	
	int_damy9:		
	movb $'9', (0xb8008) 
	movb $0x04, (0xb8009) 
	popl %eax 
	iret 
	
	int_damya:		
	movb $'a', (0xb8008) 
	movb $0x04, (0xb8009) 
	popl %eax 
	iret 
	
	int_damyb:		
	movb $'b', (0xb8008) 
	movb $0x04, (0xb8009) 
	popl %eax 
	iret 
	
	int_keybord:		
	pushl	%ds			
	pushl	%es			
	pushal			
	movl	%ss, %eax		
	movl	%eax, %ds		
	movl	%eax, %es		
	aiueo2:	
	hlt		
	jmp aiueo2	
	call	test		
	pushl $0xFFFFFFFF		
	popl 	%eax		
	popal				
	popl	%es			
	popl	%ds			
	popl %eax		
	iret				
	
	.ident	"GCC: (Ubuntu/Linaro 4.6.1-9ubuntu3) 4.6.1"
	.section	.note.GNU-stack,"",@progbits
