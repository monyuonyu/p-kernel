/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel AArch64 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys_timer.h
 *	システムタイマドライバ
 *	（AArch64 ベアメタルポート / ARM generic timer + GICv2）
 *
 *	EL1 物理タイマ（cntp_*、PPI INTID=30）を TIMER_PERIOD ミリ秒
 *	周期で駆動します。CNTFRQ_EL0 は QEMU virt で 62.5MHz。
 */

#ifndef _SYSDEPEND_TARGET_SYSTIMER_
#define _SYSDEPEND_TARGET_SYSTIMER_

#define INTNO_TIMER_GIC		(30)	/* EL1 物理タイマの PPI */

IMPORT void gic_enable_irq(UINT intid);

/*
 * タイマ周期のリロード値 [カウント]
 */
Inline unsigned long knl_hw_timer_interval( void )
{
	unsigned long freq;
	__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
	return freq / 1000UL * (unsigned long)TIMER_PERIOD;
}

/*
 * タイマ割込みハンドラ（interrupt.c のベクタ経由で IRQ 入口から呼ばれる）
 *	カウントダウンをリロードして knl_timer_handler_startup
 *	（cpu_support.S — knl_taskindp を増減して knl_timer_handler を
 *	呼ぶ）へ橋渡しします。
 */
IMPORT void knl_timer_handler_startup(void);

LOCAL void knl_hw_timer_irq(void)
{
	unsigned long interval = knl_hw_timer_interval();
	__asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(interval));
	__asm__ volatile ("dsb sy" ::: "memory");

	knl_timer_handler_startup();
}

/*
 * システムタイマの起動
 *	タイマ PPI のハンドラ登録・GIC 有効化・カウント開始を行います。
 */
Inline void knl_start_hw_timer( void )
{
	unsigned long interval = knl_hw_timer_interval();

	knl_define_inthdr(INTNO_TIMER_GIC, TA_HLNG, (FP)knl_hw_timer_irq);
	gic_enable_irq(INTNO_TIMER_GIC);

	__asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(interval));
	__asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(1UL));
	__asm__ volatile ("dsb sy" ::: "memory");
	__asm__ volatile ("isb" ::: "memory");
}

/*
 * タイマ割込みのクリア（EOI は cpu_support.S の IRQ 入口が GICC_EOIR で
 * 行うため、ここでは何もしません）
 */
Inline void knl_clear_hw_timer_interrupt( void )
{
}

/*
 * タイマ割込みの終了処理
 */
Inline void knl_end_of_hw_timer_interrupt( void )
{
}

/*
 * システムタイマの停止
 */
Inline void knl_terminate_hw_timer( void )
{
	__asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(0UL));
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
