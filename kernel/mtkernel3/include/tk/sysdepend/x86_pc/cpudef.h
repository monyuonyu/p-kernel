/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpudef.h
 *	CPU 依存定義（x86 ベアメタルポート依存部）
 *
 *	レジスタ操作 API（tk_get_reg / tk_set_reg）は本ポートでは
 *	サポートしません（TK_SUPPORT_REGOPS = FALSE）。
 */

#ifndef __TK_CPUDEF_DEPEND_H__
#define __TK_CPUDEF_DEPEND_H__

/* コプロセッサ属性（FPU 退避は side-table 方式でディスパッチャが行う） */
#define TA_COPS		0
#define TA_FPU		0

/*
 * 汎用レジスタ（cdecl の callee-saved 相当のみ）
 */
typedef struct t_regs {
	VW	r[4];		/* ebx, esi, edi, ebp */
} T_REGS;

/*
 * 例外関連レジスタ
 */
typedef struct t_eit {
	void	*pc;		/* プログラムカウンタ (eip) */
	UW	eflags;		/* フラグレジスタ */
} T_EIT;

/*
 * 制御レジスタ
 */
typedef struct t_cregs {
	void	*ssp;		/* システムスタックポインタ (esp) */
} T_CREGS;

#endif /* __TK_CPUDEF_DEPEND_H__ */
