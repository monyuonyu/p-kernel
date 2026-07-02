/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	syslib.h
 *	システムライブラリ（x86 ベアメタルポート依存部）
 *
 *	割込み禁止/許可は実際の cli/sti 命令で行います。
 *	disint() は直前の EFLAGS を返し、enaint() は保存された
 *	EFLAGS.IF が立っていた場合のみ sti します（ネスト対応）。
 */

#ifndef __TK_SYSLIB_DEPEND_H__
#define __TK_SYSLIB_DEPEND_H__

#include <tk/errno.h>
#include <sys/sysdef.h>

#define EFLAGS_IF	(1u << 9)	/* EFLAGS の割込み許可フラグ */

/*
 * 割込み禁止
 *	直前の EFLAGS を返してから cli します。
 */
Inline UW disint(void)
{
	UW eflags;
	__asm__ volatile (
		"pushfl\n\t"
		"popl %0\n\t"
		"cli"
		: "=r"(eflags) : : "memory"
	);
	return eflags;
}

/*
 * 割込み許可（disint の戻り値で状態復元）
 */
Inline void enaint(UW eflags)
{
	if ( eflags & EFLAGS_IF ) {
		__asm__ volatile ("sti" : : : "memory");
	}
}

#define DI(intsts)	( (intsts) = (UINT)disint() )
#define EI(intsts)	( enaint((UW)(intsts)) )
#define isDI(intsts)	( ((intsts) & EFLAGS_IF) == 0 )

#endif /* __TK_SYSLIB_DEPEND_H__ */
