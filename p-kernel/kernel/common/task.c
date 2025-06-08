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
 * @file task.c
 * @brief タスク制御機能 (Task Control Functions)
 *
 * このファイルは、T-Kernelのタスク管理の中核となる機能を実装します。
 * マルチタスク環境におけるタスクの生成・削除、状態遷移、スケジューリングなど、
 * 基本的なタスク制御機能を提供します。
 *
 * 主な機能：
 * - タスクの状態管理（実行可能(READY)、待機(WAIT)、休止(DORMANT)など）
 * - タスクスケジューリング（優先度ベースのプリエンプティブ方式）
 * - 同一優先度タスク間のラウンドロビンスケジューリング
 * - 実行可能キュー(Ready Queue)の管理
 * - タスク優先度の動的変更
 * - タスクコンテキストの保存・復帰
 *
 * @note T-KernelはリアルタイムOSとして設計されており、タスクスケジューリングは
 *       厳密な優先度ベースで行われます。最高優先度のタスクが常に実行されます。
 * @note このモジュールはカーネルの最も重要な部分の1つであり、
 *       多くのシステムコールの基盤となります。
 */

/** [共通定義開始] */
#include "kernel.h"
#include "task.h"
#include "ready_queue.h"
#include "wait.h"
#include "cpu_task.h"
#include "tkdev_timer.h"
#include "check.h"
/** [共通定義終了] */

#ifdef USE_FUNC_CTXTSK
/**
 * @brief タスクディスパッチ禁止状態フラグ
 *
 * このフラグはタスクディスパッチの許可/禁止状態を制御します。
 * task.hで定義されているDDS_XXXの値を取ります：
 * - DDS_ENABLE: ディスパッチ許可状態
 * - DDS_DISABLE: ディスパッチ禁止状態
 */
Noinit(EXPORT INT	knl_dispatch_disabled);

/**
 * @brief タスク実行制御変数群
 * 
 * タスクスケジューリングの中核となる以下のグローバル変数群：
 */
Noinit(EXPORT TCB	*knl_ctxtsk);		/**< 現在実行中のタスク(Current Task) */
Noinit(EXPORT TCB	*knl_schedtsk);	/**< 次に実行予定のタスク(Scheduled Task) */
Noinit(EXPORT RDYQUE	knl_ready_queue);	/**< 実行可能状態のタスクを管理するキュー(Ready Queue) */
#endif /* USE_FUNC_CTXTSK */

#ifdef USE_FUNC_TCB_TABLE
/**
 * @brief タスク制御情報
 * 
 * 全タスクの制御ブロックと管理キュー
 */
Noinit(EXPORT TCB	knl_tcb_table[NUM_TSKID]);	/**< 全タスクの制御ブロックを格納する配列(TCB Table) */
Noinit(EXPORT QUEUE	knl_free_tcb);			/**< 未使用のTCBを管理するキュー(Free TCB Queue) */
#endif /* USE_FUNC_TCB_TABLE */

#ifdef USE_FUNC_TASK_INITIALIZE
/**
 * @brief タスク制御ブロック（TCB）の初期化
 * @return エラーコード（E_OK: 成功、E_SYS: システムエラー）
 * 
 * タスク管理システム全体を初期化します。
 * - 実行制御情報の初期化
 * - 実行可能キューの初期化
 * - 全TCBの初期化と未使用キューへの登録
 * - ディスパッチ許可状態の設定
 * 
 * @note システム起動時に一度だけ呼び出されます
 */
EXPORT ER knl_task_initialize( void )
{
	INT	i;      /* ループカウンタ */
	TCB	*tcb;   /* タスク制御ブロックポインタ */
	ID	tskid;  /* タスクID */

	/* システム情報の取得 */
	if ( NUM_TSKID < 1 ) {  /* タスク数チェック */
		return E_SYS;  /* システムエラー */
	}

	/* タスク実行制御情報の初期化 */
	knl_ctxtsk = knl_schedtsk = NULL;  /* 実行中/次実行タスクをNULLに */
	knl_ready_queue_initialize(&knl_ready_queue);  /* 実行可能キュー初期化 */
	knl_dispatch_disabled = DDS_ENABLE;  /* ディスパッチ許可状態に設定 */

	/* 全TCBを未使用キューに登録 */
	QueInit(&knl_free_tcb);  /* 未使用キュー初期化 */
	for ( tcb = knl_tcb_table, i = 0; i < NUM_TSKID; tcb++, i++ ) {
		tskid = ID_TSK(i);  /* タスクID生成 */
		tcb->tskid = tskid;  /* TCBにID設定 */
		tcb->state = TS_NONEXIST;  /* 状態を未存在に設定 */
#if CFN_MAX_PORID > 0
		tcb->wrdvno = tskid;  /* 待ち合わせ番号設定 */
#endif

		QueInsert(&tcb->tskque, &knl_free_tcb);  /* 未使用キューに追加 */
	}

	return E_OK;  /* 正常終了 */
}
#endif /* USE_FUNC_TASK_INITIALIZE */

#ifdef USE_FUNC_MAKE_DORMANT
/**
 * @brief タスクを休止状態に設定
 * @param tcb 対象タスクの制御ブロック
 * 
 * タスクを休止状態（DORMANT）に設定し、実行開始に必要な
 * 初期化を行います。
 * 
 * 初期化される内容：
 * - タスク状態をTS_DORMANTに設定
 * - 優先度を初期優先度に復元
 * - システムモードを初期値に復元
 * - ウェイクアップカウント、サスペンドカウントをクリア
 * - ロック関連フラグのクリア
 * - デバッグ情報のクリア
 * - ミューテックスリストのクリア
 * - タスクコンテキストの設定
 * 
 * @note このタスクは実行可能状態になる前に呼び出される必要があります
 */
EXPORT void knl_make_dormant( TCB *tcb )
{
	/* 休止状態でリセットすべき変数の初期化 */
	tcb->state	= TS_DORMANT;  /* 休止状態に設定 */
	tcb->priority	= tcb->bpriority = tcb->ipriority;  /* 優先度を初期値に */
	tcb->sysmode	= tcb->isysmode;  /* システムモードを初期値に */
	tcb->wupcnt	= 0;  /* ウェイクアップカウンタクリア */
	tcb->suscnt	= 0;  /* サスペンドカウンタクリア */

	tcb->klockwait	= FALSE;  /* カーネルロック待ちフラグOFF */
	tcb->klocked	= FALSE;  /* カーネルロック状態フラグOFF */

#if USE_DBGSPT && defined(USE_FUNC_TD_INF_TSK)
	tcb->stime	= 0;  /* システム時間リセット */
	tcb->utime	= 0;  /* ユーザ時間リセット */
#endif

	tcb->wercd = NULL;  /* 待ち要因コードクリア */

#if CFN_MAX_MTXID > 0
	tcb->mtxlist	= NULL;  /* ミューテックスリストクリア */
#endif

	/* タスク開始用コンテキストの設定 */
	knl_setup_context(tcb);  /* CPU依存処理 */
}
#endif /* USE_FUNC_MAKE_DORMANT */

/* ------------------------------------------------------------------------ */

#ifdef USE_FUNC_MAKE_READY
/**
 * @brief タスクを実行可能状態に設定
 * @param tcb 対象タスクの制御ブロック
 * 
 * タスク状態をREADYに更新し、実行可能キューに挿入します。
 * 必要に応じて'knl_schedtsk'を更新し、タスクディスパッチャを
 * 起動要求します。
 * 
 * 処理内容：
 * - タスク状態をTS_READYに設定
 * - 実行可能キューへの挿入
 * - より高い優先度の場合、次実行タスクとして設定
 * - ディスパッチ要求の発行
 * 
 * @note この関数により、タスクは実行可能になります
 */
EXPORT void knl_make_ready( TCB *tcb )
{
	tcb->state = TS_READY;
	if ( knl_ready_queue_insert(&knl_ready_queue, tcb) ) {
		knl_schedtsk = tcb;
		knl_dispatch_request();
	}
}
#endif /* USE_FUNC_MAKE_READY */

#ifdef USE_FUNC_MAKE_NON_READY
/**
 * @brief タスクを実行不可能状態に設定
 * @param tcb 対象タスクの制御ブロック
 * 
 * タスクを実行可能キューから削除します。削除されたタスクが
 * 'knl_schedtsk'（次実行タスク）の場合は、実行可能キューの
 * 最高優先度タスクを新しい'knl_schedtsk'に設定します。
 * 
 * 処理内容：
 * - 実行可能キューからタスクを削除
 * - 次実行タスクだった場合の再スケジューリング
 * - ディスパッチ要求の発行
 * 
 * @note 対象タスクは事前にREADY状態である必要があります
 * @note この関数により、タスクは実行不可能になります
 */
EXPORT void knl_make_non_ready( TCB *tcb )
{
	knl_ready_queue_delete(&knl_ready_queue, tcb);
	if ( knl_schedtsk == tcb ) {
		knl_schedtsk = knl_ready_queue_top(&knl_ready_queue);
		knl_dispatch_request();
	}
}
#endif /* USE_FUNC_MAKE_NON_READY */

#ifdef USE_FUNC_CHANGE_TASK_PRIORITY
/**
 * @brief タスク優先度の動的変更
 * @param tcb 対象タスクの制御ブロック
 * @param priority 新しい優先度
 * 
 * 実行時にタスクの優先度を変更します。READY状態のタスクの場合は
 * 実行可能キューからの削除・再挿入により適切な位置に配置します。
 * 
 * 処理内容：
 * - READY状態の場合：キューからの削除→優先度変更→再挿入
 * - 非READY状態の場合：優先度のみ変更
 * - 待ち状態のタスクの場合：優先度変更フックの実行
 * - 必要に応じて再スケジューリング
 * 
 * @note READY状態のタスクは一時的にキューから削除されます
 * @note 優先度変更により、タスクの実行順序が変わる可能性があります
 */
EXPORT void knl_change_task_priority( TCB *tcb, INT priority )
{
	INT	oldpri;

	if ( tcb->state == TS_READY ) {
		/*
		 * When deleting a task from the ready queue, 
		 * TCBの'priority'フィールドの値が必要となるため、
		 * 'tcb->priority'を変更する前にタスクを
		 * 実行可能キューから削除する必要があります。
		 */
		knl_ready_queue_delete(&knl_ready_queue, tcb);
		tcb->priority = (UB)priority;
		knl_ready_queue_insert(&knl_ready_queue, tcb);
		knl_reschedule();
	} else {
		oldpri = tcb->priority;
		tcb->priority = (UB)priority;

		/* タスク優先度変更時のフックルーチンが定義されている場合、
		    それを実行する */
		if ( (tcb->state & TS_WAIT) != 0 && tcb->wspec->chg_pri_hook) {
			(*tcb->wspec->chg_pri_hook)(tcb, oldpri);
		}
	}
}
#endif /* USE_FUNC_CHANGE_TASK_PRIORITY */

#ifdef USE_FUNC_ROTATE_READY_QUEUE
/**
 * @brief 実行可能キューのローテーション
 * @param priority ローテーション対象の優先度
 * 
 * 指定された優先度の実行可能キューをローテーションし、
 * 同一優先度内でのラウンドロビンスケジューリングを実現します。
 * 
 * 処理内容：
 * - 指定優先度のキューの先頭タスクを末尾に移動
 * - 再スケジューリングの実行
 * 
 * @note タイムスライスによるラウンドロビン実装に使用されます
 * @note 同一優先度のタスク間で公平な実行時間配分を実現します
 */
EXPORT void knl_rotate_ready_queue( INT priority )
{
	knl_ready_queue_rotate(&knl_ready_queue, priority);
	knl_reschedule();
}
#endif /* USE_FUNC_ROTATE_READY_QUEUE */

#ifdef USE_FUNC_ROTATE_READY_QUEUE_RUN
/**
 * @brief 最高優先度タスクを含む実行可能キューのローテーション
 * 
 * 現在の最高優先度タスクがある場合に、その優先度の
 * 実行可能キューをローテーションします。
 * 
 * 処理内容：
 * - 実行予定タスクの存在確認
 * - 最高優先度の取得
 * - 該当優先度キューのローテーション
 * - 再スケジューリングの実行
 * 
 * @note この関数はtk_rot_rdqシステムコールの実装で使用されます
 * @note 同一優先度レベルのタスク間で実行権を切り替える(ラウンドロビン)機能を提供します
 */
EXPORT void knl_rotate_ready_queue_run( void )
{
	if ( knl_schedtsk != NULL ) {  /* 実行予定タスクが存在する場合 */
		knl_ready_queue_rotate(&knl_ready_queue,
				knl_ready_queue_top_priority(&knl_ready_queue));  /* 最高優先度キューのローテーション */
		knl_reschedule();  /* 再スケジューリング */
	}
}
#endif /* USE_FUNC_ROTATE_READY_QUEUE_RUN */

/* ------------------------------------------------------------------------ */
/*
 *	Debug support function
 */
#if USE_DBGSPT

#ifdef USE_FUNC_TD_RDY_QUE
/**
 * @brief 実行可能キューの参照（デバッグサポート関数）
 * @param pri 参照する優先度
 * @param list タスクIDを格納する配列
 * @param nent 配列の要素数
 * @return 指定優先度の実行可能タスク数
 * 
 * 指定された優先度の実行可能キューに登録されているタスクの
 * IDリストを取得します。
 * 
 * 処理内容：
 * - 優先度の妥当性チェック
 * - 割り込み禁止での安全なキュー参照
 * - タスクIDの配列への格納
 * - 実際のタスク数の返却
 *
 * @note この関数は主にデバッガやシステム監視ツールで使用されます
 * @note 割り込み禁止状態で実行されるため、リアルタイム性能に影響を与える可能性があります
 */
SYSCALL INT td_rdy_que_impl( PRI pri, ID list[], INT nent )
{
	QUEUE	*q, *tskque;  /* キュー走査用ポインタ */
	INT	n = 0;      /* カウンタ */

	CHECK_PRI(pri);  /* 優先度の妥当性チェック */

	BEGIN_DISABLE_INTERRUPT;  /* 割り込み禁止開始 */
	tskque = &knl_ready_queue.tskque[int_priority(pri)];  /* 対象優先度のキュー取得 */
	for ( q = tskque->next; q != tskque; q = q->next ) {  /* キュー走査 */
		if ( n++ < nent ) {  /* 配列サイズチェック */
			*(list++) = ((TCB*)q)->tskid;  /* タスクIDを配列に格納 */
		}
	}
	END_DISABLE_INTERRUPT;  /* 割り込み禁止終了 */

	return n;  /* 検出したタスク数を返却 */
}
#endif /* USE_FUNC_TD_RDY_QUE */

#endif /* USE_DBGSPT */
