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
 * @file	power.c
 * @brief	省電力機能
 *
 * 省電力モードの設定 API（tk_set_pow）と、
 * 省電力モード切り替え禁止回数の管理変数を提供します。
 */

#include "kernel.h"
#include "check.h"

/*
 * 省電力モード切り替えの禁止回数
 *	0 のとき切り替えは許可状態
 */
EXPORT UINT	knl_lowpow_discnt = 0;

#if TK_SUPPORT_LOWPOWER
/**
 * @brief 省電力モードの設定
 *
 * pwmode に応じて、サスペンド状態への移行（TPW_DOSUSPEND）、
 * 省電力モード切り替えの禁止（TPW_DISLOWPOW）・許可（TPW_ENALOWPOW）を
 * 行います。禁止はネスト可能で、回数は knl_lowpow_discnt で管理します。
 *
 * @param pwmode	省電力モード（TPW_DOSUSPEND / TPW_DISLOWPOW / TPW_ENALOWPOW）
 * @retval E_OK	正常終了
 * @retval E_PAR	pwmode が不正
 * @retval E_QOVR	禁止回数が上限（LOWPOW_LIMIT）を超過
 * @retval E_OBJ	禁止されていない状態で TPW_ENALOWPOW を指定した
 * @retval E_CTX	タスク独立部からの呼び出し
 */
SYSCALL ER tk_set_pow( UINT pwmode )
{
	ER	ercd = E_OK;

	CHECK_INTSK();

	BEGIN_CRITICAL_SECTION;

	switch ( pwmode ) {
	  case TPW_DOSUSPEND:
		off_pow();
		break;
	  case TPW_DISLOWPOW:
		if ( knl_lowpow_discnt >= LOWPOW_LIMIT ) {
			ercd = E_QOVR;
		} else {
			knl_lowpow_discnt++;
		}
		break;
	  case TPW_ENALOWPOW:
		if ( knl_lowpow_discnt <= 0 ) {
			ercd = E_OBJ;
		} else {
			knl_lowpow_discnt--;
		}
		break;

	  default:
		ercd = E_PAR;
	}
	END_CRITICAL_SECTION;

	return ercd;
}

#endif	/* TK_SUPPORT_LOWPOWER */



