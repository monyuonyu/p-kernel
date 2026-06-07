 ;
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
 ;

 ;
 ;	@(#)disint.S (RL78-S2)
 ;
 ;	Enable/disable external interrupt
 ;

#include <machine.h>
#include <sysdef.h>

;（1）C 言語で定義した関数を参照する方法
;    C 言語により記述された関数をアセンブリ言語ルーチンから呼び出す手順は，次のようになります。
;（a）ワーク・レジスタ（AX，BC，DE）を退避する
;（b）引数をスタックに積む
;（c）C 言語ルーチンをコールする
;（d）引数のバイト数分スタック・ポインタ（SP）の値を修正する
;（e）C 言語ルーチンの戻り値（BC，またはDE，BC）を参照する

;（2）アセンブリ言語で定義した関数を参照する方法
;    C 言語ルーチンから呼び出されるアセンブリ言語で定義した関数では，次の手順で処理を行います。
;（a）ベース・ポインタ，レジスタ変数用saddr 領域を退避する
;（b）スタック・ポインタ（SP）をベース・ポインタ（HL）へコピーする
;（c）関数FUNC 本来の処理を行う
;（d）戻り値をセットする
;（e）退避したレジスタを復帰する
;（f）関数main へリターンする

 ;
 ; Disable external interrupt
 ; UINT disint( void )
 ;	Disable interrupt by Rise prioperty to 0.
 ;
	.public	_disint
	DI	;IE ← 0(Disable Interrupt)
_disint:

 ;
 ; Enable external interrupt (restore to original state)
 ; UINT enaint( UINT intsts )
 ;	Enable interrupt.
 ;
	.public	_enaint
_enaint:
	EI	;IE ← 1(Enable Interrupt)
 ;
 ; Get the value of Base Priority Mask Register.
 ; UINT get_basepri( void )
 ;
	.public	_get_basepri
	PUSH HL				; ベース・ポインタを退避
	PUSH AX				; 作業用空間退避
	MOVW HL, SP			; スタック・ポインタをコピー
;	MOVW AX, [HL]		; arg1
;	MOVW AX, [HL + 10]	; arg2

	CLRW AX				; 初期化
	MOV  A,  PSW		; プログラム・ステータス・ワード取得
	AND  A,  #6			; ISP0,ISP1取り出し
	SHR  A,  1			; 1bit右シフト

	MOVW BC, AX			; 戻り値を設定(BC)
	POP  AX				; 作業用空間復帰
	POP  HL				; ベース・ポインタを復帰
	RET
_get_basepri:

