/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdef.h
 *	システム依存定義（x86 ベアメタルポート依存部）
 */

#ifndef __SYS_SYSDEF_DEPEND_H__
#define __SYS_SYSDEF_DEPEND_H__

/*
 * 割込みベクタ数（IDT は 256 エントリ）
 */
#define N_INTVEC		(256)

/*
 * SVC ハンドラ数（SVC トラップは使用しない。tk_* は直接呼び出し）
 */
#define N_SVCHDR		(0)

/*
 * 低消費電力モード禁止要求の最大ネスト数（tk_set_pow）
 */
#define LOWPOW_LIMIT		(0x7fff)

/*
 * 初期タスクのスタックサイズ [バイト]（include/sys/inittask.h の上書き）
 *	micro T-Kernel 2.0 ポートと同じ 8KB（VFS/FAT32/AI 初期化が
 *	深いスタックを必要とする）。
 */
#define INITTASK_STKSZ		(8 * 1024)

/*
 * システムタイマ周期の許容範囲 [ミリ秒]（knldef.h の検証が参照）
 */
#define MIN_TIMER_PERIOD	(1)
#define MAX_TIMER_PERIOD	(100)

/*
 * CPU 機能の有無（knldef.h の検証が参照）
 */
#define CPU_HAS_PTMR		(0)
#define CPU_HAS_FPU		(0)	/* FPU 退避は side-table 方式（fpu.c）で
					   ディスパッチャが直接フック */
#define CPU_HAS_DSP		(0)

/*
 * タスクのシステムスタックサイズ [バイト]（task_manage.c が参照）
 */
#define MIN_SYS_STACK_SIZE	(1024)
#define DEFAULT_SYS_STKSZ	(MIN_SYS_STACK_SIZE)

/*
 * システムメモリ領域の上端
 *	knl_lowmem_limit に設定する物理アドレス（hw_setting.c が参照）。
 *	micro T-Kernel 2.0 ポート（arch/x86/include/utk_config_depend.h）と
 *	同じ 64MB。
 */
#define X86_SYSTEMAREA_END	(0x04000000)

#endif /* __SYS_SYSDEF_DEPEND_H__ */
