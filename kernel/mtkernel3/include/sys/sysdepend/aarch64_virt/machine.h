/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel AArch64 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	machine.h
 *	マシン種別定義（AArch64 ベアメタルポート依存部）
 *
 *	QEMU virt（Cortex-A53 / GICv2 / PL011）で EL1 実行する
 *	ベアメタルターゲットです（Raspberry Pi 3 も同系）。
 *	MMU は無効（VA==PA）、割込みは DAIF、tick は ARM generic timer。
 */

#ifndef __SYS_SYSDEPEND_MACHINE_H__
#define __SYS_SYSDEPEND_MACHINE_H__

/* ----- AArch64 ベアメタル（QEMU virt）ターゲット定義 ----- */

#define AARCH64_VIRT		1	/* ターゲットシステム : AArch64 QEMU virt */
#define CPU_AARCH64		1	/* ターゲット CPU : AArch64 (ARMv8-A) */
#define CPU_CORE_AARCH64	1	/* ターゲット CPU コア : AArch64 */

#define TARGET_DIR		aarch64_virt	/* sysdepend ディレクトリ名 */
#define KNL_SYSDEP_PATH		aarch64_virt	/* カーネル sysdepend パス */

/*
 **** CPU 依存プロファイル（AArch64 ベアメタル）
 */
#define ALLOW_MISALIGN		0	/* MMU 無効の Device メモリでは非整列不可
					   （ビルドも -mstrict-align） */
#define BIGENDIAN		0	/* リトルエンディアン */
#define INT_BITWIDTH		32	/* INT 型のビット幅 */

/*
 * p-kernel 拡張の固定幅整数型（アプリ層互換用）
 */
#ifndef _in_asm_source_
typedef signed char	S1;	/* 符号付き  8 ビット整数 */
typedef signed short	S2;	/* 符号付き 16 ビット整数 */
typedef signed int	S4;	/* 符号付き 32 ビット整数 */
typedef unsigned char	U1;	/* 符号なし  8 ビット整数 */
typedef unsigned short	U2;	/* 符号なし 16 ビット整数 */
typedef unsigned int	U4;	/* 符号なし 32 ビット整数 */
typedef void		*VP;	/* 不定形データへのポインタ（T-Kernel 2.0 互換） */
#endif /* _in_asm_source_ */

#endif /* __SYS_SYSDEPEND_MACHINE_H__ */
