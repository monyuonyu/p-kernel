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
 * @file wait.c
 * @brief 同期処理の共通ルーチン
 * 
 * T-Kernelの同期オブジェクトで共通して使用される
 * 待ち状態管理の基本機能を実装する。
 * 
 * 主な機能：
 * - 待ち状態の解除処理
 * - タイムアウト処理
 * - 待ちキューの管理
 * - 汎用制御ブロック（GCB）の操作
 */

/** [BEGIN Common Definitions] */
#include "kernel.h"
#include "task.h"
#include "wait.h"
/** [END Common Definitions] */

#ifdef USE_FUNC_WAIT_RELEASE_OK
/**
 * @brief 正常終了での待ち解除
 * @param tcb 対象タスクの制御ブロック
 * 
 * 指定されたタスクの待ち状態を正常終了（E_OK）で解除します。
 * 待ち解除処理を実行し、エラーコードにE_OKを設定します。
 * 
 * @note セマフォやミューテックスなどの獲得成功時に使用されます
 */
EXPORT void knl_wait_release_ok( TCB *tcb )
{
	knl_wait_release(tcb);
	*tcb->wercd = E_OK;
}
#endif /* USE_FUNC_WAIT_RELEASE_OK */

#ifdef USE_FUNC_WAIT_RELEASE_OK_ERCD
/**
 * @brief 指定エラーコードでの待ち解除
 * @param tcb 対象タスクの制御ブロック
 * @param ercd 設定するエラーコード
 * 
 * 指定されたタスクの待ち状態を指定されたエラーコードで解除します。
 * 待ち解除処理を実行し、指定されたエラーコードを設定します。
 * 
 * @note 部分成功や特定の成功状態を返す必要がある場合に使用されます
 */
EXPORT void knl_wait_release_ok_ercd( TCB *tcb, ER ercd )
{
	knl_wait_release(tcb);
	*tcb->wercd = ercd;
}
#endif /* USE_FUNC_WAIT_RELEASE_OK_ERCD */

#ifdef USE_FUNC_WAIT_RELEASE_NG
/**
 * @brief エラー終了での待ち解除
 * @param tcb 対象タスクの制御ブロック
 * @param ercd 設定するエラーコード
 * 
 * 指定されたタスクの待ち状態をエラー終了で解除します。
 * 待ち解除処理を実行し、待ち解除フックが定義されている場合は
 * それを実行し、指定されたエラーコードを設定します。
 * 
 * @note リソースの削除や強制的な待ち解除時に使用されます
 * @note rel_wai_hookにより、オブジェクト固有のクリーンアップが実行されます
 */
EXPORT void knl_wait_release_ng( TCB *tcb, ER ercd )
{
	knl_wait_release(tcb);
	if ( tcb->wspec->rel_wai_hook != NULL ) {
		(*tcb->wspec->rel_wai_hook)(tcb);
	}
	*tcb->wercd = ercd;
}
#endif /* USE_FUNC_WAIT_RELEASE_NG */

#ifdef USE_FUNC_WAIT_RELEASE_TMOUT
/**
 * @brief タイムアウトによる待ち解除
 * @param tcb 対象タスクの制御ブロック
 * 
 * タイムアウトによりタスクの待ち状態を解除します。
 * 待ちキューからの削除、非待ち状態への変更、そして
 * 必要に応じて待ち解除フックを実行します。
 * 
 * 処理内容：
 * - タスクを待ちキューから削除
 * - タスクを非待ち状態に変更
 * - 待ち解除フックの実行（定義されている場合）
 * 
 * @note タイマーコールバック関数として使用されます
 * @note エラーコードはE_TMOUTが設定済みです
 */
EXPORT void knl_wait_release_tmout( TCB *tcb )
{
	QueRemove(&tcb->tskque);
	knl_make_non_wait(tcb);
	if ( tcb->wspec->rel_wai_hook != NULL ) {
		(*tcb->wspec->rel_wai_hook)(tcb);
	}
}
#endif /* USE_FUNC_WAIT_RELEASE_TMOUT */

#ifdef USE_FUNC_MAKE_WAIT
/**
 * @brief アクティブタスクを待ち状態に変更
 * 
 * 現在実行中のタスクを待ち状態に変更し、
 * タイマーイベントキューに登録する。
 * 
 * @param tmout タイムアウト時間（ミリ秒）
 * @param atr 待ち属性
 * @note TMO_FEVRを指定した場合は無限待ちとなる
 */
EXPORT void knl_make_wait( TMO tmout, ATR atr )
{
	switch ( knl_ctxtsk->state ) {
	  case TS_READY:
		knl_make_non_ready(knl_ctxtsk);
		knl_ctxtsk->state = TS_WAIT;
		break;
	  case TS_SUSPEND:
		knl_ctxtsk->state = TS_WAITSUS;
		break;
	}
	knl_timer_insert(&knl_ctxtsk->wtmeb, tmout, (CBACK)knl_wait_release_tmout, knl_ctxtsk);
}
#endif /* USE_FUNC_MAKE_WAIT */

#ifdef USE_FUNC_MAKE_WAIT_RELTIM
/**
 * @brief 相対時間指定でアクティブタスクを待ち状態に変更
 * @param tmout 相対タイムアウト時間（マイクロ秒単位）
 * @param atr 待ち属性
 * 
 * 現在実行中のタスクを待ち状態に変更し、
 * 相対時間で指定されたタイムアウト時間で
 * タイマーイベントキューに登録します。
 * 
 * @note TMO_FEVRを指定した場合は無限待ちとなります
 * @note サスペンド状態のタスクは待ちサスペンド状態になります
 */
EXPORT void knl_make_wait_reltim( RELTIM tmout, ATR atr )
{
	switch ( knl_ctxtsk->state ) {
	  case TS_READY:
		knl_make_non_ready(knl_ctxtsk);
		knl_ctxtsk->state = TS_WAIT;
		break;
	  case TS_SUSPEND:
		knl_ctxtsk->state = TS_WAITSUS;
		break;
	}
	knl_timer_insert_reltim(&knl_ctxtsk->wtmeb, tmout, (CBACK)knl_wait_release_tmout, knl_ctxtsk);
}
#endif /* USE_FUNC_MAKE_WAIT_RELTIM */

#ifdef USE_FUNC_WAIT_DELETE
/**
 * @brief 待ちキューの全タスクをE_DLTエラーで解除
 * @param wait_queue 対象の待ちキュー
 * 
 * 指定された待ちキューに接続されている全タスクを解除し、
 * E_DLTエラーを設定します。オブジェクトの削除時に使用されます。
 * 
 * 処理内容：
 * - 待ちキューが空になるまで繰り返し処理
 * - 各タスクの待ち解除処理
 * - エラーコードにE_DLTを設定
 * 
 * @note セマフォ、ミューテックス、イベントフラグなどの削除時に使用されます
 * @note 待ち中のタスクにオブジェクト削除を通知します
 */
EXPORT void knl_wait_delete( QUEUE *wait_queue )
{
	TCB	*tcb;

	while ( !isQueEmpty(wait_queue) ) {
		tcb = (TCB*)wait_queue->next;
		knl_wait_release(tcb);
		*tcb->wercd = E_DLT;
	}
}
#endif /* USE_FUNC_WAIT_DELETE */

#ifdef USE_FUNC_WAIT_TSKID
/**
 * @brief 待ちキューの先頭タスクIDを取得
 * @param wait_queue 対象の待ちキュー
 * @return 先頭タスクのID、キューが空の場合は0
 * 
 * 指定された待ちキューの先頭にあるタスクのIDを取得します。
 * キューが空の場合は0を返します。
 * 
 * @note デバッグ情報取得や状態参照時に使用されます
 * @note FIFOキュー、優先度キューのどちらでも使用可能です
 */
EXPORT ID knl_wait_tskid( QUEUE *wait_queue )
{
	if ( isQueEmpty(wait_queue) ) {
		return 0;
	}

	return ((TCB*)wait_queue->next)->tskid;
}
#endif /* USE_FUNC_WAIT_TSKID */

#ifdef USE_FUNC_GCB_MAKE_WAIT
/**
 * @brief 汎用制御ブロック（GCB）用の待ち状態設定
 * @param gcb 汎用制御ブロックへのポインタ
 * @param tmout タイムアウト時間（TMO_POLでポーリング）
 * 
 * アクティブタスクを待ち状態に変更し、タイマー待ちキューと
 * オブジェクト待ちキューに接続します。また、'knl_ctxtsk'の'wid'を設定します。
 * 
 * 処理内容：
 * - エラーコードのE_TMOUTでの初期化
 * - ポーリングでない場合の待ち状態設定
 * - オブジェクトIDの設定
 * - タイマーと待ちキューへの登録
 * - TA_TPRI属性に応じたキュー挿入方式の選択
 * 
 * @note セマフォ、ミューテックス、イベントフラグなどの共通処理です
 * @note TMO_POLの場合は待ち状態にはならず、即座に戻ります
 */
EXPORT void knl_gcb_make_wait( GCB *gcb, TMO tmout )
{
	*knl_ctxtsk->wercd = E_TMOUT;
	if ( tmout != TMO_POL ) {
		knl_ctxtsk->wid = gcb->objid;
		knl_make_wait(tmout, gcb->objatr);
		if ( (gcb->objatr & TA_TPRI) != 0 ) {
			knl_queue_insert_tpri(knl_ctxtsk, &gcb->wait_queue);
		} else {
			QueInsert(&knl_ctxtsk->tskque, &gcb->wait_queue);
		}
	}
}
#endif /* USE_FUNC_GCB_MAKE_WAIT */

#ifdef USE_FUNC_GCB_CHANGE_PRIORITY
/**
 * @brief タスク優先度変更時の待ちキュー位置調整
 * @param gcb 汎用制御ブロックへのポインタ
 * @param tcb 優先度が変更されたタスクの制御ブロック
 * 
 * タスクの優先度が変更された場合に、待ちキュー内での
 * タスク位置を調整します。オブジェクト属性でTA_TPRIが
 * 指定されている場合のみ呼び出されます。
 * 
 * 処理内容：
 * - タスクを待ちキューから一旦削除
 * - 新しい優先度に応じて適切な位置に再挿入
 * 
 * @note 優先度別待ちキュー（TA_TPRI）でのみ有効です
 * @note ミューテックスの優先度継承時に使用されます
 */
EXPORT void knl_gcb_change_priority( GCB *gcb, TCB *tcb )
{
	QueRemove(&tcb->tskque);
	knl_queue_insert_tpri(tcb, &gcb->wait_queue);
}
#endif /* USE_FUNC_GCB_CHANGE_PRIORITY */

#ifdef USE_FUNC_GCB_TOP_OF_WAIT_QUEUE
/**
 * @brief 待ちキューの第一候補タスクの検索
 * @param gcb 汎用制御ブロックへのポインタ
 * @param tcb 検索対象に含めるタスクの制御ブロック
 * @return 最初に実行されるべきタスクの制御ブロック
 * 
 * 指定されたタスクを含めて、待ちキューの最初のタスクを検索します。
 * （注意：タスクを待ちキューに挿入するわけではありません）
 * 
 * 処理内容：
 * - 待ちキューが空の場合は指定されたタスクを返却
 * - FIFOキューの場合はキューの先頭タスクを返却
 * - 優先度キューの場合は優先度比較で決定
 * 
 * @note セマフォやミューテックスの次の取得者決定に使用されます
 * @note キューの状態を変更せずに次の候補を調べるための関数です
 */
EXPORT TCB* knl_gcb_top_of_wait_queue( GCB *gcb, TCB *tcb )
{
	TCB	*q;

	if ( isQueEmpty(&gcb->wait_queue) ) {
		return tcb;
	}

	q = (TCB*)gcb->wait_queue.next;
	if ( (gcb->objatr & TA_TPRI) == 0 ) {
		return q;
	}

	return ( tcb->priority < q->priority )? tcb: q;
}
#endif /* USE_FUNC_GCB_TOP_OF_WAIT_QUEUE */
