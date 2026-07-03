/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel 互換拡張
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/**
 * @file	subsystem.c
 * @brief	サブシステム管理（p-kernel 互換拡張）
 *
 * micro T-Kernel 2.0 のサブシステム（拡張 SVC）機構の最小移植です。
 * μT-Kernel 3.0 本体では廃止された機能ですが、p-kernel の x86
 * ベアメタルポートが ring3 ユーザ空間の syscall 橋渡し
 * （fs_ssy / net_ssy / blk_ssy）に使用するため復元しています。
 *
 * 提供する機能:
 * - サブシステムの定義・削除（tk_def_ssy）
 * - サブシステム定義の参照（tk_ref_ssy）
 * - 拡張 SVC ハンドラへの分岐（knl_svc_ientry）
 * - タスク終了時のリソース解放フック（knl_ssy_cleanup）
 *
 * USE_SUBSYSTEM を定義したターゲット（x86 ベアメタル）でのみ
 * 実体がコンパイルされます。
 */

#include "kernel.h"
#include "check.h"
#include "subsystem.h"

#if USE_SUBSYSTEM

Noinit(EXPORT SSYCB knl_ssycb_table[NUM_SSYID]);	/* サブシステム制御ブロック */

/**
 * @brief	未登録スロットのダミーハンドラ
 *
 * 未定義のサブシステムへの拡張 SVC 呼び出しに対して
 * E_RSFN（予約機能コード）を返します。
 */
EXPORT INT knl_no_support( void *pk_para, FN fncd )
{
	(void)pk_para; (void)fncd;
	return E_RSFN;
}

/**
 * @brief	サブシステム管理の初期化
 *
 * 全サブシステム ID を未登録状態（knl_no_support）に設定します。
 * ターゲットの初期化処理（sysdepend の devinit 等）から呼びます。
 *
 * @retval E_OK	正常終了
 */
EXPORT ER knl_subsystem_initialize( void )
{
	INT	i;

	for ( i = 0; i < NUM_SSYID; i++ ) {
		knl_ssycb_table[i].svchdr    = knl_no_support;
		knl_ssycb_table[i].cleanupfn = NULL;
	}

	return E_OK;
}

/**
 * @brief	サブシステムの定義（tk_def_ssy）
 *
 * pk_dssy が NULL 以外なら新規登録、NULL なら削除を行います。
 *
 * @param ssid		サブシステム ID（1〜CNF_MAX_SSYID）
 * @param pk_dssy	サブシステム定義情報（NULL = 削除）
 * @retval E_OK	正常終了
 * @retval E_ID	ssid が範囲外
 * @retval E_OBJ	既に登録済み（登録時）
 * @retval E_NOEXS	未登録（削除時）
 */
SYSCALL ER tk_def_ssy( ID ssid, CONST T_DSSY *pk_dssy )
{
	SSYCB	*ssycb;
	ER	ercd = E_OK;

#if CHK_ID
	if ( ssid < MIN_SSYID || ssid > MAX_SSYID ) {
		return E_ID;
	}
#endif
#if CHK_RSATR
	if ( pk_dssy != NULL ) {
		CHECK_RSATR(pk_dssy->ssyatr, TA_NULL);
	}
#endif

	ssycb = get_ssycb(ssid);

	BEGIN_CRITICAL_SECTION;
	if ( pk_dssy != NULL ) {
		/* 登録 */
		if ( ssycb->svchdr != knl_no_support ) {
			ercd = E_OBJ;		/* 登録済み */
		} else {
			ssycb->svchdr    = (SVC)pk_dssy->svchdr;
			ssycb->cleanupfn = (SSYCLEANUP)pk_dssy->cleanupfn;
		}
	} else {
		/* 削除 */
		if ( ssycb->svchdr == knl_no_support ) {
			ercd = E_NOEXS;		/* 未登録 */
		} else {
			ssycb->svchdr    = knl_no_support;
			ssycb->cleanupfn = NULL;
		}
	}
	END_CRITICAL_SECTION;

	return ercd;
}

/**
 * @brief	サブシステム定義の参照（tk_ref_ssy）
 *
 * @param ssid		サブシステム ID
 * @param pk_rssy	状態情報の格納先
 * @retval E_OK	正常終了（登録あり）
 * @retval E_ID	ssid が範囲外
 * @retval E_NOEXS	未登録
 */
SYSCALL ER tk_ref_ssy( ID ssid, T_RSSY *pk_rssy )
{
	SSYCB	*ssycb;
	ER	ercd = E_OK;

#if CHK_ID
	if ( ssid < MIN_SSYID || ssid > MAX_SSYID ) {
		return E_ID;
	}
#endif

	ssycb = get_ssycb(ssid);

	BEGIN_CRITICAL_SECTION;
	if ( ssycb->svchdr == knl_no_support ) {
		ercd = E_NOEXS;
	} else if ( pk_rssy != NULL ) {
		pk_rssy->ssypri = 0;	/* 優先度は管理していない */
	}
	END_CRITICAL_SECTION;

	return ercd;
}

/**
 * @brief	全サブシステムのクリーンアップ関数を呼ぶ
 *
 * タスク終了時に呼び出し、各サブシステムが保持するタスク単位の
 * リソース（ソケット・ファイルディスクリプタ等）を解放させます。
 * タスクレベル（割込みコンテキスト外）で呼ぶこと。
 *
 * @param tskid	終了するタスクの ID
 */
EXPORT void knl_ssy_cleanup( ID tskid )
{
	SSYCB	*ssycb, *end;

	end = knl_ssycb_table + NUM_SSYID;
	for ( ssycb = knl_ssycb_table; ssycb < end; ssycb++ ) {
		if ( ssycb->svchdr != knl_no_support && ssycb->cleanupfn != NULL ) {
			ssycb->cleanupfn(tskid);
		}
	}
}

/**
 * @brief	拡張 SVC ハンドラへの分岐ルーチン
 *
 * 機能コードの下位 8 ビットからサブシステム ID を取り出し、
 * 対応するハンドラを呼び出します。タスク部から呼ばれた場合は
 * 実行中タスクを準タスク部（sysmode++）として実行します。
 *
 * @param pk_para	パラメータパケット
 * @param fncd	機能コード（下位 8bit = SSID）
 * @return ハンドラの戻り値（未登録なら E_RSFN）
 */
EXPORT ER knl_svc_ientry( void *pk_para, FN fncd )
{
	ID	ssid;
	SSYCB	*ssycb;
	ER	ercd;

	/* 下位 8 ビットがサブシステム ID */
	ssid = fncd & 0xff;
	if ( ssid < MIN_SSYID || ssid > MAX_SSYID ) {
		return E_RSFN;
	}
	ssycb = get_ssycb(ssid);

	if ( in_indp() ) {
		/* タスク独立部から呼ばれた場合はそのまま実行 */
		ercd = (*ssycb->svchdr)(pk_para, fncd);
	} else {
		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode++;
		ENABLE_INTERRUPT;

		/* 拡張 SVC ハンドラの呼び出し */
		ercd = (*ssycb->svchdr)(pk_para, fncd);

		DISABLE_INTERRUPT;
		knl_ctxtsk->sysmode--;
		ENABLE_INTERRUPT;
	}

	return ercd;
}

#endif /* USE_SUBSYSTEM */
