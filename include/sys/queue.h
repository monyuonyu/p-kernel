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
 * @file	queue.h
 * @brief	キュー操作
 *
 * カーネル内部で使用する双方向リンクキュー（リング構造）の型定義と
 * 操作用 inline 関数群を提供します。タスクの待ち行列や実行可能
 * キュー（ready queue）などの基本データ構造として使用されます。
 */

#ifndef	__SYS_QUEUE_H__
#define __SYS_QUEUE_H__

#include <tk/tkernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief	双方向リンクキュー（リング構造）
 *
 * next と prev で環状に連結します。ヘッダ自身も要素と同じ構造で、
 * 空のキューはヘッダの next / prev が自分自身を指します。
 */
typedef struct queue {
	struct queue	*next;		/* 次エントリ */
	struct queue	*prev;		/* 前エントリ */
} QUEUE;

/**
 * @brief	キューの初期化
 *
 * que の next / prev を自分自身に向け、空のキューにします。
 *
 * @param que	初期化するキューヘッダ
 */
Inline void QueInit( QUEUE *que )
{
	que->next = (struct queue *)que;
	que->prev = (struct queue *)que;
}

/**
 * @brief	キューが空かどうかの判定
 *
 * @param que	判定するキューヘッダ
 *
 * @retval TRUE		キューは空
 * @retval FALSE	キューに要素がある
 */
Inline BOOL isQueEmpty( QUEUE *que )
{
	return ( que->next == que )? TRUE: FALSE;
}

/**
 * @brief	キューへの挿入
 *
 * entry を que の直前に挿入します。que がキューヘッダの場合、
 * キューの末尾への追加になります。
 *
 * @param entry	挿入するエントリ
 * @param que	挿入位置（この直前に挿入）
 */
Inline void QueInsert( QUEUE *entry, QUEUE *que )
{
	entry->prev = (struct queue*) que->prev;
	entry->next = que;
	que->prev->next = entry;
	que->prev = entry;
}

/**
 * @brief	キューからの削除
 *
 * entry をキューから取り外します。
 *
 * @param entry	削除するエントリ
 *
 * @note entry がどのキューにもつながっていない（next が自分自身を
 *	指す）場合は何もしません。取り外した entry 自身のリンクは
 *	更新されないため、再利用時は初期化が必要です。
 */
Inline void QueRemove( QUEUE *entry )
{
	if ( entry->next != entry ) {
		entry->prev->next = (struct queue*) entry->next;
		entry->next->prev = (struct queue*) entry->prev;
	}
}

/**
 * @brief	先頭エントリの取り出し
 *
 * que の直後のエントリをキューから削除し、そのエントリを返します。
 *
 * @param que	キューヘッダ
 *
 * @return 削除したエントリ。que が空の場合は NULL。
 *
 * @note 取り出した entry 自身のリンクは更新されないため、
 *	再利用時は初期化が必要です。
 */
Inline QUEUE* QueRemoveNext( QUEUE *que )
{
	QUEUE	*entry;

	if ( que->next == que ) {
		return NULL;
	}

	entry = que->next;
	que->next = (struct queue*)entry->next;
	entry->next->prev = que;

	return entry;
}

#ifdef __cplusplus
}
#endif
#endif /* __SYS_QUEUE_H__ */
