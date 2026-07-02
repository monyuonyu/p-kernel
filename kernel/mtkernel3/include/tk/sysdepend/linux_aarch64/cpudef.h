/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux AArch64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpudef.h
 *	CPU 依存定義（Linux AArch64 ユーザモードポート依存部）
 *
 *	レジスタ操作 API（tk_get_reg / tk_set_reg）は本ポートでは
 *	サポートしません（TK_SUPPORT_REGOPS = FALSE）。構造体定義は
 *	ヘッダの型整合のためにのみ存在します。
 */

#ifndef __TK_CPUDEF_DEPEND_H__
#define __TK_CPUDEF_DEPEND_H__

/* コプロセッサ属性（FPU 管理は行わない） */
#define TA_COPS		0
#define TA_FPU		0

/*
 * 汎用レジスタ（AAPCS64 の callee-saved 相当のみ）
 */
typedef struct t_regs {
	VD	r[12];		/* x19-x28, x29(fp), x30(lr) */
} T_REGS;

/*
 * 例外関連レジスタ
 */
typedef struct t_eit {
	void	*pc;		/* プログラムカウンタ */
	UD	pstate;		/* プロセッサ状態 */
} T_EIT;

/*
 * 制御レジスタ
 */
typedef struct t_cregs {
	void	*ssp;		/* システムスタックポインタ (sp) */
} T_CREGS;

#endif /* __TK_CPUDEF_DEPEND_H__ */
