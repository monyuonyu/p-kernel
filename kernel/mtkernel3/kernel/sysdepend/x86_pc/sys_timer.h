/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys_timer.h
 *	システムタイマドライバ（x86 ベアメタルポート / 8254 PIT）
 *
 *	IRQ0（PIT）を 100Hz で駆動し、boot/x86/idt.c の
 *	x86_irq_handlers[0] 経由で knl_timer_handler_startup（dispatch.S）
 *	へ届けます。EOI はハンドラ先頭（knl_clear_hw_timer_interrupt）で
 *	先送りします — END_CRITICAL_SECTION のディスパッチで IRQ ハンドラへ
 *	戻らないことがあるためです。
 */

#ifndef _SYSDEPEND_TARGET_SYSTIMER_
#define _SYSDEPEND_TARGET_SYSTIMER_

/* 8254 PIT */
#define PIT_BASE_HZ	(1193182UL)
#define PIT_CH0		(0x40)
#define PIT_CMD		(0x43)

/* I/O ポートアクセス */
Inline void knl_outb(UH port, UB val)
{
	__asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
Inline UB knl_inb(UH port)
{
	UB ret;
	__asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
	return ret;
}

/* boot/x86 側の IRQ ディスパッチテーブルと PIC 制御
 *	pic_* は regparm(1)（引数を EAX で受ける）— boot/x86/pic.c の
 *	定義と呼び出し規約を一致させること。 */
IMPORT void (*x86_irq_handlers[16])(void);
IMPORT void __attribute__((regparm(1))) pic_unmask_irq(UB irq);

/*
 * システムタイマの起動
 *	PIT を TIMER_PERIOD ミリ秒周期（100Hz）に設定し、IRQ0 の
 *	ハンドラを登録してアンマスクします。
 */
Inline void knl_start_hw_timer( void )
{
	UINT div = (UINT)(PIT_BASE_HZ * (unsigned long)TIMER_PERIOD / 1000UL);

	/* Ch0, lobyte/hibyte, mode 3（レートジェネレータ）, バイナリ */
	knl_outb(PIT_CMD, 0x36);
	knl_outb(PIT_CH0, (UB)(div & 0xFF));
	knl_outb(PIT_CH0, (UB)((div >> 8) & 0xFF));

	x86_irq_handlers[0] = knl_timer_handler_startup;
	pic_unmask_irq(0);

}

/*
 * タイマ割込みのクリア（ハンドラ先頭で呼ばれる）
 *	IRQ0 の specific EOI をマスタ PIC へ送る。ディスパッチで
 *	ハンドラへ戻れなくても EOI 済みにするための先送り。
 */
Inline void knl_clear_hw_timer_interrupt( void )
{
	knl_outb(0x20, 0x60);	/* specific EOI (level 0 = IRQ0) */
}

/*
 * タイマ割込みの終了処理（EOI は先頭で送信済みのため何もしない）
 */
Inline void knl_end_of_hw_timer_interrupt( void )
{
}

/*
 * システムタイマの停止（IRQ0 をマスク）
 */
Inline void knl_terminate_hw_timer( void )
{
	knl_outb(0x21, (UB)(knl_inb(0x21) | 0x01));
}

/*
 * 前回タイマ割込みからの経過時間 [ナノ秒]
 *	PIT のカウンタをラッチして tick 内の経過を算出します。
 */
Inline UW knl_get_hw_timer_nsec( void )
{
	UINT count, period_counts, elapsed;

	knl_outb(PIT_CMD, 0x00);	/* Ch0 ラッチ */
	count  = (UINT)knl_inb(PIT_CH0);
	count |= (UINT)knl_inb(PIT_CH0) << 8;

	period_counts = (UINT)(PIT_BASE_HZ * (unsigned long)TIMER_PERIOD / 1000UL);
	elapsed = period_counts - count;

	return (UW)((elapsed * 1000UL) / (PIT_BASE_HZ / 1000000UL));
}

#endif /* _SYSDEPEND_TARGET_SYSTIMER_ */
