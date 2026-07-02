/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux AArch64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys_timer.h
 *	システムタイマドライバ（Linux AArch64 ユーザモードポート）
 *
 *	周期 tick は POSIX タイマ（SIGALRM, arch/linux/aarch64/preempt.c の
 *	arch_timer_start）が供給します。SIGALRM ハンドラが
 *	knl_timer_handler_startup（dispatch.S）経由で knl_timer_handler を
 *	呼ぶため、ベアメタルの「ハードウェアタイマ割込み」フックは
 *	ほぼ空実装になります。
 */

#ifndef _SYSDEPEND_TARGET_SYSTIMER_
#define _SYSDEPEND_TARGET_SYSTIMER_

/*
 * ホスト側タイマ制御（arch/linux/aarch64/preempt.c）
 *	※ MIN/MAX_TIMER_PERIOD は include/sys/sysdepend/linux_x86_64/
 *	  sysdef.h で定義。
 */
IMPORT void arch_signals_init(void);
IMPORT void arch_timer_start(long usec_period);

/*
 * システムタイマの起動
 *	SIGALRM のシグナルハンドラを登録し、TIMER_PERIOD ミリ秒周期の
 *	POSIX タイマを開始します。knl_timer_startup（timer.c）から
 *	割込み禁止状態で呼ばれます。禁止中に届いた tick は
 *	preempt.c 側で保留され、割込み許可時に再生されます。
 */
Inline void knl_start_hw_timer( void )
{
	arch_signals_init();
	arch_timer_start((long)TIMER_PERIOD * 1000);
}

/*
 * タイマ割込みのクリア（ハンドラ先頭で呼ばれる。本ポートでは不要）
 */
Inline void knl_clear_hw_timer_interrupt( void )
{
}

/*
 * タイマ割込みの終了処理（ハンドラ末尾で呼ばれる。本ポートでは不要）
 */
Inline void knl_end_of_hw_timer_interrupt( void )
{
}

/*
 * システムタイマの停止
 *	プロセス終了時に POSIX タイマも消えるため、明示停止は行いません。
 */
Inline void knl_terminate_hw_timer( void )
{
}

/*
 * 前回タイマ割込みからの経過時間 [ナノ秒]
 *	高精度化が必要になるまで 0 を返します（tick 精度で運用）。
 */
Inline UW knl_get_hw_timer_nsec( void )
{
	return 0;
}

#endif /* _SYSDEPEND_TARGET_SYSTIMER_ */
