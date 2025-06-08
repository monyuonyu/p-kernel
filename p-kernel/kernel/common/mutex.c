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
 * @file mutex.c
 * @brief ミューテックス管理機能
 * 
 * T-Kernelのミューテックス（Mutex：相互排他オブジェクト）の実装を提供する。
 * ミューテックスは共有リソースへの排他制御を行うための同期オブジェクトであり、
 * 優先度継承や優先度上限プロトコルによるデッドロック回避機能を持つ。
 * 
 * 主な機能：
 * - ミューテックスの作成・削除（tk_cre_mtx, tk_del_mtx）
 * - ミューテックスのロック・アンロック（tk_loc_mtx, tk_unl_mtx）
 * - ミューテックスの状態参照（tk_ref_mtx）
 * - 優先度継承プロトコル（TA_INHERIT）
 * - 優先度上限プロトコル（TA_CEILING）
 * - 多重ロックの検出と防止
 * 
 * 優先度継承・上限プロトコルにより優先度逆転問題を解決し、
 * リアルタイム性を保証する。
 */

/** [BEGIN Common Definitions] */
#include "kernel.h"
#include "task.h"
#include "wait.h"
#include "check.h"
#include "mutex.h"
/** [END Common Definitions] */

#if CFN_MAX_MTXID > 0

#ifdef USE_FUNC_MTXCB_TABLE
Noinit(EXPORT MTXCB	knl_mtxcb_table[NUM_MTXID]);	/* Mutex control block */
Noinit(EXPORT QUEUE	knl_free_mtxcb);	/* FreeQue */
#endif /* USE_FUNC_MTXCB_TABLE */


#ifdef USE_FUNC_MUTEX_INITIALIZE
/**
 * @brief ミューテックス制御ブロック初期化
 * 
 * システム起動時にミューテックス制御ブロックテーブルを初期化する。
 * 全ての制御ブロックをフリーキューに登録し、使用可能な状態にする。
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_SYS システムエラー（ミューテックスID数が無効）
 * 
 * @note システム初期化時に一度だけ呼び出される
 * @note 割り込み禁止状態で呼び出される
 */
EXPORT ER knl_mutex_initialize(void)
{
	MTXCB	*mtxcb, *end;

	/* Get system information */
	if ( NUM_MTXID < 1 ) {
		return E_SYS;
	}

	/* Register all control blocks onto FreeQue */
	QueInit(&knl_free_mtxcb);
	end = knl_mtxcb_table + NUM_MTXID;
	for( mtxcb = knl_mtxcb_table; mtxcb < end; mtxcb++ ) {
		mtxcb->mtxid = 0;
		QueInsert(&mtxcb->wait_queue, &knl_free_mtxcb);
	}

	return E_OK;
}
#endif /* USE_FUNC_MUTEX_INITIALIZE */


#ifdef USE_FUNC_RELEASE_MUTEX
/**
 * @brief ミューテックス解放とタスク優先度調整
 * 
 * 指定されたミューテックスをタスクの保持リストから削除し、
 * タスクの優先度を適切に調整する。以下の優先度のうち最高値を設定する：
 * (A) タスクが保持する全ミューテックスにおける最高優先度
 * (B) タスクのベース優先度
 * 
 * @param tcb 対象タスクの制御ブロック
 * @param relmtxcb 解放するミューテックスの制御ブロック
 * 
 * @note 優先度継承・上限プロトコルに基づいて優先度を再計算する
 * @note TA_CEILING: 上限優先度を考慮
 * @note TA_INHERIT: 待ちタスクの最高優先度を考慮
 */
EXPORT void knl_release_mutex( TCB *tcb, MTXCB *relmtxcb )
{
	MTXCB	*mtxcb, **prev;
	INT	newpri, pri;

	/* (B) The base priority of task */
	newpri = tcb->bpriority;

	/* (A) The highest priority in mutex which is locked */
	pri = newpri;
	prev = &tcb->mtxlist;
	while ( (mtxcb = *prev) != NULL ) {
		if ( mtxcb == relmtxcb ) {
			/* Delete from list */
			*prev = mtxcb->mtxlist;
			continue;
		}

		switch ( mtxcb->mtxatr & TA_CEILING ) {
		  case TA_CEILING:
			pri = mtxcb->ceilpri;
			break;
		  case TA_INHERIT:
			if ( mtx_waited(mtxcb) ) {
				pri = mtx_head_pri(mtxcb);
			}
			break;
		  default: /* TA_TFIFO, TA_TPRI */
			/* nothing to do */
			break;
		}
		if ( newpri > pri ) {
			newpri = pri;
		}

		prev = &mtxcb->mtxlist;
	}

	if ( newpri != tcb->priority ) {
		/* Change priority of lock get task */
		knl_change_task_priority(tcb, newpri);
	}
}
#endif /* USE_FUNC_RELEASE_MUTEX */

#ifdef USE_FUNC_SIGNAL_ALL_MUTEX
/**
 * @brief タスク終了時の全ミューテックス解放
 * 
 * タスクが終了する際に、そのタスクが保持している全てのミューテックスを解放する。
 * 各ミューテックスで待機中のタスクがあれば起床させ、ミューテックスの所有権を移譲する。
 * 
 * @param tcb 終了するタスクの制御ブロック
 * 
 * @note 終了タスクのミューテックスリストや優先度の処理は不要
 * @note TA_CEILINGミューテックスの場合は新しい所有タスクの優先度を上限値まで上げる
 * @note 待ちタスクがない場合はミューテックスを未ロック状態にする
 */
EXPORT void knl_signal_all_mutex( TCB *tcb )
{
	MTXCB	*mtxcb, *next_mtxcb;
	TCB	*next_tcb;

	next_mtxcb = tcb->mtxlist;
	while ( (mtxcb = next_mtxcb) != NULL ) {
		next_mtxcb = mtxcb->mtxlist;

		if ( mtx_waited(mtxcb) ) {
			next_tcb = (TCB*)mtxcb->wait_queue.next;

			/* Wake wait task */
			knl_wait_release_ok(next_tcb);

			/* Change mutex get task */
			mtxcb->mtxtsk = next_tcb;
			mtxcb->mtxlist = next_tcb->mtxlist;
			next_tcb->mtxlist = mtxcb;

			if ( (mtxcb->mtxatr & TA_CEILING) == TA_CEILING ) {
				if ( next_tcb->priority > mtxcb->ceilpri ) {
					/* Raise the priority for the task
					   that got lock to the highest
					   priority limit */
					knl_change_task_priority(next_tcb,
							mtxcb->ceilpri);
				}
			}
		} else {
			/* No wait task */
			mtxcb->mtxtsk = NULL;
		}
	}
}
#endif /* USE_FUNC_SIGNAL_ALL_MUTEX */

#ifdef USE_FUNC_CHG_PRI_MUTEX
/**
 * @brief ミューテックスによるタスク優先度変更制限
 * 
 * タスクの優先度変更時に、ミューテックスの制約による制限を適用する。
 * 
 * 制限ルール：
 * 1. タスクがミューテックスをロックしている場合、保持する全ミューテックスの
 *    最高優先度より低い優先度には設定できない
 * 2. TA_CEILING属性のミューテックスをロック中または待ち中の場合、
 *    その上限優先度より高い優先度には設定できない（E_ILUSE）
 * 3. 上記以外の場合は指定された優先度をそのまま返す
 * 
 * @param tcb 対象タスクの制御ブロック
 * @param priority 設定しようとする優先度
 * 
 * @return INT 実際に設定可能な優先度またはエラーコード
 * @retval E_ILUSE 優先度制限により設定不可
 * @retval その他 設定可能な優先度値
 */
EXPORT INT knl_chg_pri_mutex( TCB *tcb, INT priority )
{
	MTXCB	*mtxcb;
	INT	hi_pri, low_pri, pri;

	hi_pri  = priority;
	low_pri = int_priority(MIN_PRI);

	/* Mutex lock wait */
	if ( (tcb->state & TS_WAIT) != 0 && (tcb->wspec->tskwait & TTW_MTX) != 0 ) {
		mtxcb = get_mtxcb(tcb->wid);
		if ( (mtxcb->mtxatr & TA_CEILING) == TA_CEILING ) {
			pri = mtxcb->ceilpri;
			if ( pri > low_pri ) {
				low_pri = pri;
			}
		}
	}

	/* Locked Mutex */
	pri = hi_pri;
	for ( mtxcb = tcb->mtxlist; mtxcb != NULL; mtxcb = mtxcb->mtxlist ) {
		switch ( mtxcb->mtxatr & TA_CEILING ) {
		  case TA_CEILING:
			pri = mtxcb->ceilpri;
			if ( pri > low_pri ) {
				low_pri = pri;
			}
			break;
		  case TA_INHERIT:
			if ( mtx_waited(mtxcb) ) {
				pri = mtx_head_pri(mtxcb);
			}
			break;
		  default: /* TA_TFIFO, TA_TPRI */
			/* nothing to do */
			break;
		}
		if ( pri < hi_pri ) {
			hi_pri = pri;
		}
	}

	if ( priority < low_pri ) {
		return E_ILUSE;
	}
	return hi_pri;
}
#endif /* USE_FUNC_CHG_PRI_MUTEX */


#ifdef USE_FUNC_TK_CRE_MTX
/**
 * @brief ミューテックス生成
 * 
 * 指定されたパラメータに基づいてミューテックスを生成する。
 * 優先度継承または優先度上限プロトコルを選択できる。
 * 
 * @param pk_cmtx ミューテックス生成情報パケットへのポインタ
 * 
 * @return ID 生成されたミューテックスID
 * @retval 正の値 生成されたミューテックスのID
 * @retval E_RSATR 予約属性エラー
 * @retval E_PAR パラメータエラー（上限優先度が不正等）
 * @retval E_LIMIT ミューテックス数の上限超過
 * 
 * @note TA_CEILING指定時は上限優先度（ceilpri）の指定が必要
 * @note 生成時点ではどのタスクにもロックされていない状態
 */
SYSCALL ID tk_cre_mtx_impl( CONST T_CMTX *pk_cmtx )
{
#if CHK_RSATR
	const ATR VALID_MTXATR = {
		 TA_CEILING
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
	};
#endif
	MTXCB	*mtxcb;
	ID	mtxid;
	INT	ceilpri;
	ER	ercd;

	CHECK_RSATR(pk_cmtx->mtxatr, VALID_MTXATR);

	if ( (pk_cmtx->mtxatr & TA_CEILING) == TA_CEILING ) {
		CHECK_PRI(pk_cmtx->ceilpri);
		ceilpri = int_priority(pk_cmtx->ceilpri);
	} else {
		ceilpri = 0;
	}

	BEGIN_CRITICAL_SECTION;
	/* Get control block from FreeQue */
	mtxcb = (MTXCB*)QueRemoveNext(&knl_free_mtxcb);
	if ( mtxcb == NULL ) {
		ercd = E_LIMIT;
	} else {
		mtxid = ID_MTX(mtxcb - knl_mtxcb_table);

		/* Initialize control block */
		QueInit(&mtxcb->wait_queue);
		mtxcb->mtxid   = mtxid;
		mtxcb->exinf   = pk_cmtx->exinf;
		mtxcb->mtxatr  = pk_cmtx->mtxatr;
		mtxcb->ceilpri = ceilpri;
		mtxcb->mtxtsk  = NULL;
		mtxcb->mtxlist = NULL;
#if USE_OBJECT_NAME
		if ( (pk_cmtx->mtxatr & TA_DSNAME) != 0 ) {
			strncpy((char*)mtxcb->name, (char*)pk_cmtx->dsname,
				(UINT)OBJECT_NAME_LENGTH);
		}
#endif
		ercd = mtxid;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_CRE_MTX */

#ifdef USE_FUNC_TK_DEL_MTX
/**
 * @brief ミューテックス削除
 * 
 * 指定されたミューテックスを削除し、関連リソースを解放する。
 * ミューテックスを保持するタスクがある場合は適切に処理し、
 * 待機中のタスクがある場合は全て起床させる（E_DLTエラーで）。
 * 
 * @param mtxid 削除するミューテックスのID
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正ID
 * @retval E_NOEXS オブジェクト未生成
 * 
 * @note 保持タスクがある場合はリストから削除し、優先度を調整する
 * @note 削除後、該当IDのミューテックスは使用できなくなる
 */
SYSCALL ER tk_del_mtx_impl( ID mtxid )
{
	MTXCB	*mtxcb;
	ER	ercd = E_OK;

	CHECK_MTXID(mtxid);

	mtxcb = get_mtxcb(mtxid);

	BEGIN_CRITICAL_SECTION;
	if ( mtxcb->mtxid == 0 ) {
		ercd = E_NOEXS;
	} else {
		/* If there is a task that holds mutex to delete,
		 * delete the mutex from the list
		 * and adjust the task priority if necessary.
		 */
		if ( mtxcb->mtxtsk != NULL ) {
			knl_release_mutex(mtxcb->mtxtsk, mtxcb);
		}

		/* Free wait state of task (E_DLT) */
		knl_wait_delete(&mtxcb->wait_queue);

		/* Return to FreeQue */
		QueInsert(&mtxcb->wait_queue, &knl_free_mtxcb);
		mtxcb->mtxid = 0;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_MTX */


#ifdef USE_FUNC_TK_LOC_MTX
/**
 * @brief ミューテックス待ちタスクの優先度変更時処理
 * 
 * ミューテックスで待機中のタスクの優先度が変更された場合の処理を行う。
 * TA_INHERIT属性の場合は、ミューテックス保持タスクの優先度も調整する。
 * 
 * @param tcb 優先度が変更されたタスクの制御ブロック
 * @param oldpri 変更前の優先度
 * 
 * @note TA_INHERIT時の優先度継承処理：
 *       - 待ちタスクの優先度が上がった場合、保持タスクの優先度も上げる
 *       - 待ちタスクの優先度が下がった場合、保持タスクの優先度を再計算する
 */
LOCAL void mtx_chg_pri( TCB *tcb, INT oldpri )
{
	MTXCB	*mtxcb;
	TCB	*mtxtsk;

	mtxcb = get_mtxcb(tcb->wid);
	knl_gcb_change_priority((GCB*)mtxcb, tcb);

	if ( (mtxcb->mtxatr & TA_CEILING) == TA_INHERIT ) {
		mtxtsk = mtxcb->mtxtsk;
		if ( mtxtsk->priority > tcb->priority ) {
			/* Since the highest priority of the lock wait task
			   became higher, raise the lock get task priority
			   higher */
			knl_change_task_priority(mtxtsk, tcb->priority);

		} else if ( mtxtsk->priority == oldpri ) {
			/* Since the highest priority of the lock wait task
			   might become lower, adjust this priority */
			reset_priority(mtxtsk);
		}
	}
}

/**
 * @brief ミューテックス待ちタスク解放時処理（TA_INHERIT専用）
 * 
 * TA_INHERIT属性のミューテックスで待機中のタスクが解放される際の処理を行う。
 * 保持タスクの優先度を再計算する必要があるかをチェックする。
 * 
 * @param tcb 解放されるタスクの制御ブロック
 * 
 * @note 解放されるタスクが最高優先度だった場合、保持タスクの優先度を再計算
 */
LOCAL void mtx_rel_wai( TCB *tcb )
{
	MTXCB	*mtxcb;
	TCB	*mtxtsk;

	mtxcb = get_mtxcb(tcb->wid);
	mtxtsk = mtxcb->mtxtsk;

	if ( mtxtsk->priority == tcb->priority ) {
		/* Since the highest priority of the lock wait task might 
		   become lower, adjust this priority */
		reset_priority(mtxtsk);
	}
}

/*
 * Definition of mutex wait specification
 */
LOCAL CONST WSPEC knl_wspec_mtx_tfifo   = { TTW_MTX, NULL, NULL };
LOCAL CONST WSPEC knl_wspec_mtx_tpri    = { TTW_MTX, mtx_chg_pri, NULL };
LOCAL CONST WSPEC knl_wspec_mtx_inherit = { TTW_MTX, mtx_chg_pri, mtx_rel_wai };

/**
 * @brief ミューテックスロック
 * 
 * 指定されたミューテックスをロックする。既にロックされている場合は
 * 指定されたタイムアウト時間まで待機する。優先度プロトコルに応じて
 * 適切な優先度制御を行う。
 * 
 * @param mtxid ミューテックスID
 * @param tmout タイムアウト時間（TMO_POL:ポーリング、TMO_FEVR:永久待ち）
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了（ロック取得成功）
 * @retval E_ID 不正ID
 * @retval E_NOEXS オブジェクト未生成
 * @retval E_ILUSE 不正使用（多重ロックまたは優先度制限違反）
 * @retval E_TMOUT タイムアウト発生
 * @retval E_RLWAI 待ち状態の強制解除
 * 
 * @note 多重ロック（同一タスクによる重複ロック）は禁止
 * @note TA_CEILING: 上限優先度制限をチェック、ロック取得時に優先度上昇
 * @note TA_INHERIT: 待ち発生時に保持タスクの優先度を継承
 */
SYSCALL ER tk_loc_mtx_impl( ID mtxid, TMO tmout )
{
	MTXCB	*mtxcb;
	TCB	*mtxtsk;
	ATR	mtxatr;
	ER	ercd = E_OK;

	CHECK_MTXID(mtxid);
	CHECK_TMOUT(tmout);
	CHECK_DISPATCH();

	mtxcb = get_mtxcb(mtxid);

	BEGIN_CRITICAL_SECTION;
	if ( mtxcb->mtxid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( mtxcb->mtxtsk == knl_ctxtsk ) {
		ercd = E_ILUSE;  /* Multiplexed lock */
		goto error_exit;
	}

	mtxatr = mtxcb->mtxatr & TA_CEILING;
	if ( mtxatr == TA_CEILING ) {
		if ( knl_ctxtsk->bpriority < mtxcb->ceilpri ) {
			/* Violation of highest priority limit */
			ercd = E_ILUSE;
			goto error_exit;
		}
	}

	mtxtsk = mtxcb->mtxtsk;
	if ( mtxtsk == NULL ) {
		/* Get lock */
		mtxcb->mtxtsk = knl_ctxtsk;
		mtxcb->mtxlist = knl_ctxtsk->mtxlist;
		knl_ctxtsk->mtxlist = mtxcb;

		if ( mtxatr == TA_CEILING ) {
			if ( knl_ctxtsk->priority > mtxcb->ceilpri ) {
				/* Raise its own task to the highest
				   priority limit */
				knl_change_task_priority(knl_ctxtsk, mtxcb->ceilpri);
			}
		}
	} else {
		ercd = E_TMOUT;
		if ( tmout == TMO_POL ) {
			goto error_exit;
		}

		if ( mtxatr == TA_INHERIT ) {
			if ( mtxtsk->priority > knl_ctxtsk->priority ) {
				/* Raise the priority of task during
				   locking to the same priority as its
				   own task */
				knl_change_task_priority(mtxtsk, knl_ctxtsk->priority);
			}
		}

		/* Ready for wait */
		knl_ctxtsk->wspec = ( mtxatr == TA_TFIFO   )? &knl_wspec_mtx_tfifo:
				( mtxatr == TA_INHERIT )? &knl_wspec_mtx_inherit:
							  &knl_wspec_mtx_tpri;
		knl_ctxtsk->wercd = &ercd;
		knl_ctxtsk->wid = mtxcb->mtxid;
		knl_make_wait(tmout, mtxcb->mtxatr);
		if ( mtxatr == TA_TFIFO ) {
			QueInsert(&knl_ctxtsk->tskque, &mtxcb->wait_queue);
		} else {
			knl_queue_insert_tpri(knl_ctxtsk, &mtxcb->wait_queue);
		}
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_LOC_MTX */

#ifdef USE_FUNC_TK_UNL_MTX
/**
 * @brief ミューテックスアンロック
 * 
 * 指定されたミューテックスをアンロックする。待機中のタスクがあれば
 * 起床させて新しい所有者とし、適切な優先度制御を行う。
 * 
 * @param mtxid ミューテックスID
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正ID
 * @retval E_NOEXS オブジェクト未生成
 * @retval E_ILUSE 不正使用（他タスクがロックしたミューテックス）
 * 
 * @note 自分がロックしていないミューテックスのアンロックは禁止
 * @note アンロック時に自タスクの優先度を適切に調整
 * @note 待ちタスクがある場合は次のタスクに所有権を移譲
 * @note TA_CEILING: 新所有者の優先度を上限値まで上昇させる
 */
SYSCALL ER tk_unl_mtx_impl( ID mtxid )
{
	MTXCB	*mtxcb;	
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_MTXID(mtxid);
	CHECK_INTSK();

	mtxcb = get_mtxcb(mtxid);

	BEGIN_CRITICAL_SECTION;
	if ( mtxcb->mtxid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( mtxcb->mtxtsk != knl_ctxtsk ) {
		ercd = E_ILUSE;  /* This is not locked by its own task */
		goto error_exit;
	}

	/* Delete the mutex from the list,
	   and adjust its own task priority if necessary. */
	knl_release_mutex(knl_ctxtsk, mtxcb);

	if ( mtx_waited(mtxcb) ) {
		tcb = (TCB*)mtxcb->wait_queue.next;

		/* Release wait */
		knl_wait_release_ok(tcb);

		/* Change mutex get task */
		mtxcb->mtxtsk = tcb;
		mtxcb->mtxlist = tcb->mtxlist;
		tcb->mtxlist = mtxcb;

		if ( (mtxcb->mtxatr & TA_CEILING) == TA_CEILING ) {
			if ( tcb->priority > mtxcb->ceilpri ) {
				/* Raise the priority of the task that
				   got lock to the highest priority limit */
				knl_change_task_priority(tcb, mtxcb->ceilpri);
			}
		}
	} else {
		/* No wait task */
		mtxcb->mtxtsk = NULL;
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_UNL_MTX */


#ifdef USE_FUNC_TK_REF_MTX
/**
 * @brief ミューテックス状態参照
 * 
 * 指定されたミューテックスの現在の状態情報を取得する。
 * 保持タスク、待機タスクの情報等を提供する。
 * 
 * @param mtxid ミューテックスID
 * @param pk_rmtx ミューテックス状態情報を格納するパケットへのポインタ
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正ID
 * @retval E_NOEXS オブジェクト未生成
 * 
 * @note 返される情報には保持タスクID、待機タスクID等が含まれる
 * @note 状態参照時点でのスナップショット情報である
 */
SYSCALL ER tk_ref_mtx_impl( ID mtxid, T_RMTX *pk_rmtx )
{
	MTXCB	*mtxcb;
	ER	ercd = E_OK;

	CHECK_MTXID(mtxid);

	mtxcb = get_mtxcb(mtxid);

	BEGIN_CRITICAL_SECTION;
	if ( mtxcb->mtxid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rmtx->exinf = mtxcb->exinf;
		pk_rmtx->htsk = ( mtxcb->mtxtsk != NULL )?
					mtxcb->mtxtsk->tskid: 0;
		pk_rmtx->wtsk = knl_wait_tskid(&mtxcb->wait_queue);
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REF_MTX */

/* ------------------------------------------------------------------------ */
/*
 *	Debugger support function
 */
#if USE_DBGSPT

#ifdef USE_FUNC_MUTEX_GETNAME
#if USE_OBJECT_NAME
/*
 * Get object name from control block
 */
EXPORT ER knl_mutex_getname(ID id, UB **name)
{
	MTXCB	*mtxcb;
	ER	ercd = E_OK;

	CHECK_MTXID(id);

	BEGIN_DISABLE_INTERRUPT;
	mtxcb = get_mtxcb(id);
	if ( mtxcb->mtxid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (mtxcb->mtxatr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = mtxcb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */
#endif /* USE_FUNC_MUTEX_GETNAME */

#ifdef USE_FUNC_TD_LST_MTX
/*
 * Refer mutex usage state
 */
SYSCALL INT td_lst_mtx_impl( ID list[], INT nent )
{
	MTXCB	*mtxcb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_mtxcb_table + NUM_MTXID;
	for ( mtxcb = knl_mtxcb_table; mtxcb < end; mtxcb++ ) {
		if ( mtxcb->mtxid == 0 ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = mtxcb->mtxid;
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_MTX */

#ifdef USE_FUNC_TD_REF_MTX
/*
 * Refer mutex state
 */
SYSCALL ER td_ref_mtx_impl( ID mtxid, TD_RMTX *pk_rmtx )
{
	MTXCB	*mtxcb;
	ER	ercd = E_OK;

	CHECK_MTXID(mtxid);

	mtxcb = get_mtxcb(mtxid);

	BEGIN_DISABLE_INTERRUPT;
	if ( mtxcb->mtxid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rmtx->exinf = mtxcb->exinf;
		pk_rmtx->htsk = ( mtxcb->mtxtsk != NULL )?
					mtxcb->mtxtsk->tskid: 0;
		pk_rmtx->wtsk = knl_wait_tskid(&mtxcb->wait_queue);
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_MTX */

#ifdef USE_FUNC_TD_MTX_QUE
/*
 * Refer mutex wait queue
 */
SYSCALL INT td_mtx_que_impl( ID mtxid, ID list[], INT nent )
{
	MTXCB	*mtxcb;
	QUEUE	*q;
	ER	ercd = E_OK;

	CHECK_MTXID(mtxid);

	mtxcb = get_mtxcb(mtxid);

	BEGIN_DISABLE_INTERRUPT;
	if ( mtxcb->mtxid == 0 ) {
		ercd = E_NOEXS;
	} else {
		INT n = 0;
		for ( q = mtxcb->wait_queue.next; q != &mtxcb->wait_queue; q = q->next ) {
			if ( n++ < nent ) {
				*list++ = ((TCB*)q)->tskid;
			}
		}
		ercd = n;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_MTX_QUE */

#endif /* USE_DBGSPT */
#endif /* CFN_MAX_MTXID */
