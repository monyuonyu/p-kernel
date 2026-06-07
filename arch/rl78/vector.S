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

 ;  @(#) vector.S
 ;
 ;    Vector table and interrupt handlers
 ;

;	.code 16
;	.syntax unified
;	.thumb

 ; Vector Table
;	.section .vector
;	.align 2

;__vector:
;	.word	EXCEPTION_STACK_TOP 		;  0 -> sp value for reset
;	.word	_Reset_Handler		;  1 -> reset
;	.word	_nmi_handler		;  2 -> Non maskable interrupt
;	.word	_hardfault_handler		;  3 -> Hard fault
;	.word	_memmng_handler		;  4 -> Memory manage fault
;	.word	_busfault_handler		;  5 -> Bus fault abort
;	.word	_usagefault_handler	;  6 -> Usage fault
;	.word	0				;  7 -> Reserved
;	.word	0				;  8 -> Reserved
;	.word	0				;  9 -> Reserved
;	.word	0				;  10 -> Reserved
;	.word	_knl_svcall_entry		;  11 -> Svcall
;	.word	0				;  12 -> Debug monitor
;	.word	0				;  13 -> Reserved
;	.word	_knl_dispatch_entry	;  14 -> Pend SV
;	.word	_knl_timer_handler_startup	;  15 -> Systick
;	;  External Interrupts
;__extintvec:
;	.word	_CSV_Handler		;  0: Clock Super Visor
;	.word	_SWDT_Handler		;  1: Software Watchdog Timer
;	.word	_LVD_Handler		;  2: Low Voltage Detector
;	.word	_MFT_WG_IRQHandler		;  3: Wave Form Generator / DTIF
;	.word	_INT0_7_Handler		;  4: External Interrupt Request ch.0 to ch.7
;	.word	_INT8_15_Handler		;  5: External Interrupt Request ch.8 to ch.15
;	.word	_DT_Handler		;  6: Dual Timer / Quad Decoder
;	.word	_MFS0RX_IRQHandler		;  7: MultiFunction Serial ch.0
;	.word	_MFS0TX_IRQHandler		;  8: MultiFunction Serial ch.0
;	.word	_MFS1RX_IRQHandler		;  9: MultiFunction Serial ch.1
;	.word	_MFS1TX_IRQHandler		;  10: MultiFunction Serial ch.1
;	.word	_MFS2RX_IRQHandler		;  11: MultiFunction Serial ch.2
;	.word	_MFS2TX_IRQHandler		;  12: MultiFunction Serial ch.2
;	.word	_MFS3RX_IRQHandler		;  13: MultiFunction Serial ch.3
;	.word	_MFS3TX_IRQHandler		;  14: MultiFunction Serial ch.3
;	.word	_MFS4RX_IRQHandler		;  15: MultiFunction Serial ch.4
;	.word	_MFS4TX_IRQHandler		;  16: MultiFunction Serial ch.4
;	.word	_MFS5RX_IRQHandler		;  17: MultiFunction Serial ch.5
;	.word	_MFS5TX_IRQHandler		;  18: MultiFunction Serial ch.5
;	.word	_MFS6RX_IRQHandler		;  19: MultiFunction Serial ch.6
;	.word	_MFS6TX_IRQHandler		;  20: MultiFunction Serial ch.6
;	.word	_MFS7RX_IRQHandler		;  21: MultiFunction Serial ch.7
;	.word	_MFS7TX_IRQHandler		;  22: MultiFunction Serial ch.7
;	.word	_PPG_Handler		;  23: PPG
;	.word	_TIM_IRQHandler		;  24: OSC / PLL / Watch Counter
;	.word	_ADC0_IRQHandler		;  25: ADC0
;	.word	_ADC1_IRQHandler		;  26: ADC1
;	.word	_ADC2_IRQHandler		;  27: ADC2
;	.word	_MFT_FRT_IRQHandler	;  28: Free-run Timer
;	.word	_MFT_IPC_IRQHandler	;  29: Input Capture
;	.word	_MFT_OPC_IRQHandler	;  30: Output Compare
;	.word	_BT0_IRQHandler		;  31: Base Timer ch.0 to ch.7
;	.word	_CAN0_IRQHandler		;  32: CAN ch.0
;	.word	_CAN1_IRQHandler		;  33: CAN ch.1
;	.word	_USBF_Handler		;  34: USB Function
;	.word	_USB_Handler		;  35: USB Function / USB HOST
;	.word	_DummyHandler		;  36: Reserved
;	.word	_DummyHandler		;  37: Reserved
;	.word	_DMAC0_Handler		;  38: DMAC ch.0
;	.word	_DMAC1_Handler		;  39: DMAC ch.1
;	.word	_DMAC2_Handler		;  40: DMAC ch.2
;	.word	_DMAC3_Handler		;  41: DMAC ch.3
;	.word	_DMAC4_Handler		;  42: DMAC ch.4
;	.word	_DMAC5_Handler		;  43: DMAC ch.5
;	.word	_DMAC6_Handler		;  44: DMAC ch.6
;	.word	_DMAC7_Handler		;  45: DMAC ch.7
;	.word	_BT8_IRQHandler		;  46: Base Timer ch.8 to ch.15
;	.word	_knl_dispatch_to_schedtsk	;  47:	force dispatch

;	.weak	_knl_intvec
;	.set	_knl_intvec, __extintvec

 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
 ;;
 ;; Default interrupt handlers.
 ;;
;	.text
;	.align 2

;	.thumb
;	.thumb_func
	.public _Default_Handler
_Default_Handler:
;	b	.

;	.weak	_CSV_Handler
;	.thumb_set _CSV_Handler, _Default_Handler
;	.weak	_SWDT_Handler
;	.thumb_set _SWDT_Handler, _Default_Handler
;	.weak	_LVD_Handler
;	.thumb_set _LVD_Handler, _Default_Handler
;	.weak	_MFT_WG_IRQHandler
;	.thumb_set _MFT_WG_IRQHandler, _Default_Handler
;	.weak	_INT0_7_Handler
;	.thumb_set _INT0_7_Handler, _Default_Handler
;	.weak	_INT8_15_Handler
;	.thumb_set _INT8_15_Handler, _Default_Handler
;	.weak	_DT_Handler
;	.thumb_set _DT_Handler, _Default_Handler
;	.weak	_MFS0RX_IRQHandler
;	.thumb_set _MFS0RX_IRQHandler, _Default_Handler
;	.weak	_MFS0TX_IRQHandler
;	.thumb_set _MFS0TX_IRQHandler, _Default_Handler
;	.weak	_MFS1RX_IRQHandler
;	.thumb_set _MFS1RX_IRQHandler, _Default_Handler
;	.weak	_MFS1TX_IRQHandler
;	.thumb_set _MFS1TX_IRQHandler, _Default_Handler
;	.weak	_MFS2RX_IRQHandler
;	.thumb_set _MFS2RX_IRQHandler, _Default_Handler
;	.weak	_MFS2TX_IRQHandler
;	.thumb_set _MFS2TX_IRQHandler, _Default_Handler
;	.weak	_MFS3RX_IRQHandler
;	.thumb_set _MFS3RX_IRQHandler, _Default_Handler
;	.weak	_MFS3TX_IRQHandler
;	.thumb_set _MFS3TX_IRQHandler, _Default_Handler
;	.weak	_MFS4RX_IRQHandler
;	.thumb_set _MFS4RX_IRQHandler, _Default_Handler
;	.weak	_MFS4TX_IRQHandler
;	.thumb_set _MFS4TX_IRQHandler, _Default_Handler
;	.weak	_MFS5RX_IRQHandler
;	.thumb_set _MFS5RX_IRQHandler, _Default_Handler
;	.weak	_MFS5TX_IRQHandler
;	.thumb_set _MFS5TX_IRQHandler, _Default_Handler
;	.weak	_MFS6RX_IRQHandler
;	.thumb_set _MFS6RX_IRQHandler, _Default_Handler
;	.weak	_MFS6TX_IRQHandler
;	.thumb_set _MFS6TX_IRQHandler, _Default_Handler
;	.weak	_MFS7RX_IRQHandler
;	.thumb_set _MFS7RX_IRQHandler, _Default_Handler
;	.weak	_MFS7TX_IRQHandler
;	.thumb_set _MFS7TX_IRQHandler, _Default_Handler
;	.weak	_PPG_Handler
;	.thumb_set _PPG_Handler, _Default_Handler
;	.weak	_TIM_IRQHandler
;	.thumb_set _TIM_IRQHandler, _Default_Handler
;	.weak	_ADC0_IRQHandler
;	.thumb_set _ADC0_IRQHandler, _Default_Handler
;	.weak	_ADC1_IRQHandler
;	.thumb_set _ADC1_IRQHandler, _Default_Handler
;	.weak	_ADC2_IRQHandler
;	.thumb_set _ADC2_IRQHandler, _Default_Handler
;	.weak	_MFT_FRT_IRQHandler
;	.thumb_set _MFT_FRT_IRQHandler, _Default_Handler
;	.weak	_MFT_IPC_IRQHandler
;	.thumb_set _MFT_IPC_IRQHandler, _Default_Handler
;	.weak	_MFT_OPC_IRQHandler
;	.thumb_set _MFT_OPC_IRQHandler, _Default_Handler
;	.weak	_BT0_IRQHandler
;	.thumb_set _BT0_IRQHandler, _Default_Handler
;	.weak	_CAN0_IRQHandler
;	.thumb_set _CAN0_IRQHandler, _Default_Handler
;	.weak	_CAN1_IRQHandler
;	.thumb_set _CAN1_IRQHandler, _Default_Handler
;	.weak	_USBF_Handler
;	.thumb_set _USBF_Handler, _Default_Handler
;	.weak	_USB_Handler
;	.thumb_set _USB_Handler, _Default_Handler
;	.weak	_DMAC0_Handler
;	.thumb_set _DMAC0_Handler, _Default_Handler
;	.weak	_DMAC1_Handler
;	.thumb_set _DMAC1_Handler, _Default_Handler
;	.weak	_DMAC2_Handler
;	.thumb_set _DMAC2_Handler, _Default_Handler
;	.weak	_DMAC3_Handler
;	.thumb_set _DMAC3_Handler, _Default_Handler
;	.weak	_DMAC4_Handler
;	.thumb_set _DMAC4_Handler, _Default_Handler
;	.weak	_DMAC5_Handler
;	.thumb_set _DMAC5_Handler, _Default_Handler
;	.weak	_DMAC6_Handler
;	.thumb_set _DMAC6_Handler, _Default_Handler
;	.weak	_DMAC7_Handler
;	.thumb_set _DMAC7_Handler, _Default_Handler
;	.weak	_BT8_IRQHandler
;	.thumb_set _BT8_IRQHandler, _Default_Handler
;	.weak	_DummyHandler
;	.thumb_set _DummyHandler, _Default_Handler

 ; NMI handler
;	.thumb
; 	.thumb_func
	.public	_nmi_handler
_nmi_handler:
;	b	.

 ; Hard fault handler.
;	.thumb
; 	.thumb_func
	.public	_hardfault_handler
_hardfault_handler:
;	b	.

 ; memory fault handler.
;	.thumb
; 	.thumb_func
	.public	_memmng_handler
_memmng_handler:
;	b	.

 ; Bus fault handler.
;	.thumb
; 	.thumb_func
	.public	_busfault_handler
_busfault_handler:
;	b	.

 ; Usage fault handler.
;	.thumb
; 	.thumb_func
	.public	_usagefault_handler
_usagefault_handler:
;	b	.

;	.end
