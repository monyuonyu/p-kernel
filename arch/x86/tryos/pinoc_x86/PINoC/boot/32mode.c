/*
 * 32mode.c
 *
 *  Created on: 2011/12/11
 *      Author: mizu
 */

#include "realmode.h"
#include "32mode.h"

/* waitkeybord */
void waitkeybord()
{
	__asm__(
			"in    $0x64,    %al    \n\t"
			"and   $0x02,    %al    \n\t"
			"jnz   waitkeybord      \n\t"
	);
}


/* io_A20GATE */
void io_A20GATE()
{
	__asm__(
			"movb    $0xff,    %al    \n\t"
			"outb    %al,      $0x21  \n\t"
			"nop                      \n\t"
			"outb    %al,      $0xa1  \n\t"

			"cli                      \n\t"

			"call    waitkeybord    \n\t"
			"movb    $0xd1,   %al    \n\t"
			"outb    %al,     $0x64  \n\t"
			"call    waitkeybord    \n\t"
			"movb    $0xdf,   %al    \n\t"
			"outb    %al,     $0x60  \n\t"
			"call    waitkeybord    \n\t"

	);

}

/* io_PROTECTMODE */
void io_PROTECTMODE()
{

	__asm__(
			"movw	$0x6000,	%ax		\n\t"
			"movw	%ax,		%ds		\n\t"
			"movw	%ax,		%es		\n\t"
			"movw	%ax,		%fs		\n\t"
			"movw	%ax,		%gs		\n\t"

			"movl    %cr0,      %eax	\n\t"
			"orl     $0x01,     %eax	\n\t"
			"movl    %eax,      %cr0	\n\t"
			"jmp     _piplineflash		\n\t"
			"_piplineflash:				\n\t"

			"ljmp	$0x00008,	$0x5800	\n\t"
	);

}

