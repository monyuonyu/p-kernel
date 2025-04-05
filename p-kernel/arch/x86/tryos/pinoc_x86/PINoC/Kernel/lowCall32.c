/*
 * lowCall32.c
 *
 *  Created on: 2011/12/12
 *      Author: mizu
 */


#include "../../common/typedef.h"
#include "../../common/global.h"
#include "../../lib/string.h"
#include "protectmode.h"
#include "lowCall32.h"

/* io_hlt32 */
__asm__(
		".globl io_hlt32				\n\t"
		".type	io_hlt32, @function	\n\t"
		"io_hlt32:						\n\t"
		"hlt    						\n\t"
		"ret							\n\t"
);

/* io_fin32 */
__asm__(
		".globl io_fin32				\n\t"
		".type	io_fin32, @function	\n\t"
		"io_fin32:						\n\t"
		"hlt							\n\t"
		"jmp io_fin32					\n\t"
);


/* io_out8 */
__asm__(
		".globl io_out8				\n\t"
		".type	io_out8, @function	\n\t"
		"io_out8:				\n\t"
		"movl 4(%esp), %edx	\n\t"
		"movl 8(%esp), %eax	\n\t"
		"outb %al ,%dx 		\n\t"
		"ret					\n\t"
);

/* io_in8 */
__asm__(
		".globl io_in8				\n\t"
		".type	io_in8, @function	\n\t"
		"io_in8:				\n\t"
		"movl 4(%esp), %edx	\n\t"
		"movl $0x00,	%eax	\n\t"
		"inb %dx ,%al 		\n\t"
		"ret					\n\t"
);


/* LJMP */
//__asm__(
//		"test_cs		=	0x08	\n\t"
//		"test_offset	=	0x00	\n\t"
//		"LJMP:						\n\t"
//		"ljmp	$test_cs,	$test_offset	\n\t"	// セグメント2個め、オフセット0
//);

vo io_write32(si xsize, si ysize, si xSrc, si ySrc, s1 color)
{

	s1* s1_dst = graphicmode.vram;
	s1_dst += graphicmode.xsize * ySrc;
	s1_dst += xSrc;

	si si_i, si_ii;

	FOR(si_i, ysize)
	{
		FOR(si_ii, xsize)
		{
			s1_dst[si_ii] = color;
		}
		s1_dst += graphicmode.xsize;
	}
}

/* セグメントディスクリプタを生成
 31                                      1615                                      00
 ┌─────────────────────┬─────────────────────┐
                  base                               Limit
 ├────────┬─┬─┬─┬─┬────┼─┬──┬─┬───┬─┬────────┤
          base      G  DB  予 AVL  Limit     P  DPL  S   Type  A       Base
 └────────┴─┴─┴─┴─┴────┴─┴──┴─┴───┴─┴────────┘
 ※リトルエンディアンに注意
 */
vo io_sd32(SD* _sd, ui base, ui limit, u1 a, u1 type, u1 s, u1 dpl, u1 p, u1 avl,
		u1 db, u1 g)
{
	memset(_sd, 0x00, sizeof(_sd));

	_sd->g = g;

	_sd->db = db;

	_sd->avl = avl;

	// セグメント存在フラグ
	_sd->p = p;

	// 特権レベル
	_sd->dpl = dpl;

	// ディスクリプタ・タイプ
	_sd->s = s;

	// セグメント・タイプ
	_sd->type = type;

	// アクセス済みフラグ
	_sd->a = a;

}


void init_pic()
{

	__asm__("cli");

	io_out8(PIC0_IMR, 0xff);
	io_out8(PIC1_IMR, 0xff);

	io_out8(PIC0_ICW1, 0x11);
	io_out8(PIC0_ICW2, 0x20);
	io_out8(PIC0_ICW3, 1 << 2);
	io_out8(PIC0_ICW4, 0x01);

	io_out8(PIC1_ICW1, 0x11);
	io_out8(PIC1_ICW2, 0x28);
	io_out8(PIC1_ICW3, 0x2);
	io_out8(PIC1_ICW4, 0x01);

	io_out8(PIC0_IMR, 0xfd);
	io_out8(PIC1_IMR, 0xff);

	__asm__("sti");
}
