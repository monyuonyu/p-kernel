/*
 * vectors.c
 *
 *  Created on: 2010/11/27
 *      Author: pino
 */

/*	H8/3069F 割り込みベクタ・テーブル
 * リセット			0		0x000000
 * NMI			7		0x00001c
 * トラップ			8~11	0x000020 ~ 00002c
 * 外部			12 ~ 17	0x000030 ~ 000044
 * SCI0	ERI0	52		0x0000d0
 * 		RXI0	53		0x0000d4
 * 		TXI0	54		0x0000d8
 * 		TEI0	55		0x0000dc
 * SCI1	ERI1	56		0x0000e0
 * 		RXI1	57		0x0000e4
 * 		TXI1	58		0x0000e8
 * 		TEI1	59		0x0000ec
 * SCI2	ERI2	60		0x0000f0
 * 		RXI2	61		0x0000f4
 * 		TXI2	62		0x0000f8
 * 		TEI2	63		0x0000fc
 */

extern void start(void);
extern void intr_syscall(void);
extern void intr_softerr(void);
extern void intr_serial(void);

#define NULLP (void*)(0)

/* テーブル内容はintr.Sを参照 */
void (*vectors[])(void) =
{
	start, NULLP, NULLP, NULLP, NULLP, NULLP, NULLP, NULLP,														/* 8  */
	intr_syscall, intr_softerr, intr_softerr, intr_softerr, NULLP, NULLP, NULLP, NULLP,							/* 16 */
	NULLP, NULLP, NULLP, NULLP, NULLP, NULLP, NULLP, NULLP,														/* 24 */
	NULLP, NULLP, NULLP, NULLP, NULLP, NULLP, NULLP, NULLP,														/* 32 */
	NULLP, NULLP, NULLP, NULLP, NULLP, NULLP, NULLP, NULLP,														/* 40 */
	NULLP, NULLP, NULLP, NULLP, NULLP, NULLP, NULLP, NULLP,														/* 48 */
	NULLP, NULLP, NULLP, NULLP, intr_serial, intr_serial, intr_serial, intr_serial,								/* 52 */
	intr_serial, intr_serial, intr_serial, intr_serial, intr_serial, intr_serial, intr_serial, intr_serial,		/* 64 */
};
