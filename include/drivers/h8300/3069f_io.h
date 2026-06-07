/*
 * 3069f_io.h
 *
 *  Created on: 2010/11/27
 *      Author: pino
 */
#ifndef _3069F_IO_H_
#define _3069F_IO_H_

struct byte
{
	unsigned char byte;
	union
	{
		unsigned char b7:1;
		unsigned char b6:1;
		unsigned char b5:1;
		unsigned char b4:1;
		unsigned char b3:1;
		unsigned char b2:1;
		unsigned char b1:1;
		unsigned char b0:1;
	}bit;
};

/* EMC = 1 */
#define _P1DDR 0xFEE000
#define _P2DDR 0xFEE001
#define _P3DDR 0xFEE002
#define _P4DDR 0xFEE003
#define _P5DDR 0xFEE004

#define _P1DR 0xFFFFD0
#define _P2DR 0xFFFFD1
#define _P3DR 0xFFFFD2
#define _P4DR 0xFFFFD3
#define _P5DR 0xFFFFD4

#define P1DDR (*(volatile unsigned char*)_P1DDR)
#define P2DDR (*(volatile unsigned char*)_P2DDR)
#define P3DDR (*(volatile unsigned char*)_P3DDR)
#define P4DDR (*(volatile unsigned char*)_P4DDR)
#define P5DDR (*(volatile unsigned char*)_P5DDR)

#define P1DR (*(volatile unsigned char*)_P1DR)
#define P2DR (*(volatile unsigned char*)_P2DR)
#define P3DR (*(volatile unsigned char*)_P3DR)
#define P4DR (*(volatile unsigned char*)_P4DR)
#define P5DR (*(volatile unsigned char*)_P5DR)


// 3069アセンブラ
#define as_SLEEP_3069 asm volatile ("sleep");
#define as_SLEEP_LOOP_3069 while(1){asm volatile ("sleep");}

#endif
