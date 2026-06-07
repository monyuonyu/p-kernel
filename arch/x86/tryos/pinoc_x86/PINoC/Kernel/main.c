#include "../../common/typedef.h"
#include "../../common/global.h"
#include "protectmode.h"
#include "lowCall32.h"
#include "vector.h"

void test();

/********************************************************************************
	メモリマップ
	0x40000	: SP
	0x7C00		: bootsct
	0x5000		: bootld
	0x5800		: Kernel
	0x8000		: GDT
	0x8400		: LDT

********************************************************************************/

//void init_pic();

//vo io_sd32(SD* _sd, ui base, ui limit, u1 a, u1 type, u1 s, u1 dpl, u1 p, u1 avl, u1 db, u1 g);
//vo io_write32(si xsize, si ysize, si xSrc, si ySrc, s1 color);
//vo io_hlt32();
//vo io_fin32();
//void io_out8(int port, int data);

__asm__("jmp kernel_ent");

//	簡易カーネル
void main32()
{
	int i;

	__asm__(
			"kernel_ent: 			\n\t"
	);

	// データセグメントの反映
	__asm__(
			"movw	$0x10,		%ax		\n\t"
			"movw	%ax,		%ds		\n\t"
			"movw	%ax,		%fs		\n\t"
			"movw	%ax,		%gs		\n\t"
	);

	// スタックセグメントの反映
	__asm__(
			"movw	$0x10,		%ax		\n\t"
			"movw	%ax,		%ss		\n\t"

			"movl	$0x40000,	%esp	\n\t"
	);

	// プロテクトモードでVGA出力
	*(u1*)0xb8004 = 'C';
	*(u1*)0xb8005 = 0x04;

	// ジャンプ先にHLTLOOPを設定
	*(u1*)0x9500 = (u1)0xf4;
	*(u2*)0x9501 = (u2)0xfdeb;


	// IDTをひとつ設置 (0x21) (testを設置)
//	struct GATE_DIS *gd = (struct GATE_DIS*)(0x8500);
//	gd->offset_low	= (int)test & 0xffff;			// 下位2byte
//	gd->selecta		= 0x18;							// 3セグメント目
//	gd->reserve		= 0;
//	gd->type			= 0x0e;							// 32bit割り込みゲート
//	gd->s				= 0;								// システムディスクリプタ
//	gd->dpl			= 0;								// 特権レベル（最高）
//	gd->p				= 1;								// セグメントはメモリ上に存在する
//	gd->offset_high	= ((int)test >> 16) & 0xffff;	// 上位2byte

//	*(u8*)0x8500 = (u8)0x00009e0000189500;

	idt_init(0x8500);

//	for(i = 0; i < 256; i++)
//	{
//		//i = 0x20;
//		gd[i].offset_low	= (u4)int_keybord & 0xFFFF;			// 下位2byte
//		gd[i].selecta		= 0x08;							//
//		gd[i].reserve		= 0;
//		gd[i].type			= 0x0e;							// 32bit割り込みゲート
//		gd[i].s			= 0;								// セグメントディスクリプタ
//		gd[i].dpl			= 0;								// 特権レベル（最高）
//		gd[i].p			= 1;								// セグメントはメモリ上に存在する
//		gd[i].offset_high	= ((u4)int_keybord >> 16) & 0xFFFF;			// 上位2byte
//	}

	// IDTに登録 テスト
//		i = 0x21;
//		gd[i].offset_low	= (u4)int_damy2 & 0xFFFF;			// 下位2byte
//		gd[i].selecta		= 0x08;							//
//		gd[i].reserve		= 0;
//		gd[i].type			= 0x0e;							// 32bit割り込みゲート
//		gd[i].s			= 0;								// セグメントディスクリプタ
//		gd[i].dpl			= 0;								// 特権レベル（最高）
//		gd[i].p			= 1;								// セグメントはメモリ上に存在する
//		gd[i].offset_high	= ((u4)int_damy2 >> 16) & 0xFFFF;			// 上位2byte
////	}
//
//		i = 0x22;
//		gd[i].offset_low	= (u4)int_damy3 & 0xFFFF;			// 下位2byte
//		gd[i].selecta		= 0x08;							//
//		gd[i].reserve		= 0;
//		gd[i].type			= 0x0e;							// 32bit割り込みゲート
//		gd[i].s			= 0;								// セグメントディスクリプタ
//		gd[i].dpl			= 0;								// 特権レベル（最高）
//		gd[i].p			= 1;								// セグメントはメモリ上に存在する
//		gd[i].offset_high	= ((u4)int_damy3 >> 16) & 0xFFFF;			// 上位2byte
//
//		i = 0x23;
//		gd[i].offset_low	= (u4)int_damy4 & 0xFFFF;			// 下位2byte
//		gd[i].selecta		= 0x08;							//
//		gd[i].reserve		= 0;
//		gd[i].type			= 0x0e;							// 32bit割り込みゲート
//		gd[i].s			= 0;								// セグメントディスクリプタ
//		gd[i].dpl			= 0;								// 特権レベル（最高）
//		gd[i].p			= 1;								// セグメントはメモリ上に存在する
//		gd[i].offset_high	= ((u4)int_damy4 >> 16) & 0xFFFF;			// 上位2byte
//
//		i = 0x24;
//		gd[i].offset_low	= (u4)int_damy5 & 0xFFFF;			// 下位2byte
//		gd[i].selecta		= 0x08;							//
//		gd[i].reserve		= 0;
//		gd[i].type			= 0x0e;							// 32bit割り込みゲート
//		gd[i].s			= 0;								// セグメントディスクリプタ
//		gd[i].dpl			= 0;								// 特権レベル（最高）
//		gd[i].p			= 1;								// セグメントはメモリ上に存在する
//		gd[i].offset_high	= ((u4)int_damy5 >> 16) & 0xFFFF;			// 上位2byte
//
//		i = 0x25;
//		gd[i].offset_low	= (u4)int_damy6 & 0xFFFF;			// 下位2byte
//		gd[i].selecta		= 0x08;							//
//		gd[i].reserve		= 0;
//		gd[i].type			= 0x0e;							// 32bit割り込みゲート
//		gd[i].s			= 0;								// セグメントディスクリプタ
//		gd[i].dpl			= 0;								// 特権レベル（最高）
//		gd[i].p			= 1;								// セグメントはメモリ上に存在する
//		gd[i].offset_high	= ((u4)int_damy6 >> 16) & 0xFFFF;			// 上位2byte
//
//
//		i = 0x26;
//		gd[i].offset_low	= (u4)int_damy7 & 0xFFFF;			// 下位2byte
//		gd[i].selecta		= 0x08;							//
//		gd[i].reserve		= 0;
//		gd[i].type			= 0x0e;							// 32bit割り込みゲート
//		gd[i].s			= 0;								// セグメントディスクリプタ
//		gd[i].dpl			= 0;								// 特権レベル（最高）
//		gd[i].p			= 1;								// セグメントはメモリ上に存在する
//		gd[i].offset_high	= ((u4)int_damy7 >> 16) & 0xFFFF;			// 上位2byte
//
//
//		i = 0x27;
//		gd[i].offset_low	= (u4)int_damy8 & 0xFFFF;			// 下位2byte
//		gd[i].selecta		= 0x08;							//
//		gd[i].reserve		= 0;
//		gd[i].type			= 0x0e;							// 32bit割り込みゲート
//		gd[i].s			= 0;								// セグメントディスクリプタ
//		gd[i].dpl			= 0;								// 特権レベル（最高）
//		gd[i].p			= 1;								// セグメントはメモリ上に存在する
//		gd[i].offset_high	= ((u4)int_damy8 >> 16) & 0xFFFF;			// 上位2byte
//
//
//
//		i = 0x28;
//		gd[i].offset_low	= (u4)int_damy9 & 0xFFFF;			// 下位2byte
//		gd[i].selecta		= 0x08;							//
//		gd[i].reserve		= 0;
//		gd[i].type			= 0x0e;							// 32bit割り込みゲート
//		gd[i].s			= 0;								// セグメントディスクリプタ
//		gd[i].dpl			= 0;								// 特権レベル（最高）
//		gd[i].p			= 1;								// セグメントはメモリ上に存在する
//		gd[i].offset_high	= ((u4)int_damy9 >> 16) & 0xFFFF;			// 上位2byte
//
//
//		i = 0x29;
//		gd[i].offset_low	= (u4)int_damya & 0xFFFF;			// 下位2byte
//		gd[i].selecta		= 0x08;							//
//		gd[i].reserve		= 0;
//		gd[i].type			= 0x0e;							// 32bit割り込みゲート
//		gd[i].s			= 0;								// セグメントディスクリプタ
//		gd[i].dpl			= 0;								// 特権レベル（最高）
//		gd[i].p			= 1;								// セグメントはメモリ上に存在する
//		gd[i].offset_high	= ((u4)int_damya >> 16) & 0xFFFF;			// 上位2byte
//
//
//		i = 0x2a;
//		gd[i].offset_low	= (u4)int_damyb & 0xFFFF;			// 下位2byte
//		gd[i].selecta		= 0x08;							//
//		gd[i].reserve		= 0;
//		gd[i].type			= 0x0e;							// 32bit割り込みゲート
//		gd[i].s			= 0;								// セグメントディスクリプタ
//		gd[i].dpl			= 0;								// 特権レベル（最高）
//		gd[i].p			= 1;								// セグメントはメモリ上に存在する
//		gd[i].offset_high	= ((u4)int_damyb >> 16) & 0xFFFF;			// 上位2byte

//	i = 0x21;
//	gd[i].offset_low	= (u4)int_keybord & 0xFFFF;			// 下位2byte
//	gd[i].selecta		= 0x08;							//
//	gd[i].reserve		= 0;
//	gd[i].type			= 0x0e;							// 32bit割り込みゲート
//	gd[i].s			= 0;								// セグメントディスクリプタ
//	gd[i].dpl			= 0;								// 特権レベル（最高）
//	gd[i].p			= 1;								// セグメントはメモリ上に存在する
//	gd[i].offset_high	= ((u4)int_keybord >> 16) & 0xFFFF;			// 上位2byte

	// PICの初期化
	init_pic();

	// hlt roop
	io_fin32();

}

// キーボード割り込み先
void test()
{
	char key_ascii[0xFF] = {0,};
	key_ascii[0x1E] = 'A';
	key_ascii[0x30] = 'B';
	key_ascii[0x2E] = 'C';

	char a;
//	static int cache = 0xb8006;
//	static int suuzi = 0x30;
	io_out8(PIC0_OCW2, 0x61);
//	*(u1*)cache++ = suuzi++;
//	*(u1*)cache++ = 0x04;

	a = io_in8(0x60);
	*(char*)0xb8006 = key_ascii[a];
	*(char*)0xb8007 = 0x04;

}



