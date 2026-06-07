#include "../../common/typedef.h"
#include "../../common/global.h"

#include "realmode.h"
#include "lowCall16.h"
#include "biosCall16.h"
#include "32mode.h"

//#include "realmode.h"
vo main16()
{
//	while(1);

//	io_fin16();

//	__asm__(
//			"movb    $0x0e,   %ah  \n\t"
//			"movb    $3   ,   %al  \n\t"
//			"int     $0x10         \n\t"
//	);




	// VBEへ
//	io_graphic16(1);

	// メモリの使用可能領域を2^32まで拡張
	io_A20GATE();
//	RGB color;
//	color.B = 0xFF;
//	color.G = 0xCC;
//	color.R = 0xCC;
//
//	RGB color2;
//	color2.B = 0x99;
//	color2.G = 0x99;
//	color2.R = 0x99;
//
//	RGB color3;
//	color3.B = 0x66;
//	color3.G = 0x66;
//	color3.R = 0x66;

	// 1024x768	24bit/32bit
//	io_RGBwrite16(1024, 768, 0, 0, color);
//	io_RGBwrite16(1024, 1, 0, 768 - 27, color3);
//	io_RGBwrite16(1024, 26, 0, 768 - 26, color2);
//	io_RGBwrite16(64, 22, 2, 768 - 26 + 1, color3);

	// ジャンプ先にHLTLOOPを設定
//	*(u1*)0x6500 = (u1)0xf4;
//	*(u2*)0x6501 = (u2)0xfdeb;

//	*(u1*)0x5600 = (u1)0xf4;
//	*(u2*)0x5601 = (u2)0xfdeb;

	*(u8*)0x9008 = (u8)0x00cf9a000000FFFF;	// kernelコードセグメントの設置
	*(u8*)0x9010 = (u8)0x00cf92000000FFFF;	// kernelデータセグメントの設置
//	*(u8*)0x9018 = (u8)0x00cf9a000000FFFF;	// 割り込みコードセグメントの設置
//	*(u8*)0x9020 = (u8)0x00cf92000000FFFF;	// 割り込みデータセグメントの設置

//	__asm__(
//			"movw	$0x00cf92000000FFFF,		%ds		\n\t"
//	);


	// GDTRとIDTRの設定をします。  -> 6byte
	LGDT16(0x00FF, 0x00009000);
	LIDT16(0x00FF, 0x00008500);

//	*(u1*)0x5600 = (u1)0xf4;
//	*(u2*)0x5601 = (u2)0xfdeb;

//	io_fin16();

	*(u1*)0xb8000 = 'A';
	*(u1*)0xb8001 = 0x04;

	*(u1*)0xb8002 = 'B';
	*(u1*)0xb8003 = 0x04;

//	io_fin16();

	// パイプラインフラッシュの後、スタックを設定し、main32()へjmpします
	io_PROTECTMODE();

}


//#include "protectmode.h"
//
//__asm__(
//		"movw	$0x6000,	%ax		\n\t"
//		"movw	%ax,		%ds		\n\t"
//		"movw	%ax,		%es		\n\t"
//		"movw	%ax,		%fs		\n\t"
//		"movw	%ax,		%gs		\n\t"
//
//);
//
//#include "lowCall32.h"
//
//vo main32()
//{
//
//	__asm__("nop");
//	while (1)
//	{
//		__asm__("hlt");
////				io_hlt32();
//	}
//}
