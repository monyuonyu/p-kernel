/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Windows x86-64 ネイティブポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys_timer.h
 *	システムタイマドライバ（Windows x86-64 ネイティブポート）
 *
 *	Linux 版は SIGALRM（POSIX タイマ）が周期 tick を供給しますが、
 *	Windows v1 は協調スケジューラのため、周期割込みは使いません。
 *	tick は dispatch.c が安全点（アイドル/ディスパッチ）で
 *	QueryPerformanceCounter に追従してポンプします。ここでは tick の
 *	基準時刻を設定するだけです。
 */

#ifndef _SYSDEPEND_TARGET_SYSTIMER_
#define _SYSDEPEND_TARGET_SYSTIMER_

/* dispatch.c: tick 基準時刻の設定 */
IMPORT void knl_win_timer_start(void);

/*
 * システムタイマの起動（knl_timer_startup から割込み禁止状態で呼ばれる）
 */
Inline void knl_start_hw_timer( void )
{
	knl_win_timer_start();
}

Inline void knl_clear_hw_timer_interrupt( void )
{
}

Inline void knl_end_of_hw_timer_interrupt( void )
{
}

Inline void knl_terminate_hw_timer( void )
{
}

/*
 * 前回タイマ割込みからの経過時間 [ナノ秒]
 *	tick 精度で運用するため 0 を返します。
 */
Inline UW knl_get_hw_timer_nsec( void )
{
	return 0;
}

#endif /* _SYSDEPEND_TARGET_SYSTIMER_ */
