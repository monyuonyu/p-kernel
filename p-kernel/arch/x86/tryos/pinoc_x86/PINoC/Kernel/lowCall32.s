	.file	"lowCall32.c"
	.comm	EAX,4,4
	.comm	ECX,4,4
	.comm	EDX,4,4
	.comm	ESI,4,4
	.comm	EDI,4,4
	.comm	graphicmode,16,4
	.text
	.globl	memmove
	.type	memmove, @function
memmove:
.LFB0:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$16, %esp
	movl	8(%ebp), %eax
	movl	%eax, -8(%ebp)
	movl	12(%ebp), %eax
	movl	%eax, -4(%ebp)
	movl	$0, -12(%ebp)
	jmp	.L2
.L3:
	movl	-12(%ebp), %eax
	addl	-8(%ebp), %eax
	movl	-12(%ebp), %edx
	addl	-4(%ebp), %edx
	movzbl	(%edx), %edx
	movb	%dl, (%eax)
	addl	$1, -12(%ebp)
.L2:
	movl	-12(%ebp), %eax
	cmpl	16(%ebp), %eax
	jb	.L3
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE0:
	.size	memmove, .-memmove
	.globl	memset
	.type	memset, @function
memset:
.LFB1:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$16, %esp
	movl	8(%ebp), %eax
	movl	%eax, -4(%ebp)
	movl	$0, -8(%ebp)
	jmp	.L5
.L6:
	movl	-8(%ebp), %eax
	addl	-4(%ebp), %eax
	movl	12(%ebp), %edx
	movb	%dl, (%eax)
	addl	$1, -8(%ebp)
.L5:
	movl	-8(%ebp), %eax
	cmpl	16(%ebp), %eax
	jb	.L6
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE1:
	.size	memset, .-memset
#APP
	.code32
	.globl io_hlt32				
	.type	io_hlt32, @function	
	io_hlt32:						
	hlt    						
	ret							
	
	.globl io_fin32				
	.type	io_fin32, @function	
	io_fin32:						
	movl $12345678, %eax		
	nop		
	hlt							
	jmp io_fin32					
	
	.globl io_out8				
	.type	io_out8, @function	
	io_out8:				
	movl 4(%esp), %edx	
	movl 8(%esp), %eax	
	outb %al ,%dx 		
	ret					
	
	.globl io_in8				
	.type	io_in8, @function	
	io_in8:				
	movl 4(%esp), %edx	
	movl $0x00,	%eax	
	inb %dx ,%al 		
	ret					
	
#NO_APP
	.globl	io_write32
	.type	io_write32, @function
io_write32:
.LFB2:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$20, %esp
	movl	24(%ebp), %eax
	movb	%al, -20(%ebp)
	movl	graphicmode, %eax
	movl	%eax, -12(%ebp)
	movl	graphicmode+4, %edx
	movl	20(%ebp), %eax
	imull	%edx, %eax
	addl	%eax, -12(%ebp)
	movl	16(%ebp), %eax
	addl	%eax, -12(%ebp)
	movl	$0, -8(%ebp)
	jmp	.L8
.L11:
	movl	$0, -4(%ebp)
	jmp	.L9
.L10:
	movl	-4(%ebp), %eax
	addl	-12(%ebp), %eax
	movzbl	-20(%ebp), %edx
	movb	%dl, (%eax)
	addl	$1, -4(%ebp)
.L9:
	movl	-4(%ebp), %eax
	cmpl	8(%ebp), %eax
	jl	.L10
	movl	graphicmode+4, %eax
	addl	%eax, -12(%ebp)
	addl	$1, -8(%ebp)
.L8:
	movl	-8(%ebp), %eax
	cmpl	12(%ebp), %eax
	jl	.L11
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE2:
	.size	io_write32, .-io_write32
	.globl	io_sd32
	.type	io_sd32, @function
io_sd32:
.LFB3:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	pushl	%edi
	pushl	%esi
	pushl	%ebx
	subl	$52, %esp
	movl	20(%ebp), %edx
	movl	24(%ebp), %eax
	movl	%eax, -48(%ebp)
	movl	28(%ebp), %eax
	movl	%eax, -52(%ebp)
	movl	32(%ebp), %edi
	.cfi_offset 3, -20
	.cfi_offset 6, -16
	.cfi_offset 7, -12
	movl	36(%ebp), %esi
	movl	40(%ebp), %ebx
	movl	44(%ebp), %ecx
	movl	48(%ebp), %eax
	movb	%dl, -16(%ebp)
	movzbl	-48(%ebp), %edx
	movb	%dl, -20(%ebp)
	movzbl	-52(%ebp), %edx
	movb	%dl, -24(%ebp)
	movl	%edi, %edx
	movb	%dl, -28(%ebp)
	movl	%esi, %edx
	movb	%dl, -32(%ebp)
	movb	%bl, -36(%ebp)
	movb	%cl, -40(%ebp)
	movb	%al, -44(%ebp)
	movl	$4, 8(%esp)
	movl	$0, 4(%esp)
	movl	8(%ebp), %eax
	movl	%eax, (%esp)
	call	memset
	movzbl	-44(%ebp), %eax
	movl	%eax, %edx
	andl	$1, %edx
	movl	8(%ebp), %eax
	movl	%edx, %ecx
	andl	$1, %ecx
	movzbl	7(%eax), %edx
	andl	$-2, %edx
	orl	%ecx, %edx
	movb	%dl, 7(%eax)
	movzbl	-40(%ebp), %eax
	movl	%eax, %edx
	andl	$1, %edx
	movl	8(%ebp), %eax
	andl	$1, %edx
	leal	(%edx,%edx), %ecx
	movzbl	7(%eax), %edx
	andl	$-3, %edx
	orl	%ecx, %edx
	movb	%dl, 7(%eax)
	movzbl	-36(%ebp), %eax
	movl	%eax, %edx
	andl	$1, %edx
	movl	8(%ebp), %eax
	andl	$1, %edx
	leal	0(,%edx,8), %ecx
	movzbl	7(%eax), %edx
	andl	$-9, %edx
	orl	%ecx, %edx
	movb	%dl, 7(%eax)
	movzbl	-32(%ebp), %eax
	movl	%eax, %edx
	andl	$1, %edx
	movl	8(%ebp), %eax
	movl	%edx, %ecx
	andl	$1, %ecx
	movzbl	4(%eax), %edx
	andl	$-2, %edx
	orl	%ecx, %edx
	movb	%dl, 4(%eax)
	movzbl	-28(%ebp), %eax
	movl	%eax, %edx
	andl	$3, %edx
	movl	8(%ebp), %eax
	andl	$3, %edx
	leal	(%edx,%edx), %ecx
	movzbl	4(%eax), %edx
	andl	$-7, %edx
	orl	%ecx, %edx
	movb	%dl, 4(%eax)
	movzbl	-24(%ebp), %eax
	movl	%eax, %edx
	andl	$1, %edx
	movl	8(%ebp), %eax
	andl	$1, %edx
	leal	0(,%edx,8), %ecx
	movzbl	4(%eax), %edx
	andl	$-9, %edx
	orl	%ecx, %edx
	movb	%dl, 4(%eax)
	movzbl	-20(%ebp), %eax
	movl	%eax, %edx
	andl	$7, %edx
	movl	8(%ebp), %eax
	andl	$7, %edx
	movl	%edx, %ecx
	sall	$4, %ecx
	movzbl	4(%eax), %edx
	andl	$-113, %edx
	orl	%ecx, %edx
	movb	%dl, 4(%eax)
	movzbl	-16(%ebp), %eax
	movl	%eax, %edx
	andl	$1, %edx
	movl	8(%ebp), %eax
	movl	%edx, %ecx
	sall	$7, %ecx
	movzbl	4(%eax), %edx
	andl	$127, %edx
	orl	%ecx, %edx
	movb	%dl, 4(%eax)
	addl	$52, %esp
	popl	%ebx
	.cfi_restore 3
	popl	%esi
	.cfi_restore 6
	popl	%edi
	.cfi_restore 7
	popl	%ebp
	.cfi_def_cfa 4, 4
	.cfi_restore 5
	ret
	.cfi_endproc
.LFE3:
	.size	io_sd32, .-io_sd32
	.globl	init_pic
	.type	init_pic, @function
init_pic:
.LFB4:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$24, %esp
#APP
# 127 "lowCall32.c" 1
	cli
# 0 "" 2
#NO_APP
	movl	$255, 4(%esp)
	movl	$33, (%esp)
	call	io_out8
	movl	$255, 4(%esp)
	movl	$161, (%esp)
	call	io_out8
	movl	$17, 4(%esp)
	movl	$32, (%esp)
	call	io_out8
	movl	$32, 4(%esp)
	movl	$33, (%esp)
	call	io_out8
	movl	$4, 4(%esp)
	movl	$33, (%esp)
	call	io_out8
	movl	$1, 4(%esp)
	movl	$33, (%esp)
	call	io_out8
	movl	$17, 4(%esp)
	movl	$160, (%esp)
	call	io_out8
	movl	$40, 4(%esp)
	movl	$161, (%esp)
	call	io_out8
	movl	$2, 4(%esp)
	movl	$161, (%esp)
	call	io_out8
	movl	$1, 4(%esp)
	movl	$161, (%esp)
	call	io_out8
	movl	$253, 4(%esp)
	movl	$33, (%esp)
	call	io_out8
	movl	$255, 4(%esp)
	movl	$161, (%esp)
	call	io_out8
#APP
# 145 "lowCall32.c" 1
	sti
# 0 "" 2
#NO_APP
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE4:
	.size	init_pic, .-init_pic
	.ident	"GCC: (Ubuntu/Linaro 4.6.1-9ubuntu3) 4.6.1"
	.section	.note.GNU-stack,"",@progbits
