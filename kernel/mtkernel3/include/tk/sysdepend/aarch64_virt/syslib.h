/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel AArch64 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	syslib.h
 *	システムライブラリ（AArch64 ベアメタルポート依存部）
 *
 *	割込み禁止/許可は DAIF（IRQ=I ビット、FIQ=F ビット）で行います。
 */

#ifndef __TK_SYSLIB_DEPEND_H__
#define __TK_SYSLIB_DEPEND_H__

#include <tk/errno.h>
#include <sys/sysdef.h>

#define DAIF_F		(1u << 6)	/* FIQ マスク */
#define DAIF_I		(1u << 7)	/* IRQ マスク */

/*
 * 割込み禁止
 *	直前の DAIF を返してから IRQ+FIQ をマスクします。
 */
Inline UW disint(void)
{
	UW daif;
	__asm__ volatile (
		"mrs %0, daif\n\t"
		"msr daifset, #0x3"
		: "=r"(daif) : : "memory"
	);
	return daif;
}

/*
 * 割込み許可（disint の戻り値で状態復元）
 */
Inline void enaint(UW daif)
{
	if ( !(daif & DAIF_I) ) {
		__asm__ volatile ("msr daifclr, #0x3" : : : "memory");
	}
}

#define DI(intsts)	( (intsts) = (UINT)disint() )
#define EI(intsts)	( enaint((UW)(intsts)) )
#define isDI(intsts)	( ((intsts) & DAIF_I) != 0 )

/*
 * GIC 割込み許可（ドライバが SPI を配線する際に使用。interrupt.c）
 */
IMPORT void gic_enable_irq(UINT intid);

#endif /* __TK_SYSLIB_DEPEND_H__ */
