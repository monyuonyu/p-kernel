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
 * @file fastlock.c
 * @brief 高速排他制御ロック機能
 *
 * このファイルは、T-Kernelライブラリの高速排他制御ロック機能を
 * 実装します。FastLockは、通常のセマフォよりも高速な排他制御を
 * 提供するためのメカニズムです。
 *
 * 主な特徴：
 * - 軽量な排他制御メカニズム
 * - セマフォベースの実装
 * - 競合がない場合の高速動作
 * - FIFO/優先度順の待ちキューサポート
 *
 * アルゴリズム：
 * - 初期状態でcnt=-1（アンロック状態）
 * - Lock時：cnt をインクリメントし、>0 ならセマフォ待ち
 * - Unlock時：cnt をデクリメントし、>0 ならセマフォシグナル
 * - 競合がない場合はセマフォ操作なしで高速動作
 */

/** [BEGIN Common Definitions] */
#include <basic.h>
#include <tkernel.h>
#include <util.h>
#include <libstr.h>
#include "libtk_config.h"

/* ------------------------------------------------------------------------ */
/**
 * @brief FastLockカウンタのアトミックインクリメント
 *
 * FastLockのカウンタをアトミックにインクリメントします。
 * インクリメント後の値が正なら競合あり、セマフォ待ちが必要です。
 *
 * @param lock FastLock構造体へのポインタ
 *
 * @return インクリメント後のカウンタ値
 *   @retval >0 競合あり（セマフォ待ち必要）
 *   @retval <=0 競合なし（ロック取得成功）
 *
 * @note この関数は割り込み禁止でアトミック性を保証します。
 */
INT Inc( FastLock *lock )
{
	UINT	imask;
	INT	c;
	DI(imask);
	c = ++lock->cnt;
	EI(imask);
	return c;
}
/**
 * @brief FastLockカウンタのアトミックデクリメント
 *
 * FastLockのカウンタをアトミックにデクリメントします。
 * デクリメント前の値が正なら待ちタスクあり、セマフォシグナルが必要です。
 *
 * @param lock FastLock構造体へのポインタ
 *
 * @return デクリメント前のカウンタ値
 *   @retval >0 待ちタスクあり（セマフォシグナル必要）
 *   @retval <=0 待ちタスクなし（シグナル不要）
 *
 * @note この関数は割り込み禁止でアトミック性を保証します。
 */
INT Dec( FastLock *lock )
{
	UINT	imask;
	INT	c;
	DI(imask);
	c = lock->cnt--;
	EI(imask);
	return c;
}

/* ------------------------------------------------------------------------ */
/** [END Common Definitions] */

#ifdef USE_FUNC_LOCK
/**
 * @brief 高速ロック取得
 *
 * FastLockを使って排他ロックを取得します。
 * 競合がない場合は高速に動作し、競合がある場合はセマフォで待ちます。
 *
 * @param lock FastLock構造体へのポインタ
 *
 * @note この関数はブロッキング関数で、ロックが取得できるまで待ちます。
 *       ネストしたロックはサポートしません。
 */
EXPORT void Lock( FastLock *lock )
{
	if ( Inc(lock) > 0 ) {
		tk_wai_sem(lock->id, 1, TMO_FEVR);
	}
}
#endif /* USE_FUNC_LOCK */

#ifdef USE_FUNC_UNLOCK
/**
 * @brief 高速ロック解放
 *
 * FastLockで取得した排他ロックを解放します。
 * 待ちタスクがある場合はセマフォで通知します。
 *
 * @param lock FastLock構造体へのポインタ
 *
 * @note Lock()で取得したロックは必ず同じタスクでUnlock()してください。
 *       ロックを取得していない状態で呼び出した場合の動作は未定義です。
 */
EXPORT void Unlock( FastLock *lock )
{
	if ( Dec(lock) > 0 ) {
		tk_sig_sem(lock->id, 1);
	}
}
#endif /* USE_FUNC_UNLOCK */

#ifdef USE_FUNC_CREATELOCK
/**
 * @brief 高速ロックの生成
 *
 * FastLock構造体を初期化し、内部で使用するセマフォを生成します。
 *
 * @param lock 初期化するFastLock構造体へのポインタ
 * @param name セマフォの名前（NULL可）
 *
 * @return エラーコード
 *   @retval E_OK 正常終了
 *   @retval E_LIMIT セマフォ数が上限に達した
 *
 * @note 初期状態はアンロック状態（cnt=-1）です。
 *       セマフォはTA_TPRI属性（優先度順待ち）で生成されます。
 */
EXPORT ER CreateLock( FastLock *lock, CONST UB *name )
{
	T_CSEM	csem;
	ER	ercd;

	if ( name == NULL ) {
		csem.exinf = NULL;
	} else {
		strncpy((char*)&csem.exinf, (char*)name, sizeof(csem.exinf));
	}
	csem.sematr  = TA_TPRI;
	csem.isemcnt = 0;
	csem.maxsem  = 1;

	ercd = tk_cre_sem(&csem);
	if ( ercd < E_OK ) {
		return ercd;
	}

	lock->id = ercd;
	lock->cnt = -1;

	return E_OK;
}
#endif /* USE_FUNC_CREATELOCK */

#ifdef USE_FUNC_DELETELOCK
/**
 * @brief 高速ロックの削除
 *
 * FastLockで使用していたセマフォを削除し、構造体を無効化します。
 *
 * @param lock 削除するFastLock構造体へのポインタ
 *
 * @note ロックが取得されている状態や待ちタスクがある状態で
 *       この関数を呼び出した場合の動作は未定義です。
 *       削除後はlock->idを0に設定します。
 */
EXPORT void DeleteLock( FastLock *lock )
{
	if ( lock->id > 0 ) {
		tk_del_sem(lock->id);
	}
	lock->id = 0;
}
#endif /* USE_FUNC_DELETELOCK */
