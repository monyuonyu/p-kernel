 ;
 ;----------------------------------------------------------------------
 ;    micro T-Kernel 2.0 Software Package
 ;
 ;    Copyright (C) 2006-2014 by Ken Sakamura.
 ;    This software is distributed under the T-License 2.0.
 ;----------------------------------------------------------------------
 ;
 ;    Released by T-Engine Forum(http://www.t-engine.org/) at 2014/09/01.
 ;
 ;----------------------------------------------------------------------
 ;

 ;
 ;	cpu_support.S (FM3)
 ;	Device-Dependent CPU Operation
 ;

#define	_in_asm_source_

#if USE_TRAP | (USE_DBGSPT & USE_HOOK_TRACE)
#include "isysconf.h"
#endif

#include "tkdev_conf.h"
#include "offset.h"

;	.code 16
;	.syntax unified
;	.thumb

;  ------------------------------------------------------------------------
 ;
 ; Dispatcher
 ;	dispatch_entry:
 ;		Normal dispatch processing.
 ;	_ret_int_dispatch:
 ;		Called when dispatch is required by 'tk_ret_int().'
 ;
 ;	Contexts to save
 ;	Save registers except for ssp to a stack. Save 'ssp' to TCB.
 ;
 ;	   High Address +---------------+
 ;			| xpsr		|
 ;			| Return Address| Return address (pc)
 ;			| lr		|
 ;			| R12 (ip)	|
 ;			| R0-R3		|
 ;			+---------------+ Save by Exception entry process.
 ;			+---------------+
 ;			| lr		|
 ;			| taskmode	|
 ;		ssp ->	| R4 - R11	|
 ;	    Low Address +---------------+
 ;
;	.text
;	.align 2
;	.thumb
;	.thumb_func
	.public	_knl_dispatch_to_schedtsk
_knl_dispatch_to_schedtsk:
	;  Interrupt is disabled(CPSR.I=1 F=1), during SVC mode
;	ldr	sp, =TMP_STACK_TOP		;  Set temporal stack

;	ldr	ip, =_knl_dispatch_disabled
;	ldr	r0, =1
;	str	r0, [ip]			;  Dispatch disable

;	ldr	r4, =_knl_ctxtsk		;  R4 = &ctxtsk
;	ldr	r0, =0
#if USE_DBGSPT
;	ldr	r8, [r4]
#endif

;	str	r0, [r4]			;  ctxtsk = NULL

	;  Clear the pending state from the PendSV exception
;	ldr	r0, =SCB_ICSR
;	ldr	ip, =ICSR_PENDSVCLR
;	str	ip, [r0]

	br	!!l_dispatch0

;	.thumb
;	.thumb_func
	.public	_knl_dispatch_entry
_knl_dispatch_entry:
_ret_int_dispatch:
;	push	{lr}				;  Context save (lr)

;	ldr	r0, =_knl_dispatch_disabled
;	ldr	r1, =1
;	str	r1, [r0]			;  Dispatch disable

;	ldr	lr, =_knl_taskmode
;	ldr	lr, [lr]
;	stmfd	sp!, {r4-r11, lr}

	;  Context save
;	ldr	r4, =_knl_ctxtsk		;  R4 = &ctxtsk
;	ldr	r0, =0
;	ldr	r8, [r4]
;	cmp	r8, #0
;	it	ne
;	strne	sp, [r8, #TCB_tskctxb + CTXB_ssp]	;  Save 'ssp' to TCB
;	str	r0, [r4]			;  ctxtsk = NULL

l_dispatch0:

#if USE_DBGSPT & USE_HOOK_TRACE
;	ldr	ip, =hook_stop_jmp		;  Hook processing
;	ldr	pc, [ip]

;	.thumb_func
ret_hook_stop:
#endif

;	ldr	r5, =_knl_schedtsk		;  R5 = &schedtsk
;	ldr	r6, =_knl_lowpow_discnt	;  R6 = &lowpow_discnt

l_dispatch1:
;	ldr	r0, =MAX_EXTINT_PRI
;	msr	basepri, r0

;	ldr	r8, [r5]			;  R8 = schedtsk
;	cmp	r8, #0				;  Is there 'schedtsk'?
;	bne	l_dispatch2

	;  Because there is no task that should be executed, move to the power-saving mode
;	ldr	ip, [r6]			;  Is 'low_pow' disabled?
;	cmp	ip, #0
;	it	eq
;	bleq	_knl_low_pow		;  call low_pow()

;	ldr	r0, =0
;	msr	basepri, r0

	br	!!l_dispatch1

l_dispatch2:					;  Switch to 'schedtsk'
;	str	r8, [r4]			;  ctxtsk = schedtsk
;	ldr	sp, [r8, #TCB_tskctxb + CTXB_ssp]	;  Restore 'ssp' from TCB

#if USE_DBGSPT & USE_HOOK_TRACE
;	ldr	ip, =hook_exec_jmp		;  Hook processing
;	ldr	pc, [ip]

;	.thumb_func
ret_hook_exec:
#endif

;	ldr	r0, =_knl_dispatch_disabled
;	ldr	r1, =0
;	str	r1, [r0]			;  Dispatch enable

;	ldmfd	sp!, {r4-r11, lr}
;	ldr	r1, =_knl_taskmode
;	str	lr, [r1]

;	pop	{lr}				;  R14_svc restore

;	ldr	r0, =0
;	msr	basepri, r0

;	mrs	r0, basepri

;	EXC_RETURN


#if USE_DBGSPT & USE_HOOK_TRACE
 ;
 ; Task dispatcher hook routine call
 ;	void stop( ID tskid, INT lsid, UINT tskstat )
 ;	void exec( ID tskid, INT lsid )
 ;
;	.text
;	.align 2
;	.thumb_func
hook_stop:
;	cmp	r8, #0			;  r8 = ctxtsk
;	beq	l_notask

;	ldrb	r2, [r8, #TCB_state]	;  tskstat
;	mov	r2, r2, lsl #1
;	ldr	r0, [r8, #TCB_tskid]	;  tskid

;	ldr	ip, =_knl_hook_stopfn
;	ldr	ip, [ip]
;	blx	ip			;  call stop(tskid, lsid, tskstat)

l_notask:
	br	!!ret_hook_stop

;	.thumb_func
hook_exec:
					;  r8 = ctxtsk
;	ldr	r0, [r8, #TCB_tskid]	;  tskid

;	ldr	ip, =_knl_hook_execfn
;	ldr	ip, [ip]
;	blx	ip			;  call exec(tskid, lsid)

	br	!!ret_hook_exec

 ;
 ; Set/Free task dispatcher hook routine
 ;
;	.text
;	.align 2
;	.thumb
;	.thumb_func
	.public _knl_hook_dsp
_knl_hook_dsp:
;	ldr	r0, =hook_exec_jmp
;	ldr	r1, =hook_stop_jmp
;	ldr	r2, =hook_exec
;	ldr	r3, =hook_stop
;	str	r2, [r0]
;	str	r3, [r1]
;	bx	lr

;	.thumb
;	.thumb_func
	.public _knl_unhook_dsp
_knl_unhook_dsp:
;	ldr	r0, =hook_exec_jmp
;	ldr	r1, =hook_stop_jmp
;	ldr	r2, =ret_hook_exec
;	ldr	r3, =ret_hook_stop
;	str	r2, [r0]
;	str	r3, [r1]
;	bx	lr

;	.data
;	.align 2
;hook_exec_jmp:	.long	ret_hook_exec
;hook_stop_jmp:	.long	ret_hook_stop

#endif ;  USE_DBGSPT & USE_HOOK_TRACE


#if USE_HLL_INTHDR
;  ------------------------------------------------------------------------
 ;
 ; High level programming language routine for interrupt handler
 ;	Called by interrupt entry routine.
 ;
 ;	The vector table address is set in 'ip.'
 ;	(ip - EIT_VECTBL) / 4 = Vector number
 ;
 ;
;	.text
;	.align 2
;	.thumb
;	.thumb_func
	.public	_knl_inthdr_startup
_knl_inthdr_startup:
;	push	{r4,lr}			;  push EXC_RETURN value to stack
					;  r4 is dummy (AAPCS)
;	ldr	ip, =_knl_taskindp	;  Task independent part
;	ldr	r1, [ip]
;	add	r1, r1, #1
;	str	r1, [ip]

;	mrs	r0, ipsr
;	sub	r0, r0, #16		;  interrupt number (intno)

#if USE_DBGSPT & USE_HOOK_TRACE
;	ldr	ip, =hook_ienter_jmp
;	ldr	pc, [ip]

;	.thumb_func
ret_hook_ienter:
#endif

;	ldr	ip, =_knl_hll_inthdr
;	lsl	r1, r0, #2
;	ldr	ip, [ip, r1]
;	blx	ip			;  call hll_inthdr[n](intno)

#if USE_DBGSPT & USE_HOOK_TRACE
;	ldr	ip, =hook_ileave_jmp
;	ldr	pc, [ip]

;	.thumb_func
ret_hook_ileave:
#endif

;	ldr	ip, =_knl_taskindp
;	ldr	r2, [ip]
;	sub	r2, r2, #1
;	str	r2, [ip]

;	pop	{r4,lr}			;  pop EXC_RETURN value from stack
					;  r4 is dummy (AAPCS)
;	EXC_RETURN


#if USE_DBGSPT & USE_HOOK_TRACE
 ;
 ; Interrupt handler hook routine call
 ;
;	.text
;	.align 2
;	.thumb_func
hook_ienter:
;	stmfd	sp!, {r0, r1}		;  Register save
;	push	{r3}

;	ldr	ip, =_knl_hook_ienterfn
;	ldr	ip, [ip]
;	blx	ip			;  call enter(intno, sp)

;	pop	{r3}			;  Register restore
;	ldmfd	sp, {r0, r1}		;  Leave 'intno,' 'sp' on stack
	br	!!ret_hook_ienter

;	.thumb_func
hook_ileave:
;	ldmfd	sp!, {r0, r1}		;  Restore 'intno,' 'sp'

;	ldr	ip, =_knl_hook_ileavefn
;	ldr	ip, [ip]
;	blx	ip			;  call leave(intno, info)

	br	!!ret_hook_ileave

 ;
 ; Set/Free interrupt handler hook routine
 ;
;	.thumb
;	.thumb_func
	.public	_knl_hook_int
_knl_hook_int:
;	ldr	r0, =hook_ienter_jmp
;	ldr	r1, =hook_ileave_jmp
;	ldr	r2, =hook_ienter
;	ldr	r3, =hook_ileave
;	str	r2, [r0]
;	str	r3, [r1]
;	bx	lr

;	.thumb
;	.thumb_func
	.public	_knl_unhook_int
_knl_unhook_int:
;	ldr	r0, =hook_ienter_jmp
;	ldr	r1, =hook_ileave_jmp
;	ldr	r2, =ret_hook_ienter
;	ldr	r3, =ret_hook_ileave
;	str	r2, [r0]
;	str	r3, [r1]
;	bx	lr

;	.data
;	.align 2
;hook_ienter_jmp:		.long	ret_hook_ienter
;hook_ileave_jmp:		.long	ret_hook_ileave

#endif ;  USE_DBGSPT & USE_HOOK_TRACE
#endif ;  USE_HLL_INTHDR


 ;
 ; tk_ret_int_impl()
 ;
;	.text
;	.align 2
;	.thumb
;	.thumb_func
	.public	_tk_ret_int_impl
_tk_ret_int_impl:
;	EXC_RETURN

;  ------------------------------------------------------------------------

#if USE_TRAP | USE_DBGSPT | (CFN_MAX_SSYID > 0)
 ;
 ; Unsupported system call
 ;
;	.text
;	.align 2
;	.thumb
;	.thumb_func
	.public	_knl_no_support
_knl_no_support:
;	ldr	r0, =E_RSFN
;	bx	lr

 ;
 ; System call entry table
 ;
#if USE_TRAP | (USE_DBGSPT & USE_HOOK_TRACE)
;	.text
	.align	2
_svctbl:
;	.int	_knl_no_support
#define	tk_ret_int_impl	knl_no_support
#include <tksvctbl.h>
#undef	tk_ret_int_impl
#endif ;  USE_TRAP

 ;
 ; System call entry
 ;	Do not need to save the temporary register.
 ;	The compiler saves the permanent register.
 ;
 ;		+---------------+
 ;		| xPSR		|
 ;		| Return Address|
 ;		| lr		|
 ;		| ip		|
 ;		| r3		| Function code
 ;		| r2		|
 ;		| r1		| Addressof(svc #0) + 1
 ;		| r0		|
 ;	sp ->	+---------------+
 ;
;	.text
;	.align 2
;	.thumb
;	.thumb_func
	.public	_knl_svcall_entry
_knl_svcall_entry:
;	tst	lr, #0x4		;  Test EXC_RETURN number in LR bit 2
;	ite	eq
;	mrseq	r1, msp			;  Main stack
;	mrsne	r1, psp			;  Process stack

	;  Get Parameter of SVC instruction
;	ldr	r2, [r1, #24]
;	ldrb	r1, [r2, #-2]

;	ldr	r0, svcall_exc_ret

;	cmp	r1, #SVC_SYSCALL
;	it	eq
;	ldreq	r0, =_knl_call_entry

;	cmp	r1, #SVC_EXTENDED_SVC
;	it	eq
;	ldreq	r0, =_knl_call_entry

#if USE_DBGSPT & (USE_TRAP | USE_HOOK_TRACE)
	cmp	r1, #SVC_DEBUG_SUPPORT
;	it	eq
;	ldreq	r0, =_knl_call_dbgspt
#endif

;	ldr	r2, =svcall_exc_ret
;	cmp	r0, r2
;	ittt	eq
;	ldreq	r2, =E_RSFN
;	streq	r2, [sp]
;	beq	svcall_exc_ret

;	ldr	r1, [sp, #24]
;	orr	r1, r1, #0x00000001
;	str	r1, [sp, #20]

;	str	r0, [sp, #24]

svcall_exc_ret:
;	EXC_RETURN

;	.thumb
;	.thumb_func
	.public	_knl_call_entry
_knl_call_entry:

;	stmfd	sp!, {r0-r7, ip, lr}

	;  Load func code
;	add	r5, sp, #8*4

;	ldr	ip, =_knl_taskmode	;  Task mode flag update
;	ldr	r7, [ip]
;	push	{r7}			;  taskmode save
;	mov	r6, sp			;  Save stack pointer
;	mov	lr, r7, lsl #16
;	str	lr, [ip]

#if USE_DBGSPT & USE_HOOK_TRACE
;	ldr	ip, =hook_enter_jmp	;  Hook processing
;	ldr	pc, [ip]

;	.thumb_func
ret_hook_enter:
#endif

;	ldr	ip, [r5]
;	cmp	ip, #0			; 	<  0: System call
;	bge	l_esvc_function		; 	>= 0: Extended SVC

#if USE_TRAP | (USE_DBGSPT & USE_HOOK_TRACE)

	;  micro T-Kernel System Call
;	mov	r2, ip, asr #16		;  r2 = Function number
;	ldr	r1, =N_TFN + 0xffff8000
;	cmp	r2, r1
;	bgt	l_illegal_svc

;	mov	ip, ip, lsr #8
;	and	ip, ip, #0xff		;  ip = Number of arguments
	cmp	ip, #5
;	bne	l_nocopy
;	ldr	ip, [r4]		;  Copy fifth argument
;	push	{ip}
l_nocopy:

;	ldr	ip, =_svctbl - (0xffff8000 << 2)
;	add	r4, ip, r2, lsl #2
;	add	ip, r6, #4
;	ldmfd	ip, {r0, r1, r2, r3}
;	ldr	r4, [r4]		;  r4 = system call address
;	blx	r4
#else
	br	!!l_illegal_svc
#endif

l_retsvc:

#if USE_DBGSPT & USE_HOOK_TRACE
;	ldr	ip, =hook_leave_jmp	;  Hook processing
;	ldr	pc, [ip]

;	.thumb_func
ret_hook_leave:
#endif

;	mov	sp, r6
;	pop	{ip}			;  Restore register for work

;	ldr	r4, =_knl_taskmode	;  Task mode restore
;	str	ip, [r4]

;	add	sp, sp, #16
;	ldmfd	sp!, {r4-r7, ip, lr}

;	EXC_RETURN

l_illegal_svc:
;	ldr	r0, =E_RSFN
	br	!!l_retsvc

l_esvc_function:

#if CFN_MAX_SSYID > 0
	;  Extended SVC
;	ldr	r1, [r5]		;  r1 = Function code
;	bl	_knl_svc_ientry)	;  svc_ientry(pk_para, fncd
	br	!!l_retsvc
#else
;	ldr	r0, =E_SYS
	br	!!l_retsvc
#endif ;  CFN_MAX_SSYID > 0


#if USE_DBGSPT & USE_HOOK_TRACE
 ;
 ; System call/Extended SVC hook routine call
 ;	void *enter( FN fncd, TD_CALINF *calinf, ... )
 ;	void  leave( FN fncd, INT ret, void *exinf )
 ;
 ;	typedef struct td_calinf {
 ;		void	*ssp;	System stack pointer
 ;		void	*r11;	Flame pointer when calling
 ;	} TD_CALINF;
 ;
;	.text
;	.align 2

;	.thumb
;	.thumb_func
hook_enter:
;	stmfd	sp!, {r0-r3, r8-r10}	;  Save argument and register for work
;	mov	r8, sp			;  r8 = Keep stack position

;	mov	ip, r5			;  Create TD_CALINF
;	stmfd	sp!, {r5, ip}
;	mov	r9, sp			;  r9 = &TD_CALINF

;	ldr	ip, [r5]		;  ip = Function code
;	cmp	ip, #0			; 	< 0: System call
;	bge	l_hooksvc		; 	>= 0: Extended SVC

;	mov	ip, ip, lsr #8
;	and	ip, ip, #0xff		;  Number of arguments

;	cmp	ip, #5
;	itt	ge
;	ldrge	r10, [r4]
;	pushge	{r10}			;  Fifth argument
;	cmp	ip, #4
;	it	ge
;	pushge	{r3}			;  Fourth argument
;	cmp	ip, #3
;	it	ge
;	pushge	{r2}			;  Third argument
;	mov	r3, r1			;  Second argument
l_hooksvc:
;	mov	r2, r0			;  First argument
;	mov	r1, r9			;  calinf
;	ldr	r0, [r5]		;  fncd
;	ldr	ip, =_knl_hook_enterfn
;	ldr	ip, [ip]
;	blx	ip			;  exinf = enter(fncd, ...)
;	mov	ip, r0			;  Temporarily save 'exinf'

;	mov	sp, r8			;  Return stack position
;	ldmfd	sp!, {r0-r3, r8-r10}	;  Restore argument and register for work
;	push	{ip}			;  Save 'exinf' to stack
	br	!!ret_hook_enter

;	.thumb_func
hook_leave:
;	mov	r1, r0			;  r1 = ret

;	mov	lr, #0
;	ldr	r0, [r5]		;  r0 = Function code
;	cmp	r0, #0			; 	< 0: System call
;	bge	l_hooksvc2		; 	>= 0: Extended SVC

;	mov	lr, r0, lsr #8
;	and	lr, lr, #0xff		;  Number of arguments
;	subs	lr, lr, #4
;	it	lt
;	movlt	lr, #0
l_hooksvc2:

;	add	lr, lr, #1		;  Whether 'hook_enter' is executed
;	sub	ip, sp, r5		;  Check by stack usage
;	sub	r7, r5, #10*4		;  Location in which 'sp = exinf' is saved
;	cmp	lr, ip, lsr #2		;  If 'hook_enter' is executed,
;	ite	eq
;	ldreq	r2, =0			;  Get 'exinf' from stack
;	ldrne	r2, [r7]		;  If 'exinf' is not saved, 0

;	str	r9, [r7]		;  r9 save
;	mov	r9, r1			;  Save 'ret' in 'r9'

;	ldr	ip, =_knl_hook_leavefn
;	ldr	ip, [ip]
;	blx	ip			;  call leave(fncd, ret, exinf)

;	mov	r0, r9			;  r0 = ret restore
;	ldr	r9, [r7]		;  r9 restore
	br	!!ret_hook_leave

 ;
 ; Set/Free system call/extended SVC hook routine
 ;
;	.text
;	.align 2

;	.thumb
;	.thumb_func
	.public	_knl_hook_svc
_knl_hook_svc:
;	ldr	r0, =hook_enter_jmp
;	ldr	r1, =hook_leave_jmp
;	ldr	r2, =hook_enter
;	ldr	r3, =hook_leave
;	str	r2, [r0]
;	str	r3, [r1]
;	bx	lr

;	.thumb
;	.thumb_func
	.public	_knl_unhook_svc
_knl_unhook_svc:
;	ldr	r0, =hook_enter_jmp
;	ldr	r1, =hook_leave_jmp
;	ldr	r2, =ret_hook_enter
;	ldr	r3, =ret_hook_leave
;	str	r2, [r0]
;	str	r3, [r1]
;	bx	lr

;	.data
;hook_enter_jmp:	.long	ret_hook_enter
;hook_leave_jmp:	.long	ret_hook_leave

#endif ;  USE_DBGSPT & USE_HOOK_TRACE
#endif ;  USE_TRAP | USE_DBGSPT | CFN_MAX_SSYID > 0

;  ------------------------------------------------------------------------

#if USE_DBGSPT & (USE_TRAP | USE_HOOK_TRACE)
 ;
 ; Debugger support function service call entry table
 ;

;	.text
	.align	2
;	.thumb
_tdsvctbl:
;	.int	_knl_no_support
#include <tdsvctbl.h>

 ;
 ; Debugger support function service call entry
 ;
;	.text
;	.align 2
;	.thumb
;	.thumb_func
	.public	_knl_call_dbgspt
_knl_call_dbgspt:

;	stmfd	sp!, {r4-r6, lr}	;  Save register for work

;	mov	r4, ip			;  r4 -> func code
;	mov	r5, r4, asr #16		;  r5 = Function number
;	ldr	r6, =N_TDFN + 0xffff8000
;	cmp	r5, r6
;	bgt	b_illegal_svc

;	ldr	ip, =_tdsvctbl - (0xffff8000 << 2)
;	add	r4, ip, r5, lsl #2
;	ldr	r4, [r4]		;  r4 = system call address
;	blx	r4

b_retsvc:
;	ldmfd	sp!, {r4-r6, lr}	;  Restore register for work
;	EXC_RETURN


b_illegal_svc:
;	ldr	r0, =E_RSFN
	br	!!b_retsvc

#endif ;  USE_DBGSPT & (USE_TRAP | USE_HOOK_TRACE)


;  ------------------------------------------------------------------------
 ;
 ; High level programming language routine for timer handler
 ;
;	.text
;	.align 2
;	.thumb
;	.thumb_func
	.public	_knl_timer_handler_startup
_knl_timer_handler_startup:

;	stmfd	sp!, {r4-r5, ip, lr}	;  Register save

;	ldr	r4, =_knl_taskindp	;  Enter task independent part
;	ldr	r5, [r4]
;	add	r0, r5, #1
;	str	r0, [r4]

;	bl	_knl_timer_handler	;  call timer_handler()

;	str	r5, [r4]		;  Leave task independent part

;	ldmfd	sp!, {r4-r5, ip, lr}	;  Register restore

;	bx	lr

;  ------------------------------------------------------------------------

 ;
 ; Throw away the current task context.
 ; and forcibly dispatch to the task that should be performed next.
 ;	Use at system startup and 'tk_ext_tsk, tk_exd_tsk.'
 ;

;	.thumb
;	.thumb_func
	.public	_knl_force_dispatch
_knl_force_dispatch:
;	ldr	r0, =INTNO_FORCE_DISPATCH
;	ldr	r1, =SCB_STIR		;  Generate S/W Interrupt
;	str	r0, [r1]
;	dsb

;	mov	r0, #0
;	bl	_enaint
;	bx	lr

;	.thumb
;	.thumb_func
	.public	_knl_dispatch
_knl_dispatch:
;	ldr	r0, =SCB_ICSR
;	ldr	r1, =ICSR_PENDSVSET	;  Trigger pendsv exception
;	str	r1, [r0]
;	bx	lr

 ;
 ; Clear timer interrupt
 ;	Clear the timer interrupt request. Depending on the type of
 ;	hardware, there are two timings for clearing: at the beginning
 ;	and the end of the interrupt handler.
 ;	'clear_hw_timer_interrupt()' is called at the beginning of the
 ;	timer interrupt handler.
 ;	'end_of_hw_timer_interrupt()' is called at the end of the timer
 ;	interrupt handler.
 ;	Use either or both according to hardware.
 ;
;	.thumb
;	.thumb_func
	.public	_knl_clear_hw_timer_interrupt
_knl_clear_hw_timer_interrupt:
;	ldr	r0, =0xE000ED04		;  ICSR
;	ldr	r1, =0x02000000		;  PENDSTCLR
;	str	r1, [r0]
;	bx	lr

 ;
 ;	Control register operation
 ;
;	.thumb
;	.thumb_func
	.public	_knl_getXPSR
_knl_getXPSR:
;	mrs	r0, psr
;	bx	lr

;	.thumb
;	.thumb_func
	.public	_knl_getBASEPRI
_knl_getBASEPRI:
;	mrs	r0, basepri
;	bx	lr

;	.thumb
;	.thumb_func
	.public	_knl_getPRIMASK
_knl_getPRIMASK:
;	mrs	r0, primask
;	bx	lr

;	.end
