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
 * @file	ready_queue.h
 * @brief	実行可能キュー（ready queue）の操作ルーチン
 *
 * 実行可能状態のタスクを優先度別に管理する実行可能キューの
 * 構造定義と、初期化・挿入・削除・先頭タスク取得などの
 * インライン操作関数を提供します。
 */

#ifndef _READY_QUEUE_
#define _READY_QUEUE_

#include "tstdlib.h"

/*
 * 実行可能キュー構造の定義
 *	実行可能キューには優先度ごとのタスクキュー 'tskque' があり、
 *	タスクの TCB は該当する優先度のキューに登録されます。
 *	実行可能キューを効率よく探索するため、優先度ごとにタスクの
 *	有無を示すビットマップ領域 'bitmap' を用意しています。
 *
 *	また、実行可能キュー内の最高優先度タスクを効率よく探索する
 *	ため、最高のタスク優先度を 'top_priority' フィールドに保持
 *	します。実行可能キューが空のときは、このフィールドの値を
 *	NUM_TSKPRI に設定します。その際 'tskque[top_priority]' の参照
 *	で '0' を返せるように、常に '0' である 'null' フィールドを
 *	置いています。
 *
 *	カーネルロックを保持した READY 状態のタスクは同時に複数
 *	存在しません。
 */

#define BITMAPSZ	( sizeof(UINT) * 8 )
#define NUM_BITMAP	( (NUM_TSKPRI + BITMAPSZ - 1) / BITMAPSZ )

typedef	struct ready_queue {
	INT	top_priority;		/* 実行可能キュー内の最高優先度 */
	QUEUE	tskque[NUM_TSKPRI];	/* 優先度ごとのタスクキュー */
	TCB	*null;			/* 実行可能キューが空のときの番兵（常に0） */
	UINT	bitmap[NUM_BITMAP];	/* 優先度ごとのビットマップ領域 */
	TCB	*klocktsk;	/* カーネルロック保持中の READY タスク */
} RDYQUE;

IMPORT RDYQUE	knl_ready_queue;

#if NUM_TSKPRI <= INT_BITWIDTH
/**
 * @brief ビットマップからの最高優先度の探索
 *
 * ビットマップの pos ビット目から上位優先度番号方向へ走査し、
 * 最初にセットされているビット位置（＝最高優先度）を返します。
 *
 * @param bitmap 優先度ごとのタスク有無を示すビットマップ
 * @param pos    探索を開始する優先度（ビット位置）
 * @return 見つかった最高優先度。該当がなければ NUM_TSKPRI を返します。
 */
Inline INT knl_ready_queue_calc_top_priority( UINT bitmap, INT pos )
{
	for ( ; pos < NUM_TSKPRI; pos++ ) {
		if ( bitmap & (1U << pos) ) {
			return pos;
		}
	}
	return NUM_TSKPRI;
}
#endif

/**
 * @brief 実行可能キューの初期化
 *
 * 全優先度のタスクキューとビットマップを空にし、
 * top_priority を NUM_TSKPRI（空を示す値）に設定します。
 *
 * @param rq 対象の実行可能キュー
 */
Inline void knl_ready_queue_initialize( RDYQUE *rq )
{
	INT	i;

	rq->top_priority = NUM_TSKPRI;
	for ( i = 0; i < NUM_TSKPRI; i++ ) {
		QueInit(&rq->tskque[i]);
	}
	rq->null = NULL;
	rq->klocktsk = NULL;
	knl_memset(rq->bitmap, 0, sizeof(rq->bitmap));
}

/**
 * @brief 実行可能キュー内の最高優先度タスクの取得
 *
 * @param rq 対象の実行可能キュー
 * @return 最高優先度タスクの TCB。キューが空の場合は NULL を返します。
 * @note カーネルロック保持タスクがあれば、優先度によらず
 *       そのタスクを返します。
 */
Inline TCB* knl_ready_queue_top( RDYQUE *rq )
{
	/* カーネルロック保持タスクがあれば、それが最高優先度タスク */
	if ( rq->klocktsk != NULL ) {
		return rq->klocktsk;
	}

	return (TCB*)rq->tskque[rq->top_priority].next;
}

/**
 * @brief 実行可能キュー内の最高優先度の取得
 *
 * @param rq 対象の実行可能キュー
 * @return 最高優先度タスクの優先度。キューが空の場合は NUM_TSKPRI を返します。
 */
Inline INT knl_ready_queue_top_priority( const RDYQUE *rq )
{
	return rq->top_priority;
}

/**
 * @brief 実行可能キューへのタスクの挿入（同一優先度の末尾）
 *
 * 'tcb' のタスク優先度に対応するタスクキューの末尾に挿入します。
 * ビットマップ領域の該当ビットをセットし、必要に応じて
 * 'top_priority' を更新します。
 *
 * @param rq  対象の実行可能キュー
 * @param tcb 挿入するタスクの TCB
 * @retval TRUE  'top_priority' を更新した
 * @retval FALSE 'top_priority' の更新なし
 * @note 挿入タスクがカーネルロック保持中なら klocktsk に登録します。
 */
Inline BOOL knl_ready_queue_insert( RDYQUE *rq, TCB *tcb )
{
	INT	priority = tcb->priority;

	QueInsert(&tcb->tskque, &rq->tskque[priority]);
#if NUM_TSKPRI <= INT_BITWIDTH
	rq->bitmap[0] |= (1U << priority);
#else
	knl_bitset(rq->bitmap, priority);
#endif

	if ( tcb->klocked ) {
		rq->klocktsk = tcb;
	}

	if ( priority < rq->top_priority ) {
		rq->top_priority = priority;
		return TRUE;
	}
	return FALSE;
}

/**
 * @brief 実行可能キューへのタスクの挿入（同一優先度の先頭）
 *
 * 'tcb' のタスク優先度に対応するタスクキューの先頭に挿入します。
 * ビットマップ領域の該当ビットをセットし、必要に応じて
 * 'top_priority' を更新します。
 *
 * @param rq  対象の実行可能キュー
 * @param tcb 挿入するタスクの TCB
 * @note 挿入タスクがカーネルロック保持中なら klocktsk に登録します。
 */
Inline void knl_ready_queue_insert_top( RDYQUE *rq, TCB *tcb )
{
	INT	priority = tcb->priority;

	QueInsert(&tcb->tskque, rq->tskque[priority].next);
#if NUM_TSKPRI <= INT_BITWIDTH
	rq->bitmap[0] |= (1U << priority);
#else
	knl_bitset(rq->bitmap, priority);
#endif

	if ( tcb->klocked ) {
		rq->klocktsk = tcb;
	}

	if ( priority < rq->top_priority ) {
		rq->top_priority = priority;
	}
}

/**
 * @brief 実行可能キューからのタスクの削除
 *
 * 該当優先度のタスクキューから TCB を取り外し、タスクキューが
 * 空になった場合はビットマップ領域の該当ビットをクリアします。
 * さらに、削除したタスクが最高優先度だった場合は、ビットマップ
 * 領域を用いて次に高い優先度を探索し、'top_priority' を更新します。
 *
 * @param rq  対象の実行可能キュー
 * @param tcb 削除するタスクの TCB
 * @note カーネルロック待ち中のタスクは、実際にはロック待ち
 *       キューにつながれているため、キューから外して
 *       klockwait をクリアするのみで復帰します。
 */
Inline void knl_ready_queue_delete( RDYQUE *rq, TCB *tcb )
{
	INT	priority = tcb->priority;
#if NUM_TSKPRI > INT_BITWIDTH
	INT	i;
#endif

	if ( rq->klocktsk == tcb ) {
		rq->klocktsk = NULL;
	}

	QueRemove(&tcb->tskque);
	if ( tcb->klockwait ) {
		/* カーネルロック待ちキューからの削除 */
		tcb->klockwait = FALSE;
		return;
	}
	if ( !isQueEmpty(&rq->tskque[priority]) ) {
		return;
	}

#if NUM_TSKPRI <= INT_BITWIDTH
	rq->bitmap[0] &= ~(1U << priority);
#else
	knl_bitclr(rq->bitmap, priority);
#endif
	if ( priority != rq->top_priority ) {
		return;
	}

#if NUM_TSKPRI <= INT_BITWIDTH
	rq->top_priority = knl_ready_queue_calc_top_priority(rq->bitmap[0], priority);
#else
	i = knl_bitsearch1(rq->bitmap, priority, NUM_TSKPRI - priority);
	if ( i >= 0 ) {
		rq->top_priority = priority + i;
	} else {
		rq->top_priority = NUM_TSKPRI;
	}
#endif
}

/**
 * @brief 実行可能キューの回転
 *
 * 実行可能キュー内の優先度 'priority' のタスクキューについて、
 * 先頭のタスクを末尾へ移動します。キューが空なら何もしません。
 *
 * @param rq       対象の実行可能キュー
 * @param priority 回転させるタスクキューの優先度
 */
Inline void knl_ready_queue_rotate( RDYQUE *rq, INT priority )
{
	QUEUE	*tskque = &rq->tskque[priority];
	TCB	*tcb;

	tcb = (TCB*)QueRemoveNext(tskque);
	if ( tcb != NULL ) {
		QueInsert((QUEUE*)tcb, tskque);
	}
}

/**
 * @brief タスクの同一優先度キュー末尾への移動
 *
 * 'tcb' を同一優先度のタスクキューの末尾へ移動します。
 *
 * @param rq  対象の実行可能キュー
 * @param tcb 移動するタスクの TCB
 * @return 移動後にキュー先頭となったタスクの TCB
 */
Inline TCB* knl_ready_queue_move_last( RDYQUE *rq, TCB *tcb )
{
	QUEUE	*tskque = &rq->tskque[tcb->priority];

	QueRemove(&tcb->tskque);
	QueInsert(&tcb->tskque, tskque);

	return (TCB*)tskque->next;	/* 新たにキュー先頭となったタスク */
}

#endif /* _READY_QUEUE_ */
