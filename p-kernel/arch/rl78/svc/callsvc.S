 ;
 ;	@(#)callsvc.S
 ;
 ;	Call extended SVC handler
 ;
 ;	INT callsvc( FN fncd, args ... )
 ;

#include <machine.h>

#include <asm.h>
#include "utk_config.h"

;	.code 16
;	.syntax unified
;	.thumb

;	.text
;	.text
;	.align 2
;	.thumb_func
	.public	_callsvc
;	.type	_callsvc, %function
_callsvc:
;	stmfd	sp!, {r1-r3}
;	mov	ip, r0
;	mov	r0, sp
;	push	{lr}

#if USE_TRAP
;	svc	SVC_EXTENDED_SVC
#else
;	bl	knl_call_entry
#endif

;	pop	{lr}
;	add	sp, sp, #3*4
;	bx	lr
	
