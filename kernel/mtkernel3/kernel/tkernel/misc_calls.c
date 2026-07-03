/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.01
 *
 *    Copyright (C) 2006-2020 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2020/05/29.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	misc_calls.c
 * @brief	その他のシステムコール
 *
 * システム状態参照（tk_ref_sys / td_ref_sys）と
 * バージョン情報参照（tk_ref_ver）を実装します。
 */

#include "kernel.h"
#include "check.h"


#ifdef USE_FUNC_TK_REF_SYS
/**
 * @brief システム状態の参照
 *
 * 現在のシステム状態（タスク部／準タスク部／タスク独立部、
 * 割込み禁止中、ディスパッチ禁止中）と、実行中タスクおよび
 * 次に実行すべきタスクの ID を pk_rsys に返します。
 *
 * @param pk_rsys システム状態を返す領域
 * @return 常に E_OK
 * @note タスク独立部から呼ばれた場合は TSS_INDP のみを設定します。
 */
SYSCALL ER tk_ref_sys( T_RSYS *pk_rsys )
{
	BOOL	b_qtsk;

	if ( in_indp() ) {
		pk_rsys->sysstat = TSS_INDP;
	} else {
		BEGIN_DISABLE_INTERRUPT;
		b_qtsk = in_qtsk();
		END_DISABLE_INTERRUPT;

		if ( b_qtsk ) {
			pk_rsys->sysstat = TSS_QTSK;
		} else {
			pk_rsys->sysstat = TSS_TSK;
		}
		if ( in_loc() ) {
			pk_rsys->sysstat |= TSS_DINT;
		}
		if ( in_ddsp() ) {
			pk_rsys->sysstat |= TSS_DDSP;
		}
	}
	pk_rsys->runtskid = ( knl_ctxtsk != NULL )? knl_ctxtsk->tskid: 0;
	pk_rsys->schedtskid = ( knl_schedtsk != NULL )? knl_schedtsk->tskid: 0;

	return E_OK;
}
#endif /* USE_FUNC_TK_REF_SYS */

#ifdef USE_FUNC_TK_REF_VER
/**
 * @brief バージョン情報の参照
 *
 * カーネルのバージョン情報を pk_rver に返します。
 * バージョン情報が無い項目には 0 を設定します
 * （エラーにはしません）。
 *
 * @param pk_rver バージョン情報を返す領域
 * @return 常に E_OK
 */
SYSCALL ER tk_ref_ver( T_RVER *pk_rver )
{
	pk_rver->maker = (UH)VER_MAKER;	/* OS メーカ */
	pk_rver->prid  = (UH)VER_PRID;	/* OS 識別番号 */
	pk_rver->spver = (UH)VER_SPVER;	/* 仕様書バージョン */
	pk_rver->prver = (UH)VER_PRVER;	/* OS 製品バージョン */
	pk_rver->prno[0] = (UH)VER_PRNO1;	/* 製品管理情報 */
	pk_rver->prno[1] = (UH)VER_PRNO2;	/* 製品管理情報 */
	pk_rver->prno[2] = (UH)VER_PRNO3;	/* 製品管理情報 */
	pk_rver->prno[3] = (UH)VER_PRNO4;	/* 製品管理情報 */

	return E_OK;
}
#endif /* USE_FUNC_TK_REF_VER */

/* ------------------------------------------------------------------------ */
/*
 *	デバッガサポート機能
 */
#if USE_DBGSPT

#ifdef USE_FUNC_TD_REF_SYS
/**
 * @brief システム状態の参照（デバッガサポート）
 *
 * tk_ref_sys と同様に、現在のシステム状態と実行中タスク・
 * 次に実行すべきタスクの ID を pk_rsys に返します。
 *
 * @param pk_rsys システム状態を返す領域
 * @return 常に E_OK
 */
SYSCALL ER td_ref_sys( TD_RSYS *pk_rsys )
{
	BOOL	b_qtsk;

	if ( in_indp() ) {
		pk_rsys->sysstat = TSS_INDP;
	} else {
		BEGIN_DISABLE_INTERRUPT;
		b_qtsk = in_qtsk();
		END_DISABLE_INTERRUPT;

		if ( b_qtsk ) {
			pk_rsys->sysstat = TSS_QTSK;
		} else {
			pk_rsys->sysstat = TSS_TSK;
		}
		if ( in_loc() ) {
			pk_rsys->sysstat |= TSS_DINT;
		}
		if ( in_ddsp() ) {
			pk_rsys->sysstat |= TSS_DDSP;
		}
	}
	pk_rsys->runtskid = ( knl_ctxtsk != NULL )? knl_ctxtsk->tskid: 0;
	pk_rsys->schedtskid = ( knl_schedtsk != NULL )? knl_schedtsk->tskid: 0;

	return E_OK;
}
#endif /* USE_FUNC_TD_REF_SYS */

#endif /* USE_DBGSPT */
