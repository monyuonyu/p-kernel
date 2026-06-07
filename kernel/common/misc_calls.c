/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 2.0 Software Package
 *
 *    Copyright (C) 2006-2014 by Ken Sakamura.
 *    This software is distributed under the T-License 2.0.
 *----------------------------------------------------------------------
 *
 *    Released by T-Engine Forum(http://www.t-engine.org/) at 2014/09/01.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file misc_calls.c
 * @brief その他システムコール機能
 * 
 * T-Kernelの雑多なシステムコール機能の実装を提供する。
 * システム状態の参照、バージョン情報の取得、デバッガサポート機能等、
 * 分類しにくい様々なシステム機能を実装している。
 * 
 * 主な機能：
 * - システム状態参照（tk_ref_sys）
 * - バージョン情報参照（tk_ref_ver）
 * - 省電力モード制御（knl_lowpow_discnt）
 * - デバッガサポート機能（フック関数設定等）
 * - システムコール・ディスパッチャ・割り込みハンドラのフック機能
 * 
 * これらの機能は主にシステム監視、デバッグ、電力管理等の目的で使用される。
 */

/** [BEGIN Common Definitions] */
#include "kernel.h"
#include "task.h"
#include "check.h"
#include "misc_calls.h"
/** [END Common Definitions] */


#ifdef USE_FUNC_TK_REF_SYS
/**
 * @brief システム状態参照
 * 
 * カーネルの現在のシステム状態を取得する。
 * 実行中のタスク、スケジュール対象タスク、システムの動作状態等の情報を提供する。
 * 
 * @param pk_rsys システム状態情報を格納するパケットへのポインタ
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * 
 * @note 返される状態には以下が含まれる：
 *       - TSS_TSK: タスク部実行中
 *       - TSS_QTSK: 準タスク部実行中
 *       - TSS_INDP: タスク独立部実行中
 *       - TSS_DINT: 割り込み禁止状態
 *       - TSS_DDSP: ディスパッチ禁止状態
 */
SYSCALL ER tk_ref_sys_impl( T_RSYS *pk_rsys )
{
	if ( in_indp() ) {
		pk_rsys->sysstat = TSS_INDP;
	} else {
		if ( in_qtsk() ) {
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
 * @brief バージョン情報参照
 * 
 * T-Kernelのバージョン情報を取得する。
 * OSメーカー、識別番号、仕様バージョン、製品バージョン等の情報を提供する。
 * 
 * @param pk_rver バージョン情報を格納するパケットへのポインタ
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * 
 * @note カーネルバージョン情報が無い場合は各情報に0が設定される（エラーにはならない）
 * @note 返される情報：
 *       - maker: OSメーカー
 *       - prid: OS識別番号
 *       - spver: 仕様バージョン
 *       - prver: OS製品バージョン
 *       - prno[0-3]: 製品番号
 */
SYSCALL ER tk_ref_ver_impl( T_RVER *pk_rver )
{
	pk_rver->maker = (UH)CFN_VER_MAKER;	/* OS manufacturer */
	pk_rver->prid  = (UH)CFN_VER_PRID;	/* OS identification number */
	pk_rver->spver = (UH)CFN_VER_SPVER;	/* Specification version */
	pk_rver->prver = (UH)CFN_VER_PRVER;	/* OS product version */
	pk_rver->prno[0] = (UH)CFN_VER_PRNO1;	/* Product number */
	pk_rver->prno[1] = (UH)CFN_VER_PRNO2;	/* Product number */
	pk_rver->prno[2] = (UH)CFN_VER_PRNO3;	/* Product number */
	pk_rver->prno[3] = (UH)CFN_VER_PRNO4;	/* Product number */

	return E_OK;
}
#endif /* USE_FUNC_TK_REF_VER */

#ifdef USE_FUNC_LOWPOW_DISCNT
/**
 * @brief 省電力モード切り替え禁止カウンタ
 * 
 * 省電力モードへの切り替えを禁止する回数のカウンタ。
 * 0の場合は省電力モード切り替えが有効、1以上の場合は禁止される。
 * 
 * @note このカウンタが0以外の値の間は省電力モードへの移行が抑制される
 * @note 通常は電力管理モジュールや割り込み処理等で使用される
 */
EXPORT UINT	knl_lowpow_discnt = 0;
#endif /* USE_FUNC_LOWPOW_DISCNT */
/* ------------------------------------------------------------------------ */
/*
 *	Debugger support function
 */
#if USE_DBGSPT

/*
 * Hook routine address
 */
#ifdef USE_FUNC_HOOK_ENTERFN
Noinit(EXPORT FP knl_hook_enterfn);
Noinit(EXPORT FP knl_hook_leavefn);
#if TA_GP
Noinit(EXPORT void *knl_hook_svc_gp);
#endif
#endif /* USE_FUNC_HOOK_ENTERFN */

#ifdef USE_FUNC_HOOK_EXECFN
Noinit(EXPORT FP knl_hook_execfn);
Noinit(EXPORT FP knl_hook_stopfn);
#if TA_GP
Noinit(EXPORT void *knl_hook_dsp_gp);
#endif
#endif /* USE_FUNC_HOOK_EXECFN */

#ifdef USE_FUNC_HOOK_IENTERFN
Noinit(EXPORT FP knl_hook_ienterfn);
Noinit(EXPORT FP knl_hook_ileavefn);
#if TA_GP
Noinit(EXPORT void *knl_hook_int_gp);
#endif
#endif /* USE_FUNC_HOOK_IENTERFN */


#ifdef USE_FUNC_TD_HOK_SVC
/**
 * @brief システムコール・拡張SVCフック関数設定・解除
 * 
 * システムコールおよび拡張SVCの呼び出し時に実行されるフック関数を
 * 設定または解除する。デバッガやプロファイラ等の用途で使用される。
 * 
 * @param hsvc フック関数情報パケットへのポインタ（NULLの場合は解除）
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * 
 * @note hsvcがNULLの場合はシステムコールフック関数を解除する
 * @note 設定される関数：
 *       - enter: システムコール開始時に呼び出される関数
 *       - leave: システムコール終了時に呼び出される関数
 */
SYSCALL ER td_hok_svc_impl P1( TD_HSVC *hsvc )
{
	BEGIN_DISABLE_INTERRUPT;
	if ( hsvc == NULL ) { /* Cancel system call hook routine */
		/* Cancel */
		knl_unhook_svc();
	} else {
		/* Set */
		knl_hook_enterfn = hsvc->enter;
		knl_hook_leavefn = hsvc->leave;
#if TA_GP
		knl_hook_svc_gp = gp;
#endif
		knl_hook_svc();
	}
	END_DISABLE_INTERRUPT;

	return E_OK;
}
#endif /* USE_FUNC_TD_HOK_SVC */

#ifdef USE_FUNC_TD_HOK_DSP
/**
 * @brief ディスパッチャフック関数設定・解除
 * 
 * タスクディスパッチ時に実行されるフック関数を設定または解除する。
 * タスク切り替えの監視やプロファイリング等の用途で使用される。
 * 
 * @param hdsp フック関数情報パケットへのポインタ（NULLの場合は解除）
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * 
 * @note hdspがNULLの場合はディスパッチャフック関数を解除する
 * @note 設定される関数：
 *       - exec: タスク実行開始時に呼び出される関数
 *       - stop: タスク実行停止時に呼び出される関数
 */
SYSCALL ER td_hok_dsp_impl P1( TD_HDSP *hdsp )
{
	BEGIN_DISABLE_INTERRUPT;
	if ( hdsp == NULL ) { /* Cancel dispatcher hook routine */
		/* Cancel */
		knl_unhook_dsp();
	} else {
		/* Set */
		knl_hook_execfn = hdsp->exec;
		knl_hook_stopfn = hdsp->stop;
#if TA_GP
		knl_hook_dsp_gp = gp;
#endif
		knl_hook_dsp();
	}
	END_DISABLE_INTERRUPT;

	return E_OK;
}
#endif /* USE_FUNC_TD_HOK_DSP */

#ifdef USE_FUNC_TD_HOK_INT
/**
 * @brief EITハンドラフック関数設定・解除
 * 
 * 例外・割り込み・トラップハンドラ実行時に呼び出されるフック関数を
 * 設定または解除する。割り込み処理の監視やデバッグ等の用途で使用される。
 * 
 * @param hint フック関数情報パケットへのポインタ（NULLの場合は解除）
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_NOSPT サポートしていない（HLL割り込みハンドラが無効な場合）
 * 
 * @note hintがNULLの場合は割り込みハンドラフック関数を解除する
 * @note HLL割り込みハンドラが有効な場合のみサポートされる
 * @note 設定される関数：
 *       - enter: 割り込みハンドラ開始時に呼び出される関数
 *       - leave: 割り込みハンドラ終了時に呼び出される関数
 */
SYSCALL ER td_hok_int_impl P1( TD_HINT *hint )
{
#if USE_HLL_INTHDR
	BEGIN_DISABLE_INTERRUPT;
	if ( hint == NULL ) { /* Cancel interrupt handler hook routine */
		/* Cancel */
		knl_unhook_int();
	} else {
		/* Set */
		knl_hook_ienterfn = hint->enter;
		knl_hook_ileavefn = hint->leave;
#if TA_GP
		knl_hook_int_gp = gp;
#endif
		knl_hook_int();
	}
	END_DISABLE_INTERRUPT;

	return E_OK;
#else
	return E_NOSPT;
#endif /* USE_HLL_INTHDR */
}
#endif /* USE_FUNC_TD_HOK_INT */

#ifdef USE_FUNC_TD_REF_SYS
/**
 * @brief システム状態参照（デバッガサポート版）
 * 
 * デバッガサポート機能としてシステム状態を参照する。
 * 基本的にはtk_ref_sys()と同じ機能だが、デバッガ専用の拡張情報を含む場合がある。
 * 
 * @param pk_rsys システム状態情報を格納するパケットへのポインタ
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * 
 * @note 通常のtk_ref_sys()と同様のシステム状態情報を返す
 * @note デバッガやシステム監視ツール等で使用される
 */
SYSCALL ER td_ref_sys_impl( TD_RSYS *pk_rsys )
{
	if ( in_indp() ) {
		pk_rsys->sysstat = TSS_INDP;
	} else {
		if ( in_qtsk() ) {
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
