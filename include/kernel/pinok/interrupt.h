/*
 * interrupt.h
 *
 *  Created on: 2011/05/06
 *      Author: mizu
 */

#ifndef INTERRUPT_H_
#define INTERRUPT_H_

extern char softvec;	/* ソフトウェア・ベクタ */
#define SOFTVEC_ADDR (&softvec)

#define SOFTVEC_TYPE_NUM 3

// ※ アセンブラから参照するのでココではenum禁止
//typedef short softvec_type_t;	/* int でもok */
typedef enum
{
	softvec_type_syscall = 0,
	softvec_type_syserror,
	softvec_type_serial,
}softvec_type_t;


typedef void (*softvec_handler_t)(softvec_type_t type, unsigned long sp);	/* ユーザー定義の割り込みハンドラの定義 */

#define INTERUUPT_ENABLE	asm volatile("andc.b #0x3f,ccr");				/* 割り込み有効 */
#define INTERUUPT_DISABLE	asm volatile("orc.b #0xc0,ccr");				/* 割り込み無効 */

/* 割り込みベクタの初期化 */
void softvec_init();

/* 割り込みベクタへ関数を登録 */
void softvec_setintr(softvec_type_t type, softvec_handler_t handler);




/* 共通ハンドラ */
//void interrupt(softvec_type_t type, unsigned long sp);

#endif /* INTERRUPT_H_ */
