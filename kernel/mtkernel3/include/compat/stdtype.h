/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel 互換シム
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	stdtype.h（互換シム）
 *
 *	ベアメタルターゲットのプレースホルダ libc（include/lib/libc）が
 *	micro T-Kernel 2.0 の <stdtype.h> を include するため、同内容を
 *	ここで提供します（C 言語の基本型マクロのみ）。
 */

#ifndef __STDTYPE_H__
#define __STDTYPE_H__

#define __size_t	unsigned long

#ifndef	__cplusplus
#define __wchar_t	int
#endif

#endif /* __STDTYPE_H__ */
