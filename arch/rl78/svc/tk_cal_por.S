 ;
 ;	micro T-Kernel SVC interface library (mb9af312k)
 ;
 ;	   created from /usr/local/cortex-m3/utkernel_source/include/tk/syscall.h
 ;

#include <machine.h>
#include <sysdef.h>
#include <tkfncd.h>
#include "utk_config.h"

;	.code 16
;	.syntax unified
;	.thumb

#if USE_TRAP || (USE_DBGSPT && USE_HOOK_TRACE)
;	.text
;	.align 2
;	.thumb_func
	.public	_tk_cal_por
;	.type	_tk_cal_por, %function
_tk_cal_por:
;	push	{r4,lr}
;	add	r4, sp, #8
;	ldr	ip, =TFN_CAL_POR
#if USE_TRAP
;	svc	SVC_SYSCALL
#else
;	bl	_knl_call_entry
#endif
;	pop	{r4,lr}
;	bx	lr
#endif

