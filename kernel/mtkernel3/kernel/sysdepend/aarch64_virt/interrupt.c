/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel AArch64 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	interrupt.c
 *	割込み制御（AArch64 ベアメタルポート / GICv2）
 *
 *	QEMU virt の GICv2 を初期化し、割込みベクタテーブル
 *	knl_intvec[]（cpu_support.S の IRQ 入口が GICC_IAR の INTID で
 *	引く）を提供します。micro T-Kernel 2.0 ポートの
 *	arch/aarch64/tkdev_init.c（QEMU 側）から移植。
 */

#include <sys/machine.h>
#ifdef AARCH64_VIRT

#include "kernel.h"

/* GICv2 レジスタ（QEMU virt） */
#define GICD_BASE	(0x08000000UL)	/* Distributor */
#define GICC_BASE	(0x08010000UL)	/* CPU インタフェース */
#define GICD_CTLR	(0x000)
#define GICD_ISENABLER	(0x100)
#define GICC_CTLR	(0x000)
#define GICC_PMR	(0x004)

/* cpu_support.S の IRQ 入口が参照する GICC ベース（同ファイル .data） */
IMPORT void *gicc_base_ptr;

/*
 * 割込みベクタテーブル
 *	INTID → 高級言語ハンドラ。未登録は NULL（IRQ 入口が読み飛ばす）。
 */
EXPORT FP knl_intvec[N_INTVEC];

Inline void mmio_write32(unsigned long addr, UW val)
{
	*(volatile UW *)addr = val;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * GIC の単一割込みの有効化（PPI: 16..31 / SPI: 32..）
 *	ドライバ（RTL8139 の SPI 配線等）からも使用されます。
 */
EXPORT void gic_enable_irq(UINT intid)
{
	UINT word = intid >> 5;
	UINT bit  = intid & 31;

	mmio_write32(GICD_BASE + GICD_ISENABLER + word * 4, 1U << bit);
}

/*
 * 割込み管理の初期化
 *	ベクタテーブルのクリアと GICv2（Distributor + CPU I/F）の
 *	有効化を行います。
 */
EXPORT ER knl_init_interrupt( void )
{
	INT i;

	for ( i = 0; i < N_INTVEC; i++ ) {
		knl_intvec[i] = NULL;
	}

	/* cpu_support.S の IRQ 入口へ GICC ベースを公開 */
	*(unsigned long *)&gicc_base_ptr = GICC_BASE;

	mmio_write32(GICD_BASE + GICD_CTLR, 1);		/* Distributor 有効化 */
	mmio_write32(GICC_BASE + GICC_PMR,  0xFF);	/* 全優先度を通す */
	mmio_write32(GICC_BASE + GICC_CTLR, 1);		/* CPU I/F 有効化 */

	return E_OK;
}

/*
 * 割込みハンドラの定義（tk_def_int の実体）
 *	INTID にハンドラを登録します。
 */
EXPORT ER knl_define_inthdr( INT intno, ATR intatr, FP inthdr )
{
	(void)intatr;

	if ( (UINT)intno >= N_INTVEC ) {
		return E_PAR;
	}
	knl_intvec[intno] = inthdr;
	return E_OK;
}

/*
 * 割込みハンドラからの復帰（tk_ret_int の実体）
 *	高級言語ハンドラのみのため何もしません。
 */
EXPORT void knl_return_inthdr( void )
{
}

#endif /* AARCH64_VIRT */
