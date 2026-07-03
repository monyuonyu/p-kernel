/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux x86-64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	syslib.h
 *	システムライブラリ（Linux x86-64 ユーザモードポート依存部）
 *
 *	割込み禁止/許可は CPU 命令ではなくソフトウェアフラグ
 *	arch_irq_disabled_flag（arch/linux/x86_64/preempt.c）への
 *	読み書きで実現します。SIGALRM ハンドラはこのフラグが立って
 *	いる間は tick 処理を保留し、enaint() 時にまとめて再生します。
 */

#ifndef __TK_SYSLIB_DEPEND_H__
#define __TK_SYSLIB_DEPEND_H__

#include <tk/errno.h>
#include <sys/sysdef.h>

/*
 * ソフトウェア割込みフラグ（実体は preempt.c）
 */
IMPORT volatile int arch_irq_disabled_flag;
IMPORT void arch_irq_enable_with_drain(void);

/*
 * 割込み禁止
 *	直前のフラグ状態を返します（0=許可されていた / 非0=既に禁止）。
 */
Inline UW disint(void)
{
	UW prev = (UW)arch_irq_disabled_flag;
	arch_irq_disabled_flag = 1;
	return prev;
}

/*
 * 割込み許可（disint の戻り値で状態復元）
 *	呼び出し時点の保存状態が「許可」だった場合のみ解放します。
 *	これにより DI/EI のネストが正しく釣り合います。
 */
Inline void enaint(UW intsts)
{
	if (intsts == 0) {
		arch_irq_enable_with_drain();
	}
}

#define DI(intsts)	( (intsts) = (UINT)disint() )
#define EI(intsts)	( enaint((UW)(intsts)) )
#define isDI(intsts)	( (intsts) != 0 )

#endif /* __TK_SYSLIB_DEPEND_H__ */
