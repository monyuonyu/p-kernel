#include "../../common/typedef.h"
#include "realmode.h"
#include "biosCall16.h"

/* スタックを初期化 */
/* 「スタックセグメントの設定」 */
__asm__(
		"movw	$0x0400, 	%ax		\n\t"
		"movw	%ax,		%ss		\n\t"
		"movl	$0x0,		%eax	\n\t"
		"movl	%eax,		%esp	\n\t"
		"movl	%eax,		%ebp	\n\t"
);

void main16() {


	io_load16();


#if DEBUG
	io_debug16();
#endif

	// 関数の途中で抜けるのでスタックを戻しておく
	__asm__("popl	%ebx");

//	io_fin16();

//	*(u1*)0x5000 = (u1)0xf4;
//	*(u2*)0x5001 = (u2)0xfdeb;


	// ブートローダへjamp
	__asm__("jmp	0x5000");

}

