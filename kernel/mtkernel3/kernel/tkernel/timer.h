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
 * @file	timer.h
 * @brief	システムタイマモジュールの定義
 *
 * システム時刻の内部表現（LSYSTIM / ABSTIM）と相互変換、
 * タイマイベントブロック（TMEB）、およびタイマイベントキューの
 * 登録・削除インタフェースを定義します。
 */

#ifndef _TIMER_
#define _TIMER_

#include "longlong.h"

/*
 * SYSTIM の内部表現と相互変換
 */
typedef	D	LSYSTIM;	/* SYSTIM の内部表現（64ビット整数） */

/**
 * @brief	SYSTIM から内部表現（LSYSTIM）への変換
 *
 * @param	time	変換元の SYSTIM（hi/lo 分割形式）
 * @return	64ビット整数に合成した時刻値
 */
Inline LSYSTIM knl_toLSYSTIM( CONST SYSTIM *time )
{
	LSYSTIM		ltime;

	hilo_ll(ltime, time->hi, time->lo);

	return ltime;
}

/**
 * @brief	内部表現（LSYSTIM）から SYSTIM への変換
 *
 * @param	ltime	変換元の64ビット時刻値
 * @return	hi/lo 分割形式の SYSTIM
 */
Inline SYSTIM knl_toSYSTIM( LSYSTIM ltime )
{
	SYSTIM		time;

	ll_hilo(time.hi, time.lo, ltime);

	return time;
}

/*
 * 絶対時間（SYSTIM の下位32ビットに相当）
 */
typedef	UW	ABSTIM;

#define ABSTIM_DIFF_MIN  (0x7FFFFFFF)

/**
 * @brief	絶対時間の到達判定
 *
 * 32ビットの周回（ラップアラウンド）を考慮して、現在時刻 curtim が
 * イベント時刻 evttim に到達したかどうかを判定します。
 *
 * @param	curtim	現在時刻（絶対時間）
 * @param	evttim	イベント時刻（絶対時間）
 * @retval	TRUE	到達済み
 * @retval	FALSE	未到達
 */
Inline BOOL knl_abstim_reached( ABSTIM curtim, ABSTIM evttim )
{
	return (ABSTIM)(curtim - evttim) <= (ABSTIM)ABSTIM_DIFF_MIN;
}

/*
 * タイマイベントブロックの定義
 */
typedef void	(*CBACK)(void *);	/* コールバック関数の型 */

typedef struct timer_event_block {
	QUEUE	queue;		/* タイマイベントキュー */
	ABSTIM	time;		/* イベント発生時刻 */
	CBACK	callback;	/* コールバック関数 */
	void	*arg;		/* コールバック関数へ渡す引数 */
} TMEB;

/*
 * 現在時刻（ソフトウェアクロック）
 */
IMPORT LSYSTIM	knl_current_time;	/* システム稼働時間 */
IMPORT LSYSTIM	knl_real_time_ofs;	/* 実時刻との差分 */

/*
 * タイマイベントキュー
 */
IMPORT QUEUE	knl_timer_queue;

/*
 * タイマイベントキューへの登録
 */
IMPORT void knl_timer_insert( TMEB *evt, TMO tmout, CBACK cback, void *arg );
IMPORT void knl_timer_insert_reltim( TMEB *event, RELTIM tmout, CBACK callback, void *arg );
IMPORT void knl_timer_insert_abs( TMEB *evt, ABSTIM time, CBACK cback, void *arg );

/**
 * @brief	タイマイベントキューからの削除
 *
 * @param	event	削除するタイマイベントブロック
 */
Inline void knl_timer_delete( TMEB *event )
{
	QueRemove(&event->queue);
	/* KILL-CHURN-CRASH hardening (DEFENSIVE — see the note in wait.h).
	 * QueRemove unlinks the node from its neighbours but leaves next/prev
	 * pointing AT them; the node is not returned to the self-linked
	 * (empty) state.  A second QueRemove on the same node would then write
	 * through those stale pointers into whatever now occupies the queue.
	 * QueInit re-self-links, which makes QueRemove's `next != entry` no-op
	 * guard hold and every later delete idempotent. */
	QueInit(&event->queue);
}

#endif /* _TIMER_ */
