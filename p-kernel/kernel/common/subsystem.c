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
 * @file subsystem.c
 * @brief サブシステム管理機能の実装
 * 
 * T-Kernelのサブシステム管理機能を実装する。
 * サブシステムは、カーネルの機能を拡張するためのモジュールで、
 * 拡張SVC（System Call）ハンドラを登録することで、
 * アプリケーションから独自の機能を呼び出すことができる。
 * 
 * 主な機能：
 * - サブシステムの定義・削除（tk_def_ssy）
 * - サブシステム情報の参照（tk_ref_ssy）
 * - 拡張SVCハンドラの呼び出し制御
 * - タスク独立部・タスク部での実行制御
 */

/** [BEGIN Common Definitions] */
#include "kernel.h"
#include "task.h"
#include "check.h"
#include "bitop.h"
#include "subsystem.h"
/** [END Common Definitions] */

#if CFN_MAX_SSYID > 0


#ifdef USE_FUNC_SSYCB_TABLE
Noinit(EXPORT SSYCB knl_ssycb_table[NUM_SSYID]);	/* Subsystem control block */
#endif /* USE_FUNC_SSYCB_TABLE */


#ifdef USE_FUNC_SUBSYSTEM_INITIALIZE
/**
 * @brief サブシステム制御ブロックの初期化
 * 
 * システム起動時にサブシステム制御ブロック（SSYCB）を初期化する。
 * 全てのサブシステムIDを未登録状態（knl_no_support）に設定する。
 * 
 * @return E_OK: 正常終了
 * @return E_SYS: システム設定エラー
 */
EXPORT ER knl_subsystem_initialize( void )
{
	INT	i;

	/* Get system information */
	if ( NUM_SSYID < 1 ) {
		return E_SYS;
	}
	if ( NUM_SSYPRI < 1 ) {
		return E_SYS;
	}

	for ( i = 0; i < NUM_SSYID; i++ ) {
		knl_ssycb_table[i].svchdr    = knl_no_support;
	}

	return E_OK;
}
#endif /* USE_FUNC_SUBSYSTEM_INITIALIZE */


#ifdef USE_FUNC_TK_DEF_SSY
/**
 * @brief サブシステムの定義
 * 
 * サブシステムを定義または削除する。
 * pk_dssy が NULL 以外の場合は新規登録、NULL の場合は削除を行う。
 * 
 * @param ssid サブシステムID
 * @param pk_dssy サブシステム定義パケット（NULLの場合は削除）
 * @return E_OK: 正常終了
 * @return E_OBJ: オブジェクト状態エラー（既に登録済み、または未登録）
 * @return E_RSATR: 予約属性またはサポートしていない属性の指定
 */
SYSCALL ER tk_def_ssy_impl P2( ID ssid, CONST T_DSSY *pk_dssy )
{
	SSYCB	*ssycb;
	ER	ercd = E_OK;

	CHECK_SSYID(ssid);
#if CHK_RSATR
	if ( pk_dssy != NULL ) {
		CHECK_RSATR(pk_dssy->ssyatr, TA_NULL|TA_GP);
	}
#endif

	ssycb = get_ssycb(ssid);

	BEGIN_CRITICAL_SECTION;
	if ( pk_dssy != NULL ) {
		/* Register */
		if ( ssycb->svchdr != knl_no_support ) {
			ercd = E_OBJ;  /* Registered */
			goto error_exit;
		}
		ssycb->svchdr    = (SVC)pk_dssy->svchdr;
#if TA_GP
		if ( (pk_dssy->ssyatr & TA_GP) != 0 ) {
			gp = pk_dssy->gp;
		}
		ssycb->gp = gp;
#endif

	} else {
		/* Delete */
		if ( ssycb->svchdr == knl_no_support ) {
			ercd = E_NOEXS;  /* Not registered */
			goto error_exit;
		}

		ssycb->svchdr    = knl_no_support;
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEF_SSY */

#ifdef USE_FUNC_TK_REF_SSY
/*
 * Refer subsystem definition information
 */
SYSCALL ER tk_ref_ssy_impl( ID ssid, T_RSSY *pk_rssy )
{
	SSYCB	*ssycb;
	ER	ercd = E_OK;

	CHECK_SSYID(ssid);

	ssycb = get_ssycb(ssid);

	BEGIN_CRITICAL_SECTION;
	if ( ssycb->svchdr == knl_no_support ) {
		ercd = E_NOEXS;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REF_SSY */

#if USE_DBGSPT

#ifdef USE_FUNC_TD_LST_SSY
/*
 * Refer subsystem usage state
 */
SYSCALL INT td_lst_ssy_impl( ID list[], INT nent )
{
	SSYCB	*ssycb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_ssycb_table + NUM_SSYID;
	for ( ssycb = knl_ssycb_table; ssycb < end; ssycb++ ) {
		if ( ssycb->svchdr == knl_no_support ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = ID_SSY(ssycb - knl_ssycb_table);
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_SSY */

#ifdef USE_FUNC_TD_REF_SSY
/*
 * Refer subsystem definition information
 */
SYSCALL ER td_ref_ssy_impl( ID ssid, TD_RSSY *pk_rssy )
{
	SSYCB	*ssycb;
	ER	ercd = E_OK;

	CHECK_SSYID(ssid);

	ssycb = get_ssycb(ssid);

	BEGIN_DISABLE_INTERRUPT;
	if ( ssycb->svchdr == knl_no_support ) {
		ercd = E_NOEXS;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_SSY */

#endif /* USE_DBGSPT */

#ifdef USE_FUNC_SVC_IENTRY
/**
 * @brief 拡張SVCハンドラへの分岐ルーチン
 * 
 * 拡張SVCが呼び出された際に、適切なサブシステムのハンドラを呼び出す。
 * 機能コードの下位8ビットからサブシステムIDを取得し、
 * タスク独立部かタスク部かに応じて適切に実行する。
 * 
 * @param pk_para パラメータパケット
 * @param fncd 機能コード
 * @return サブシステムハンドラの戻り値
 * @return E_RSFN: 未サポート機能
 */
EXPORT ER knl_svc_ientry P2GP( void *pk_para, FN fncd )
{
	ID	ssid;
	SSYCB	*ssycb;
	ER	ercd;

	/* Lower 8 bits are subsystem ID */
	ssid = fncd & 0xff;
	if ( ssid < MIN_SSYID || ssid > MAX_SSYID ) {
		return E_RSFN;
	}

	ssycb = get_ssycb(ssid);

	if ( in_indp() ) {
		/* Execute at task-independent part */
		ercd = CallUserHandlerP2_GP(pk_para, fncd,
						ssycb->svchdr, ssycb);
	} else {
		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode++;
		ENABLE_INTERRUPT;

		/* Call extended SVC handler */
		ercd = CallUserHandlerP2_GP(pk_para, fncd,
						ssycb->svchdr, ssycb);

		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode--;
		ENABLE_INTERRUPT;
	}

	return ercd;
}
#endif /* USE_FUNC_SVC_IENTRY */

#endif /* CFN_MAX_SSYID */
