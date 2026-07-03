/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	machine.h
 *	マシン種別定義（x86 ベアメタルポート依存部）
 *
 *	QEMU（-kernel bootloader.bin）上で動くベアメタルターゲットです。
 *	CPU は IA-32e ロングモードへ遷移後、CS=0x08 の 32bit 互換モードで
 *	カーネルを実行します（ビルドは -m32 / ILP32）。
 *	  - タイマ割込み : 8254 PIT (IRQ0, 100Hz)
 *	  - 割込み制御   : 実 cli/sti（EFLAGS.IF）
 *	  - シリアル     : COM1 16550
 */

#ifndef __SYS_SYSDEPEND_MACHINE_H__
#define __SYS_SYSDEPEND_MACHINE_H__

/*
 * [TYPE]_[CPU]		ターゲットシステム
 */

/* ----- x86 PC（QEMU ベアメタル）ターゲット定義 ----- */

#define X86_PC			1	/* ターゲットシステム : x86 PC ベアメタル */
#define CPU_X86			1	/* ターゲット CPU : x86 (IA-32 互換モード) */
#define CPU_CORE_X86		1	/* ターゲット CPU コア : x86 */

#define TARGET_DIR		x86_pc	/* sysdepend ディレクトリ名 */
#define KNL_SYSDEP_PATH		x86_pc	/* カーネル sysdepend パス */

/*
 **** CPU 依存プロファイル（x86 / ILP32）
 */
#define ALLOW_MISALIGN		1	/* 非整列アクセスを許可 */
#define BIGENDIAN		0	/* リトルエンディアン */
#define INT_BITWIDTH		32	/* INT 型のビット幅 */

/*
 * p-kernel 拡張の固定幅整数型
 *	（linux_x86_64 ポートの machine.h と同一。アプリ層互換用）
 */
#ifndef _in_asm_source_
typedef signed char	S1;	/* 符号付き  8 ビット整数 */
typedef signed short	S2;	/* 符号付き 16 ビット整数 */
typedef signed int	S4;	/* 符号付き 32 ビット整数 */
typedef unsigned char	U1;	/* 符号なし  8 ビット整数 */
typedef unsigned short	U2;	/* 符号なし 16 ビット整数 */
typedef unsigned int	U4;	/* 符号なし 32 ビット整数 */
typedef void		*VP;	/* 不定形データへのポインタ（T-Kernel 2.0 互換） */

/*
 * I/O ポートアクセス
 *	micro T-Kernel 2.0 では arch/x86/include/cpu_insn.h が提供して
 *	いた inline 群のうち outb/inb を提供する（arch/ 層のドライバが
 *	広く使用。outw/inl 等は各ドライバが自前定義しているため置かない）。
 */
static __inline__ void outb(unsigned short port, unsigned char val)
{
	__asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static __inline__ unsigned char inb(unsigned short port)
{
	unsigned char ret;
	__asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
	return ret;
}
#endif /* _in_asm_source_ */

#endif /* __SYS_SYSDEPEND_MACHINE_H__ */
