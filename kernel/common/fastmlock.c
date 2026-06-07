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
 * @file fastmlock.c
 * @brief 高速排他制御マルチロック機能
 * 
 * このファイルは、T-Kernelライブラリの高速排他制御マルチロック機能を
 * 実装します。FastMLockは、一つのロックオブジェクトで最大32個の
 * 独立したロックを管理できる効率的な排他制御メカニズムです。
 * 
 * 主な特徴：
 * - 単一オブジェクトで最大32個のロック管理
 * - イベントフラグベースの実装
 * - ビット操作による高速なロック制御
 * - 優先度順待ちキューサポート（TA_TPRI）
 * - 複数タスク待ちサポート（TA_WMUL）
 * 
 * アーキテクチャ：
 * - lock->flg: 各ビットがロック状態を表現（32個のロック）
 * - lock->wai: 待機中タスク数のカウンタ
 * - lock->id: 内部で使用するイベントフラグID
 */

/** [BEGIN Common Definitions] */
#include <basic.h>
#include <tkernel.h>
#include <util.h>
#include <libstr.h>
#include "libtk_config.h"

/* ------------------------------------------------------------------------ */
/**
 * @brief アトミックな演算操作関数群
 * 
 * 以下の関数は排他制御が必要なアトミック操作を提供します：
 * - INC: インクリメント操作
 * - DEC: デクリメント操作  
 * - BTS: ビットテストアンドセット操作
 * - BR: ビットリセット操作
 * 
 * これらの操作は割り込み禁止により排他制御を実現しています。
 */

/**
 * @brief 値をアトミックにインクリメントする
 * @param val インクリメントする値へのポインタ
 * 
 * 割り込みを禁止して値を1増加させます。
 */
void INC( INT *val )
{
	UINT	imask;

	DI(imask);
	(*val)++;
	EI(imask);
}

/**
 * @brief 値をアトミックにデクリメントする
 * @param val デクリメントする値へのポインタ
 * 
 * 割り込みを禁止して値を1減少させます。
 */
void DEC( INT *val )
{
	UINT	imask;

	DI(imask);
	(*val)--;
	EI(imask);
}

/**
 * @brief ビットテストアンドセット操作
 * @param val 操作対象の値へのポインタ
 * @param no 操作するビット番号（0-31）
 * @return セット前のビット状態（TRUE: 既にセット済み、FALSE: 未セット）
 * 
 * 指定されたビットをテストし、その後1にセットします。
 * この操作はアトミックに実行されます。
 */
BOOL BTS( UINT *val, INT no )
{
	UINT	imask;
	UINT	b;
	UINT	bm = (UINT)(1 << no);

	DI(imask);
	b = *val & bm;
	*val |= bm;
	EI(imask);
	return (BOOL)b;
}

/**
 * @brief ビットリセット操作
 * @param val 操作対象の値へのポインタ
 * @param no リセットするビット番号（0-31）
 * 
 * 指定されたビットを0にリセットします。
 * この操作はアトミックに実行されます。
 */
void BR( UINT *val, INT no )
{
	UINT	imask;

	DI(imask);
	*val &= ~(UINT)(1 << no);
	EI(imask);
}

/* ------------------------------------------------------------------------ */
/** [END Common Definitions] */

#ifdef USE_FUNC_MLOCKTMO
/**
 * @brief タイムアウト指定付きロック取得
 * @param lock マルチロックオブジェクトへのポインタ
 * @param no ロック番号（0-31）
 * @param tmo タイムアウト時間（TMO_FEVRで無限待ち）
 * @return エラーコード（E_OK: 成功、E_TMOUT: タイムアウト）
 * 
 * 指定されたロック番号のロックを取得します。
 * ロックが取得できない場合は、指定された時間まで待機します。
 */
EXPORT ER MLockTmo( FastMLock *lock, INT no, TMO tmo )
{
	UINT	ptn = (UINT)(1 << no);
	UINT	flg;
	ER	ercd;

	INC(&lock->wai);
	for ( ;; ) {
		if ( !BTS(&lock->flg, no) ) {
			ercd = E_OK;
			break;
		}

		ercd = tk_wai_flg(lock->id, ptn, TWF_ORW|TWF_BITCLR, &flg, tmo);
		if ( ercd < E_OK ) {
			break;
		}
	}
	DEC(&lock->wai);

	return ercd;
}
#endif /* USE_FUNC_MLOCKTMO */

#ifdef USE_FUNC_MLOCK
/**
 * @brief ロック取得（無限待ち）
 * @param lock マルチロックオブジェクトへのポインタ
 * @param no ロック番号（0-31）
 * @return エラーコード（E_OK: 成功）
 * 
 * 指定されたロック番号のロックを取得します。
 * ロックが取得できない場合は無限に待機します。
 */
EXPORT ER MLock( FastMLock *lock, INT no )
{
	return MLockTmo(lock, no, TMO_FEVR);
}
#endif /* USE_FUNC_MLOCK */

#ifdef USE_FUNC_MUNLOCK
/**
 * @brief ロック解放
 * @param lock マルチロックオブジェクトへのポインタ
 * @param no ロック番号（0-31）
 * @return エラーコード（E_OK: 成功）
 * 
 * 指定されたロック番号のロックを解放します。
 * 待機中のタスクがある場合は、イベントフラグをセットして通知します。
 */
EXPORT ER MUnlock( FastMLock *lock, INT no )
{
	UINT	ptn = (UINT)(1 << no);
	ER	ercd;

	BR(&lock->flg, no);
	ercd = ( lock->wai == 0 )? E_OK: tk_set_flg(lock->id, ptn);

	return ercd;
}
#endif /* USE_FUNC_MUNLOCK */

#ifdef USE_FUNC_CREATEMLOCK
/**
 * @brief マルチロックオブジェクトの生成
 * @param lock 初期化するマルチロックオブジェクトへのポインタ
 * @param name オブジェクト名（NULL可）
 * @return エラーコード（E_OK: 成功）
 * 
 * マルチロックオブジェクトを生成し、初期化します。
 * 内部でイベントフラグを生成し、マルチロック機能を提供します。
 */
EXPORT ER CreateMLock( FastMLock *lock, CONST UB *name )
{
	T_CFLG	cflg;
	ER	ercd;

	if ( name == NULL ) {
		cflg.exinf = NULL;
	} else {
		strncpy((char*)&cflg.exinf, (char*)name, sizeof(cflg.exinf));
	}
	cflg.flgatr  = TA_TPRI | TA_WMUL;
	cflg.iflgptn = 0;

	lock->id = ercd = tk_cre_flg(&cflg);
	if ( ercd < E_OK ) {
		return ercd;
	}

	lock->wai = 0;
	lock->flg = 0;

	return E_OK;
}
#endif /* USE_FUNC_CREATEMLOCK */

#ifdef USE_FUNC_DELETEMLOCK
/**
 * @brief マルチロックオブジェクトの削除
 * @param lock 削除するマルチロックオブジェクトへのポインタ
 * @return エラーコード（E_OK: 成功、E_PAR: パラメータエラー）
 * 
 * マルチロックオブジェクトを削除し、関連する
 * イベントフラグも削除します。
 */
EXPORT ER DeleteMLock( FastMLock *lock )
{
	ER	ercd;

	if ( lock->id <= 0 ) {
		return E_PAR;
	}

	ercd = tk_del_flg(lock->id);
	if ( ercd < E_OK ) {
		return ercd;
	}

	lock->id = 0;

	return E_OK;
}
#endif /* USE_FUNC_DELETEMLOCK */
