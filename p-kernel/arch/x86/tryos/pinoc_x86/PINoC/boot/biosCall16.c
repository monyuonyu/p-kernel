/*
 * biosCall16.c
 *
 *  Created on: 2011/12/11
 *      Author: mizu
 */

#include "realmode.h"
#include "biosCall16.h"

/* io_hlt16 */
void io_hlt16()
{
	__asm__("hlt");

}

/* io_fin16 */
void io_fin16()
{
	__asm__(
			"_io_fin16:		\n\t"
			"hlt				\n\t"
			"jmp _io_fin16	\n\t"
	);

}

void io_debug16()
{

	__asm__(
			"pushw	%ax				\n\t"
			"movb	$0x0e,	%ah		\n\t"
			"movb	$1,		%al		\n\t"
			"int	$0x10			\n\t"
			"popw	%ax				\n\t"
	);
}

void io_load16()
{
	__asm__(
			"movw	$0x500,	%di		\n\t"
			"movw	%di,		%es		\n\t"
			"movw    $0x00,     %bx  \n\t"

			"movb    $0x00,     %ch  \n\t"
			"movb    $0x00,     %dh  \n\t"
			"movb    $0x02,     %cl  \n\t"

			"movw    $0x00,     %si  \n\t" // 失敗カウンタ初期化

			"io_load_retry:          \n\t"

			"movb    $0x02,     %ah  \n\t"
			"movb    $0x08,     %al  \n\t" // 連続したセクタr
			"movb    $0x00,     %dl  \n\t"
			"int     $0x13           \n\t" //	セクタ読み込み[bios]

			"jnc     io_load_success	\n\t"

			"add     $0x01,     %si  \n\t" //	失敗回数をカウントアップ
			"cmp     $0x05,     %si  \n\t" // 5回失敗したらエラー終了
			"jae     io_load_error   \n\t"

			"movb    $0x00,     %ah  \n\t"
			"movb    $0x00,     %dl  \n\t"
			"int     $0x13           \n\t"
			"jmp     io_load_retry   \n\t" //	初期化してリトライ
	);

	__asm__("io_load_error:");
#if DEBUG
	io_debug16();
#endif

	__asm__("io_load_success:");
#if DEBUG
	io_debug16();
#endif

}
