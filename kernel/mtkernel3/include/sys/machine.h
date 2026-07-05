/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.07.B0
 *
 *    Copyright (C) 2006-2023 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2023/11.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	machine.h
 * @brief	マシン種別定義
 *
 * ターゲットマシンごとの machine.h を取り込み、シンボル修飾マクロ
 * Csym() と、Cコンパイラ依存のマクロ（Inline / Asm 等）を定義します。
 */

#ifndef __SYS_MACHINE_H__
#define __SYS_MACHINE_H__

/* ===== システム依存部の定義 ============================================ */

#ifdef _IOTE_M367_
#include "sysdepend/iote_m367/machine.h"

#define Csym(sym) sym

#endif

#ifdef _IOTE_STM32L4_
#include "sysdepend/iote_stm32l4/machine.h"

#define Csym(sym) sym

#endif

#ifdef _IOTE_RX231_
#include "sysdepend/iote_rx231/machine.h"

#define Csym(sym) _##sym

#endif

#ifdef _IOTE_RZA2M_
#include "sysdepend/iote_rza2m/machine.h"

#define Csym(sym) sym

#endif

/* p-kernel 追加ターゲット: Linux x86-64 ユーザモード */
#ifdef _LINUX_X86_64_
#include "sysdepend/linux_x86_64/machine.h"

#define Csym(sym) sym

#endif

/* p-kernel 追加ターゲット: Linux AArch64 ユーザモード */
#ifdef _LINUX_AARCH64_
#include "sysdepend/linux_aarch64/machine.h"

#define Csym(sym) sym

#endif

/* p-kernel 追加ターゲット: Windows x86-64 ネイティブ */
#ifdef _WINDOWS_X86_64_
#include "sysdepend/windows_x86_64/machine.h"

#define Csym(sym) sym

#endif

/* p-kernel 追加ターゲット: x86 PC ベアメタル（QEMU） */
#ifdef _X86_PC_
#include "sysdepend/x86_pc/machine.h"

#define Csym(sym) sym

#endif

/* p-kernel 追加ターゲット: AArch64 ベアメタル（QEMU virt / RPi3） */
#ifdef _AARCH64_VIRT_
#include "sysdepend/aarch64_virt/machine.h"

#define Csym(sym) sym

#endif

/* ===== Cコンパイラ依存の定義 =========================================== */

#ifdef __GNUC__

#define Inline static __inline__
#define Asm __asm__ volatile
#define Noinit(decl) decl __attribute__((section(".noinit")))
#define	Section(decl,name) decl __attribute__((section(#name)))
#define WEAK_FUNC __attribute__((weak))

#define _VECTOR_ENTRY(name) .word name
#define _WEAK_ENTRY(name) .weak name

#endif /* __GNUC__ */

#endif /* __SYS_MACHINE_H__ */
