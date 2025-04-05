 ;----------------------------------------------------------------------
 ;    micro T-Kernel 2.0 Software Package
 ;
 ;    Copyright (C) 2006-2014 by Ken Sakamura.
 ;    This software is distributed under the T-License 2.0.
 ;----------------------------------------------------------------------
 ;
 ;    Released by T-Engine Forum(http://www.t-engine.org/) at 2014/09/01.
 ;    distribute by pkernel github(https://github.com/monyuonyu/) at 2016/04/29.
 ;
 ;----------------------------------------------------------------------

 ;  @(#) icrt0.S (RL78-S2)
 ;
 ;    Start up module
 ;

	.public _startup_common
_startup_common:

	; .data initialization
data_init:
data_loop:
data_done:

	; .bss initialization
#if USE_NOINIT

#else

bss_init:
bss_loop:
bss_done:

#endif /* USE_NOINIT */

#if USE_IMALLOC
#endif

#if USE_TMONITOR
	call	!!_sio_init		; initialize serial IO
#endif

	; Configure exception priorities

	; Enable software interrupt
	call	!!_EnableInt

kernel_start:
	call	!!_main

l_end:
