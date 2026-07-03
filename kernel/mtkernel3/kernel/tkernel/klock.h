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
 * @file	klock.h
 * @brief	カーネルロック（オブジェクトロック）の定義
 *
 * カーネル内部オブジェクトの排他制御に用いるロック機構の定義です。
 * ロックを獲得したタスクは最高実行優先度として扱われます。
 * ロックのネスト（多重獲得）はできません。
 */

#ifndef _KLOCK_
#define _KLOCK_

typedef struct objlock {
	QUEUE		wtskq;		/* ロック待ちタスクキュー */
} OBJLOCK;

/**
 * @brief オブジェクトロックの初期化
 *
 * ロックを空き（非ロック）状態に初期化します。
 *
 * @param loc 対象のオブジェクトロック
 */
Inline void knl_InitOBJLOCK( OBJLOCK *loc )
{
	loc->wtskq.next = NULL;
}
IMPORT void knl_LockOBJ( OBJLOCK* );
IMPORT void knl_UnlockOBJ( OBJLOCK* );

/**
 * @brief オブジェクトロックのロック状態判定
 *
 * @param loc 対象のオブジェクトロック
 * @retval TRUE  ロックされている
 * @retval FALSE ロックされていない
 */
Inline BOOL knl_isLockedOBJ( OBJLOCK *loc )
{
	return ( loc->wtskq.next != NULL )? TRUE: FALSE;
}

#endif /* KLOCK */
