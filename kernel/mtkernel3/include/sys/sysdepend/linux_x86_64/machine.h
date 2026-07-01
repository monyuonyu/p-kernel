/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux x86-64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	machine.h
 *	マシン種別定義（Linux x86-64 ユーザモードポート依存部）
 *
 *	Linux 上のユーザプロセスとして μT-Kernel 3.0 を動かすターゲットです。
 *	ハードウェア資源はすべて POSIX の機構に写像されます:
 *	  - タイマ割込み  → SIGALRM（POSIX タイマ）
 *	  - 割込み禁止    → arch_irq_disabled_flag（ソフトウェアフラグ）
 *	  - シリアル入出力 → termios raw モードの標準入出力
 */

#ifndef __SYS_SYSDEPEND_MACHINE_H__
#define __SYS_SYSDEPEND_MACHINE_H__

/*
 * [TYPE]_[CPU]		ターゲットシステム
 * CPU_xxxx		CPU 種別
 * CPU_CORE_xxx		CPU コア種別
 */

/* ----- Linux x86-64 ユーザモード ターゲット定義 ----- */

#define LINUX_X86_64		1	/* ターゲットシステム : Linux x86-64 ユーザモード */
#define CPU_X86_64		1	/* ターゲット CPU : x86-64 (AMD64) */
#define CPU_CORE_X86_64		1	/* ターゲット CPU コア : x86-64 */

#define TARGET_DIR		linux_x86_64	/* sysdepend ディレクトリ名 */
#define KNL_SYSDEP_PATH		linux_x86_64	/* カーネル sysdepend パス */

/*
 **** CPU 依存プロファイル（x86-64）
 */
#define ALLOW_MISALIGN		1	/* 非整列アクセスを許可（x86-64 は許容） */
#define BIGENDIAN		0	/* リトルエンディアン */
#define INT_BITWIDTH		32	/* INT 型のビット幅 */

/*
 * p-kernel 拡張の固定幅整数型
 *	p-kernel のアプリ層（arch/common 等）は micro T-Kernel 2.0 の
 *	include/typedef.h が定義していた S1/S2/S4・U1/U2/U4 を広く使用
 *	している。2.0 と同一の綴りで定義し、両ヘッダが同居しても
 *	重複 typedef（同一型なら C11 で許容）で衝突しないようにする。
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
