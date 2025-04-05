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

	.public _knl_intvec
_knl_intvec:
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
 ;;
 ;; Start up routine
 ;;

	.public _Reset_Handler
_Reset_Handler:
	.public _pkernel_start
_pkernel_start:
	call	!!_init_clock_control

setup_ram_vectors:
	; Load vector table to SRAM

vector_loop:

vector_done:
	;  Set vector table offset to SRAM

	br	!!_startup_common
