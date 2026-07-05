/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Windows x86-64 ネイティブポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	machine.h
 *	マシン種別定義（Windows x86-64 ネイティブポート依存部）
 *
 *	Windows 上のネイティブプロセス（mingw-w64 が生成する PE 実行
 *	ファイル）として μT-Kernel 3.0 を動かすターゲットです。WSL では
 *	なく本物の Windows で動きます。ハードウェア資源は Win32 API に
 *	写像されます:
 *	  - タスク切替え  → Windows Fiber（ConvertThreadToFiber /
 *	                    CreateFiber / SwitchToFiber）
 *	  - タイマ tick   → QueryPerformanceCounter を安全点でポンプ
 *	                    （v1 は協調スケジューラ、プリエンプト無し）
 *	  - 割込み禁止    → arch_irq_disabled_flag（ソフトウェアフラグ）
 *	  - シリアル入出力 → Win32 コンソール（GetStdHandle）
 *	  - ネットワーク  → Winsock2（-lws2_32）
 */

#ifndef __SYS_SYSDEPEND_MACHINE_H__
#define __SYS_SYSDEPEND_MACHINE_H__

/*
 * [TYPE]_[CPU]		ターゲットシステム
 * CPU_xxxx		CPU 種別
 * CPU_CORE_xxx		CPU コア種別
 */

/* ----- Windows x86-64 ネイティブ ターゲット定義 ----- */

#define WINDOWS_X86_64		1	/* ターゲットシステム : Windows x86-64 ネイティブ */
#define CPU_X86_64		1	/* ターゲット CPU : x86-64 (AMD64) */
#define CPU_CORE_X86_64		1	/* ターゲット CPU コア : x86-64 */

#define TARGET_DIR		windows_x86_64	/* sysdepend ディレクトリ名 */
#define KNL_SYSDEP_PATH		windows_x86_64	/* カーネル sysdepend パス */

/*
 **** CPU 依存プロファイル（x86-64）
 */
#define ALLOW_MISALIGN		1	/* 非整列アクセスを許可（x86-64 は許容） */
#define BIGENDIAN		0	/* リトルエンディアン */
#define INT_BITWIDTH		32	/* INT 型のビット幅 */

/*
 * p-kernel 拡張の固定幅整数型
 *	Linux ポート（linux_x86_64/machine.h）と同一の綴りで定義し、
 *	micro T-Kernel 2.0 の typedef.h 互換（S1/S2/S4・U1/U2/U4）を
 *	提供する。
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
