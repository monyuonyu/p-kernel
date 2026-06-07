/*
 * lowCall16.c
 *
 *  Created on: 2011/12/11
 *      Author: mizu
 */


#include "../../common/typedef.h"
#include "../../common/global.h"
#include "../../lib/string.h"
#include "realmode.h"

void io_printf16(s1 _Src[][8])
{
	si si_i, si_ii;
//	RGBDATA24* dst = (RGBDATA24*)0xF0000000;
//
//	RGBDATA24 color;
//	color.B = 0xFF;
//	color.G = 0xFF;
//	color.R = 0xFF;

	__asm__("nop");
	if (_Src[0][0] == 0){
		__asm__("nop");
//		dst[0] = color;
	}

//	dst[1] = color;

	// 8x16ドットの文字を描く
//		FOR(si_i, 16)
//		{
//			FOR(si_ii, 8)
//			{
//				if (_Src[si_i][si_ii])
//					__asm__("nop");
//					dst[si_ii] = color;
//			}
//			dst += 1024;
//		}
}


void io_RGBwrite16(si xsize, si ysize, si xSrc, si ySrc, RGB rgb)
{
	si si_i, si_ii;

	RGB* dst = (RGB*)graphicmode.vram;

	dst += graphicmode.xsize * ySrc;
	dst += xSrc;

	switch(graphicmode.color_bit)
	{
	case 24:
		FOR(si_i, ysize)
		{
			FOR(si_ii, xsize)
			{
				__asm__("nop");
				dst[si_ii] = rgb;
			}
			dst += graphicmode.xsize;
		}
		break;
	case 32:
		FOR(si_i, ysize)
		{
			FOR(si_ii, xsize)
			{
//				(u1*)dst += 4;
				__asm__("nop");
				dst[si_ii] = rgb;
			}
			dst += graphicmode.xsize;
		}
		break;

	}
}

/* io_graphic */
void io_graphic16(u1 mode)
{

	switch (mode)
	{
	// VGAモード
	case 1:
		__asm__(
				"movb    $0x00,    %ah	\n\t"
				"movb    $0x13,    %al	\n\t"
				"int     $0x10			\n\t"
		);
		graphicmode.vram = (s1*) 0xa0000;
		graphicmode.xsize = 320;
		graphicmode.ysize = 200;
		graphicmode.color_bit = 0x08;
		break;

		// VBEモード
	case 2:
		__asm__(
				"push	%es					\n\t"
				"push	%di					\n\t"

				// 情報格納先アドレスセット VESA情報
				"movw	$0x800,	%ax		\n\t"
				"movw	%ax,		%es		\n\t"
				"movw	$0x00,		%di		\n\t"

				// VBE情報獲得
				"movw	$0x4f00,	%ax		\n\t"
				"int	$0x10				\n\t"
				"cmpw	$0x004f,	%ax		\n\t" // ahが0 -> 正常終了 alが4f->VESAサポート
				"jnz	io_graphic_1		\n\t" // VGAモードを設定
		);

		__asm__(
				// 情報格納先アドレスセット VBE情報
				"movw	$0x810,	%ax		\n\t"
				"movw	%ax,		%es		\n\t"
				"movw	$0x00,		%di		\n\t"

				// 画面モード情報獲得
				"movw	$0x4f01,	%ax		\n\t"
				"movw	$0x0118,	%cx		\n\t" // 1024x768	24bit/32bit
				"int	$0x10				\n\t"
				"cmpw	$0x004f,	%ax		\n\t" // ahが0 -> 正常終了 alが4f->VESAサポート
				"jnz	io_graphic_1		\n\t" // VGAモードを設定
		);

		// ビットカラー情報確保 1byte
		u1* src1 = (u1*) 0x8119;
		graphicmode.color_bit = *src1;

		// vram開始番地格納 4byte
		u4* src2 = (u4*) 0x8128;
		graphicmode.vram = (s1*) *src2; // メモリの4byteの値を1byteのポインタへキャスト

		// 解像度 4byte
		graphicmode.xsize = 1024;
		graphicmode.ysize = 768;

		__asm__(
				// 画面モードを設定
				"movw	$0x4f02,	%ax		\n\t"
				"movw	$0x0118,	%bx		\n\t"
				"orw	$0x4000,	%bx		\n\t"
				"int	$0x10				\n\t"
				"cmpw	$0x004f,	%ax		\n\t" // ahが0 -> 正常終了 alが4f->VESAサポート
				"jnz	io_graphic_1		\n\t" // VGAモードを設定

				"pop	%di					\n\t"
				"pop	%es					\n\t"

				"movl	$0xE0000000,	%eax	\n\t"
				"movl	$0xFFFFFF00,	(%eax)	\n\t"
		);

		break;
	default:
		__asm__("io_graphic_1:");
		io_graphic16(1);
		break;
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
void io_sd16(SD* _sd, ui base, ui limit, u1 a, u1 type, u1 s, u1 dpl, u1 p, u1 avl,
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

void LGDT16(u2 Limit, u4 Base)
{
	u1* _LGDT = (u1*)0x8000;	// 8000は気まぐれ

	// 8000番地にLimitのパラメータを格納
	*(u2*)_LGDT = Limit;		// GDTの大きさ

	_LGDT += 2;	// 2byteカウントアップ
	*(u4*)_LGDT = Base;		// GDTの開始アドレス

	// GDTRに格納
	__asm__(
			"LGDT 0x8000"
	);

	// 以後、GDTが利用可能
}

void LIDT16(u2 Limit, u4 Base)
{
	u1* _LGDT = (u1*)0x8400;	// 8400は気まぐれ

	// 8400番地にLimitのパラメータを格納
	*(u2*)_LGDT = Limit;

	_LGDT += 2;	// 2byteカウントアップ
	*(u4*)_LGDT = Base;

	// GDTRに格納
	__asm__(
			"LIDT 0x8400"
	);
}
