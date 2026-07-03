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
 * @file	time_calls.h
 * @brief	時間管理機能の内部定義
 *
 * 周期ハンドラ管理ブロック（CYCCB）とアラームハンドラ管理ブロック
 * （ALMCB）の定義、およびタイマイベントキューへの登録・次回起動時刻
 * 算出のインライン関数を提供します。
 */

#ifndef _TIME_CALLS_H
#define _TIME_CALLS_H

#define	DIFF_TRON_UTC		(473385600000LL)		/* UTC と TRON 時間の差分（ms） */

/*
 * 周期ハンドラ管理ブロック
 */
typedef struct cyclic_handler_control_block {
	void	*exinf;		/* 拡張情報 */
	ATR	cycatr;		/* 周期ハンドラ属性 */
	FP	cychdr;		/* 周期ハンドラアドレス */
	UINT	cycstat;	/* 周期ハンドラの動作状態 */
	RELTIM	cyctim;		/* 起動周期 */
	TMEB	cyctmeb;	/* タイマイベントブロック */
#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} CYCCB;

IMPORT CYCCB	knl_cyccb_table[];	/* 周期ハンドラ管理ブロックテーブル */
IMPORT QUEUE	knl_free_cyccb;	/* 未使用管理ブロックのキュー（FreeQue） */

#define get_cyccb(id)	( &knl_cyccb_table[INDEX_CYC(id)] )


/**
 * @brief	周期ハンドラの次回起動時刻の算出
 *
 * 前回の起動予定時刻に起動周期を加えた時刻を求めます。その時刻が
 * 既に到達済みの場合は、現在時刻より後になるまで周期単位で
 * 繰り上げます。
 *
 * @param	cyccb	対象の周期ハンドラ管理ブロック
 * @return	次回起動時刻（絶対時間）
 */
Inline ABSTIM knl_cyc_next_time( CYCCB *cyccb )
{
	ABSTIM		tm, cur;

	cur = lltoul(knl_current_time);
	tm = cyccb->cyctmeb.time + cyccb->cyctim;

	if ( knl_abstim_reached(cur, tm) ) {
		/* 現在時刻より後になるように調整 */
		tm = ((cur - cyccb->cyctmeb.time) / cyccb->cyctim + 1) * cyccb->cyctim + cyccb->cyctmeb.time;
	}

	return tm;
}

IMPORT void knl_call_cychdr( CYCCB* cyccb );

/**
 * @brief	周期ハンドラのタイマイベントキューへの登録
 *
 * 指定した絶対時刻に knl_call_cychdr が呼び出されるように
 * タイマイベントを登録します。
 *
 * @param	cyccb	対象の周期ハンドラ管理ブロック
 * @param	tm	起動時刻（絶対時間）
 */
Inline void knl_cyc_timer_insert( CYCCB *cyccb, ABSTIM tm )
{
	knl_timer_insert_abs(&cyccb->cyctmeb, tm, (CBACK)knl_call_cychdr, cyccb);
}


/*
 * アラームハンドラ管理ブロック
 */
typedef struct alarm_handler_control_block {
	void	*exinf;		/* 拡張情報 */
	ATR	almatr;		/* アラームハンドラ属性 */
	FP	almhdr;		/* アラームハンドラアドレス */
	UINT	almstat;	/* アラームハンドラの動作状態 */
	TMEB	almtmeb;	/* タイマイベントブロック */
#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} ALMCB;

IMPORT ALMCB	knl_almcb_table[];	/* アラームハンドラ管理ブロックテーブル */
IMPORT QUEUE	knl_free_almcb;	/* 未使用管理ブロックのキュー（FreeQue） */

#define get_almcb(id)	( &knl_almcb_table[INDEX_ALM(id)] )

IMPORT void knl_call_almhdr( ALMCB *almcb );

/**
 * @brief	アラームハンドラのタイマイベントキューへの登録
 *
 * 現在時刻から指定の相対時間が経過した時刻に knl_call_almhdr が
 * 呼び出されるようにタイマイベントを登録します。
 *
 * @param	almcb	対象のアラームハンドラ管理ブロック
 * @param	reltim	起動までの相対時間（ms）
 */
Inline void knl_alm_timer_insert( ALMCB *almcb, RELTIM reltim )
{
	ABSTIM	tm;

	/* 指定時間経過後のハンドラ起動を保証するため
	   TIMER_PERIOD を加算する */
	tm = lltoul(knl_current_time) + reltim + TIMER_PERIOD;

	knl_timer_insert_abs(&almcb->almtmeb, tm, (CBACK)knl_call_almhdr, almcb);
}


#endif /* _TIME_CALLS_H */
