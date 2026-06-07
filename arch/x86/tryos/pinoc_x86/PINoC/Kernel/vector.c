/*
 * vector.c
 *
 *  Created on: 2012/03/04
 *      Author: mizu
 */

#include "vector.h"
#include "lowCall32.h"

void int_DE();
void int_DB();
void int_NMI();
void int_BP();
void int_OF();
void int_BR();
void int_UD();
void int_NM();
void int_DF();
void int_TS();
void int_NP();
void int_SS();
void int_GP();
void int_PF();
void int_MF();
void int_AC();
void int_MC();
void int_XF();
void int_dummy0();
void int_dummy1();

// 0 除算ERROR
__asm__(
		"int_DE:		\n\t"

		// 一文字出力
		"movb $'1', (0xb8008) \n\t"
		"movb $0x04, (0xb8009) \n\t"

		"iret \n\t"
);

// 1 デバッグ(Intelが使用)
__asm__(
		"int_DB:		\n\t"
		"iret \n\t"
);

// 2 NMI割り込み
__asm__(
		"int_NMI:		\n\t"
		"iret \n\t"
);

// 3 ブレークポイント(INT3命令)
__asm__(
		"int_BP:		\n\t"
		"iret \n\t"
);

// 4 オーバーフロー (INTO命令)
__asm__(
		"int_OF:		\n\t"
		"iret \n\t"
);

// 5 BOUNDの範囲外(BOUND命令)
__asm__(
		"int_damy6:		\n\t"
		"iret \n\t"
);

// 6 無効オペコード
__asm__(
		"int_UD:		\n\t"
		"iret \n\t"
);

// 7 デバイス使用不可
__asm__(
		"int_NM:		\n\t"
		"iret \n\t"
);

// 8 ダブルフォルト
__asm__(
		"int_DF:		\n\t"
		"iret \n\t"
);

// 9 予約済み
__asm__(
		"int_damy0:		\n\t"
		"iret \n\t"
);

// 10 無効TSS
__asm__(
		"int_TS:		\n\t"
		"iret \n\t"
);

// 11 セグメント不在
__asm__(
		"int_NP:		\n\t"
		"iret			\n\t"
);

// 12 スタックセグメントフォルト
__asm__(
		"int_SS:		\n\t"
		"iret 			\n\t"
);

// 13 一般保護
__asm__(
		"int_GP:		\n\t"
		"iret 			\n\t"
);

// 14 ページフォルト
__asm__(
		"int_PF:		\n\t"
		"iret 			\n\t"
);

// 15 予約済み
__asm__(
		"int_dummy1:		\n\t"
		"iret 			\n\t"
);


// 16 x87 FPU 浮動小数点エラー
__asm__(
		"int_MF:		\n\t"
		"iret 			\n\t"
);


// 17 アライメントチェック
__asm__(
		"int_AC:		\n\t"
		"iret 			\n\t"
);

// 18 マシンチェック
__asm__(
		"int_MC:		\n\t"
		"iret 			\n\t"
);


// 19 SIMD浮動小数点例外
__asm__(
		"int_XF:		\n\t"
		"iret 			\n\t"
);


// 0x21 キーボード割り込み
__asm__(
		"int_keybord:		\n\t"

		"pushl	%ds			\n\t"
		"pushl	%es			\n\t"
		"pushal			\n\t"

//		"movl	%esp, %eax		\n\t"
//		"pushl	%eax			\n\t"

		"movl	%ss, %eax		\n\t"
		"movl	%eax, %ds		\n\t"
		"movl	%eax, %es		\n\t"

//		"aiueo2:	\n\t"
//		"hlt		\n\t"
//		"jmp aiueo2	\n\t"

		"call	test		\n\t"

		"pushl $0xFFFFFFFF		\n\t"
		"popl 	%eax		\n\t"

//		"popl	%eax		\n\t"

		"popal				\n\t"
		"popl	%es			\n\t"
		"popl	%ds			\n\t"

		"aiueo:	\n\t"
		"hlt		\n\t"
		"jmp aiueo	\n\t"

		"popl %eax		\n\t"
		"iret				\n\t"
);



void set_idt_int(unsigned int idt_ent,int idt_no, unsigned int func_name)
{
//	idt_ent += idt_no;
//	idt_ent = func_name;

	struct GATE_DIS *gd = (struct GATE_DIS*)(0x8500);

	gd[idt_no].offset_low	= func_name & 0xFFFF;			// 下位2byte
//	gd[idt_no].selecta		= 0x08;							//
//	gd[idt_no].reserve		= 0;
//	gd[idt_no].type			= 0x0e;							// 32bit割り込みゲート
//	gd[idt_no].s			= 0;								// セグメントディスクリプタ
//	gd[idt_no].dpl			= 0;								// 特権レベル（最高）
//	gd[idt_no].p			= 1;								// セグメントはメモリ上に存在する
//	gd[idt_no].offset_high	= ((unsigned int)func_name >> 16) & 0xFFFF;			// 上位2byte

}

void idt_init(unsigned int idt_ent)
{
//	set_idt_int(idt_ent, 0, int_DE);
//	set_idt_int(idt_ent, 1, int_DB);
//	set_idt_int(idt_ent, 2, int_NMI);
//	set_idt_int(idt_ent, 3, int_BP);
//	set_idt_int(idt_ent, 4, int_OF);
//	set_idt_int(idt_ent, 5, int_BR);
//	set_idt_int(idt_ent, 6, int_UD);
//	set_idt_int(idt_ent, 7, int_NM);
//	set_idt_int(idt_ent, 8, int_DF);
//	set_idt_int(idt_ent, 9, int_dummy0);
//	set_idt_int(idt_ent, 10, int_TS);
//	set_idt_int(idt_ent, 11, int_NP);
//	set_idt_int(idt_ent, 12, int_SS);
//	set_idt_int(idt_ent, 13, int_GP);
//	set_idt_int(idt_ent, 14, int_PF);
//	set_idt_int(idt_ent, 15, int_dummy1);
//	set_idt_int(idt_ent, 16, int_MF);
//	set_idt_int(idt_ent, 17, int_AC);
//	set_idt_int(idt_ent, 18, int_MC);
//	set_idt_int(idt_ent, 19, int_XF);
}
