/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.00
 *
 *    Copyright (C) 2006-2019 by Ken Sakamura.
 *    This software is distributed under the T-License 2.1.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2019/12/11.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	int.c
 * @brief	割込み管理機能
 *
 * 割込みハンドラの定義（tk_def_int）と
 * 割込みハンドラからの復帰（tk_ret_int）を提供します。
 */

#include "kernel.h"
#include "check.h"

/* ------------------------------------------------------------------------ */
/**
 * @brief 割込みハンドラの定義
 *
 * 割込み番号 intno に対する割込みハンドラを定義します。
 * pk_dint に NULL を指定すると、定義済みのハンドラを解除します。
 *
 * @param intno	割込み番号（N_INTVEC 未満）
 * @param pk_dint	割込みハンドラ定義情報（NULL で定義解除）
 * @retval E_OK	正常終了
 * @retval E_PAR	intno が不正
 * @retval E_RSATR	intatr に不正な属性が指定された
 * @retval E_NOSPT	未サポート（静的割込みベクタテーブル使用時: USE_STATIC_IVT）
 */
SYSCALL ER tk_def_int( UINT intno, CONST T_DINT *pk_dint )
{
#if USE_STATIC_IVT
	return E_NOSPT;
#else
	ATR	intatr;
	FP	inthdr;
	ER	ercd;

	CHECK_PAR(intno < N_INTVEC);
	if(pk_dint != NULL) {
		CHECK_RSATR(pk_dint->intatr, TA_HLNG|TA_ASM);
		intatr	= pk_dint->intatr;
		inthdr	= pk_dint->inthdr;
	} else {
		intatr	= 0;
		inthdr	= NULL;
	}

	BEGIN_CRITICAL_SECTION;
	ercd = knl_define_inthdr(intno, intatr, inthdr);
	END_CRITICAL_SECTION;

	return ercd;
#endif
}

/* ------------------------------------------------------------------------ */
/**
 * @brief 割込みハンドラからの復帰
 *
 * 割込みハンドラを終了し、必要に応じてディスパッチを行います。
 * TA_ASM 属性の割込みハンドラの末尾から呼び出します。
 */
SYSCALL void tk_ret_int( void )
{
	knl_return_inthdr();
	return;
}

