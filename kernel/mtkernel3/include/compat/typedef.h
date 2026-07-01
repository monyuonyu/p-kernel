/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux x86-64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	typedef.h（互換シム）
 *
 *	p-kernel のアプリ層には micro T-Kernel 2.0 の <typedef.h>
 *	（リポジトリ直下 include/typedef.h）を直接 include するヘッダが
 *	あります。μT-Kernel 3.0 ビルドでは 2.0 版と 3.0 版の型定義が
 *	混在すると D（64bit 整数）等の基底型の綴りが衝突するため、
 *	この シムが <typedef.h> を 3.0 の型定義に付け替えます。
 *	（U1/U2/U4・S1/S2/S4・VP などの p-kernel 拡張型は
 *	  sys/sysdepend/linux_x86_64/machine.h 側で定義済み）
 */

#ifndef __MTK3_COMPAT_TYPEDEF_H__
#define __MTK3_COMPAT_TYPEDEF_H__

#include <config.h>
#include <sys/machine.h>

#if USE_STDINC_STDINT
#include <stdint.h>
#endif

#include <tk/typedef.h>

#endif /* __MTK3_COMPAT_TYPEDEF_H__ */
