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
 * @file	cpudef.h
 * @brief	CPU 依存定義
 *
 * ターゲットごとの CPU 依存定義（sysdepend 配下の cpudef.h）を
 * 取り込みます。
 */

#ifndef __TK_CPUDEF_H__
#define __TK_CPUDEF_H__

#define CPUDEF_PATH_(a)		#a
#define CPUDEF_PATH(a)		CPUDEF_PATH_(a)
#define CPUDEF_SYSDEP()		CPUDEF_PATH(sysdepend/TARGET_DIR/cpudef.h)
#include CPUDEF_SYSDEP()

#endif /* __TK_CPUDEF_H__ */