/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel AArch64 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdef.h
 *	システム依存定義（AArch64 ベアメタルポート依存部）
 */

#ifndef __SYS_SYSDEF_DEPEND_H__
#define __SYS_SYSDEF_DEPEND_H__

/*
 * 割込みベクタ数（GICv2 の INTID 空間に合わせる）
 */
#define N_INTVEC		(512)

/*
 * SVC ハンドラ数（SVC トラップは使用しない）
 */
#define N_SVCHDR		(0)

/*
 * 低消費電力モード禁止要求の最大ネスト数（tk_set_pow）
 */
#define LOWPOW_LIMIT		(0x7fff)

/*
 * 初期タスクのスタックサイズ [バイト]
 *	micro T-Kernel 2.0 ポートと同じ 8KB。
 */
#define INITTASK_STKSZ		(8 * 1024)

/*
 * システムタイマ周期の許容範囲 [ミリ秒]
 */
#define MIN_TIMER_PERIOD	(1)
#define MAX_TIMER_PERIOD	(100)

/*
 * CPU 機能の有無（knldef.h の検証が参照）
 */
#define CPU_HAS_PTMR		(0)
#define CPU_HAS_FPU		(0)	/* CPACR で常時有効化（コンテキスト退避は
					   協調切替のため callee-saved のみ） */
#define CPU_HAS_DSP		(0)

/*
 * タスクのシステムスタックサイズ [バイト]
 */
#define MIN_SYS_STACK_SIZE	(1024)
#define DEFAULT_SYS_STKSZ	(MIN_SYS_STACK_SIZE)

/*
 * システムメモリ領域の上端（hw_setting.c が knl_lowmem_limit に設定）
 *	QEMU virt: RAM は 0x40000000 起点、256MB 構成の既定で 0x50000000。
 */
#define AARCH64_SYSTEMAREA_END	(0x50000000UL)

#endif /* __SYS_SYSDEF_DEPEND_H__ */
