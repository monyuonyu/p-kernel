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
 * @file	semaphore.c
 * @brief	セマフォ機能の実装
 *
 * セマフォの生成・削除、資源の返却（tk_sig_sem）・獲得（tk_wai_sem）、
 * 状態参照、およびデバッガサポート機能を提供します。
 */

#include "kernel.h"
#include "wait.h"
#include "check.h"
#include "semaphore.h"

#if USE_SEMAPHORE == 1

Noinit(EXPORT SEMCB knl_semcb_table[NUM_SEMID]);	/* セマフォ制御ブロックテーブル */
Noinit(EXPORT QUEUE knl_free_semcb);	/* 未使用制御ブロックのキュー（FreeQue） */


/**
 * @brief セマフォ制御ブロックの初期化
 *
 * カーネル起動時に呼ばれ、全セマフォ制御ブロックを未使用状態にして
 * FreeQue に登録します。
 *
 * @return E_OK: 正常終了 / E_SYS: NUM_SEMID が 1 未満
 */
EXPORT ER knl_semaphore_initialize( void )
{
	SEMCB	*semcb, *end;

	/* システム情報の確認 */
	if ( NUM_SEMID < 1 ) {
		return E_SYS;
	}

	/* すべての制御ブロックを FreeQue に登録 */
	QueInit(&knl_free_semcb);
	end = knl_semcb_table + NUM_SEMID;
	for ( semcb = knl_semcb_table; semcb < end; semcb++ ) {
		semcb->semid = 0;
		QueInsert(&semcb->wait_queue, &knl_free_semcb);
	}

	return E_OK;
}


/**
 * @brief セマフォの生成
 *
 * 生成情報 pk_csem に従ってセマフォを生成し、セマフォIDを割り当てます。
 * 初期資源数は isemcnt、最大資源数は maxsem に設定されます。
 *
 * @param pk_csem セマフォ生成情報へのポインタ
 * @return 正の値: 生成したセマフォID
 * @retval E_LIMIT 制御ブロックに空きがない（セマフォ数の上限超過）
 * @retval E_RSATR 不正な属性指定（CHK_RSATR 有効時）
 * @retval E_PAR   isemcnt・maxsem の値が不正
 */
SYSCALL ID tk_cre_sem( CONST T_CSEM *pk_csem )
{
#if CHK_RSATR
	const ATR VALID_SEMATR = {
		 TA_TPRI
		|TA_CNT
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
	};
#endif
	SEMCB	*semcb;
	ID	semid;
	ER	ercd;

	CHECK_RSATR(pk_csem->sematr, VALID_SEMATR);
	CHECK_PAR(pk_csem->isemcnt >= 0);
	CHECK_PAR(pk_csem->maxsem > 0);
	CHECK_PAR(pk_csem->maxsem >= pk_csem->isemcnt);

	BEGIN_CRITICAL_SECTION;
	/* FreeQue から制御ブロックを取得 */
	semcb = (SEMCB*)QueRemoveNext(&knl_free_semcb);
	if ( semcb == NULL ) {
		ercd = E_LIMIT;
	} else {
		semid = ID_SEM(semcb - knl_semcb_table);

		/* 制御ブロックの初期化 */
		QueInit(&semcb->wait_queue);
		semcb->semid = semid;
		semcb->exinf = pk_csem->exinf;
		semcb->sematr = pk_csem->sematr;
		semcb->semcnt = pk_csem->isemcnt;
		semcb->maxsem = pk_csem->maxsem;
#if USE_OBJECT_NAME
		if ( (pk_csem->sematr & TA_DSNAME) != 0 ) {
			knl_strncpy((char*)semcb->name, (char*)pk_csem->dsname,
				OBJECT_NAME_LENGTH);
		}
#endif
		ercd = semid;
	}
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_DEL_SEM
/**
 * @brief セマフォの削除
 *
 * 指定したセマフォを削除します。待ち状態のタスクがあれば
 * E_DLT を返して待ち解除し、制御ブロックを FreeQue に返却します。
 *
 * @param semid 削除するセマフォID
 * @retval E_OK    正常終了
 * @retval E_NOEXS 対象のセマフォが存在しない
 * @retval E_ID    semid が不正
 */
SYSCALL ER tk_del_sem( ID semid )
{
	SEMCB	*semcb;
	ER	ercd = E_OK;

	CHECK_SEMID(semid);

	semcb = get_semcb(semid);

	BEGIN_CRITICAL_SECTION;
	if ( semcb->semid == 0 ) {
		ercd = E_NOEXS;
	} else {
		/* 待ちタスクの待ち解除（E_DLT を返す） */
		knl_wait_delete(&semcb->wait_queue);

		/* FreeQue へ返却 */
		QueInsert(&semcb->wait_queue, &knl_free_semcb);
		semcb->semid = 0;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_SEM */

/**
 * @brief セマフォ資源の返却
 *
 * セマフォに cnt 個の資源を返却し、待ちキューの先頭から順に
 * 要求数を満たすタスクの待ちを解除します。TA_CNT 属性でない場合、
 * 先頭タスクの要求を満たせなくなった時点で解除処理を打ち切ります。
 *
 * @param semid 対象のセマフォID
 * @param cnt   返却する資源数（1 以上）
 * @retval E_OK    正常終了
 * @retval E_NOEXS 対象のセマフォが存在しない
 * @retval E_QOVR  返却により最大資源数 maxsem を超過
 * @retval E_ID    semid が不正
 * @retval E_PAR   cnt が 0 以下
 * @note タスク独立部（割込みハンドラ等）からも呼び出せます。
 */
SYSCALL ER tk_sig_sem( ID semid, INT cnt )
{
	SEMCB	*semcb;
	TCB	*tcb;
	QUEUE	*queue;
	ER	ercd = E_OK;

	CHECK_SEMID(semid);
	CHECK_PAR(cnt > 0);

	semcb = get_semcb(semid);

	BEGIN_CRITICAL_SECTION;
	if ( semcb->semid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( cnt > (semcb->maxsem - semcb->semcnt) ) {
		ercd = E_QOVR;
		goto error_exit;
	}

	/* セマフォ資源数の返却 */
	semcb->semcnt += cnt;

	/* 待ち解除できるタスクの探索 */
	queue = semcb->wait_queue.next;
	while ( queue != &semcb->wait_queue ) {
		tcb = (TCB*)queue;
		queue = queue->next;

		/* 待ち解除条件を満たすか？ */
		if ( semcb->semcnt < tcb->winfo.sem.cnt ) {
			if ( (semcb->sematr & TA_CNT) == 0 ) {
				break;
			}
			continue;
		}

		/* 待ち解除 */
		knl_wait_release_ok(tcb);

		semcb->semcnt -= tcb->winfo.sem.cnt;
		if ( semcb->semcnt <= 0 ) {
			break;
		}
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

/**
 * @brief 待ちタスクの優先度変更時の処理
 *
 * 待ちキューを優先度順に並べ替えたうえで、TA_CNT 属性でない場合は
 * 待ちキューの先頭タスクから順に、資源数の許す限り資源を割り当てて
 * 待ちを解除します。
 *
 * @param tcb    優先度が変更されたタスクの TCB
 * @param oldpri 変更前の優先度（負の値なら並べ替えを行わない）
 */
LOCAL void sem_chg_pri( TCB *tcb, INT oldpri )
{
	SEMCB	*semcb;
	QUEUE	*queue;
	TCB	*top;

	semcb = get_semcb(tcb->wid);
	if ( oldpri >= 0 ) {
		/* 待ちキューの並べ替え */
		knl_gcb_change_priority((GCB*)semcb, tcb);
	}

	if ( (semcb->sematr & TA_CNT) != 0 ) {
		return;
	}

	/* 待ちキューの先頭タスクから順に、可能な限り資源を割り当てて
	   待ち解除する */
	queue = semcb->wait_queue.next;
	while ( queue != &semcb->wait_queue ) {
		top = (TCB*)queue;
		queue = queue->next;

		/* 待ち解除条件を満たすか？ */
		if ( semcb->semcnt < top->winfo.sem.cnt ) {
			break;
		}

		/* 待ち解除 */
		knl_wait_release_ok(top);

		semcb->semcnt -= top->winfo.sem.cnt;
	}
}

/**
 * @brief 待ちタスクが待ち解除されたときの処理
 *
 * 残ったタスクへの資源再割り当てを行うため、並べ替えなしで
 * sem_chg_pri() を呼び出します。
 *
 * @param tcb 待ち解除されたタスクの TCB
 */
LOCAL void sem_rel_wai( TCB *tcb )
{
	sem_chg_pri(tcb, -1);
}

/*
 * セマフォ待ち仕様の定義
 */
LOCAL CONST WSPEC knl_wspec_sem_tfifo = { TTW_SEM, NULL,        sem_rel_wai };
LOCAL CONST WSPEC knl_wspec_sem_tpri  = { TTW_SEM, sem_chg_pri, sem_rel_wai };

/**
 * @brief セマフォ資源の獲得
 *
 * セマフォから cnt 個の資源を獲得します。獲得できない場合は
 * tmout で指定した時間だけ待ち状態になります。TA_CNT 属性でない場合は
 * 待ちキューの先頭タスクのみが資源を獲得できます。
 *
 * @param semid 対象のセマフォID
 * @param cnt   獲得する資源数（1 以上、maxsem 以下）
 * @param tmout タイムアウト時間（TMO_POL / TMO_FEVR 指定可）
 * @retval E_OK    正常終了（資源を獲得）
 * @retval E_NOEXS 対象のセマフォが存在しない
 * @retval E_TMOUT タイムアウトまたはポーリング失敗
 * @retval E_DLT   待ち中に対象セマフォが削除された
 * @retval E_RLWAI 待ち中に tk_rel_wai により強制解除された
 * @retval E_ID    semid が不正
 * @retval E_PAR   cnt・tmout が不正
 * @retval E_CTX   ディスパッチ禁止状態またはタスク独立部からの呼出し
 * @note 待ち状態になり得るため、タスク部からのみ呼び出せます。
 */
SYSCALL ER tk_wai_sem( ID semid, INT cnt, TMO tmout )
{
	SEMCB	*semcb;
	ER	ercd = E_OK;

	CHECK_SEMID(semid);
	CHECK_PAR(cnt > 0);
	CHECK_TMOUT(tmout);
	CHECK_DISPATCH();

	semcb = get_semcb(semid);

	BEGIN_CRITICAL_SECTION;
	if ( semcb->semid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
#if CHK_PAR
	if ( cnt > semcb->maxsem ) {
		ercd = E_PAR;
		goto error_exit;
	}
#endif

	if ( ((semcb->sematr & TA_CNT) != 0
	      || knl_gcb_top_of_wait_queue((GCB*)semcb, knl_ctxtsk) == knl_ctxtsk)
	  && semcb->semcnt >= cnt ) {
		/* セマフォ資源の獲得 */
		semcb->semcnt -= cnt;

	} else {
		/* 待ち状態へ移行する準備 */
		knl_ctxtsk->wspec = ( (semcb->sematr & TA_TPRI) != 0 )?
					&knl_wspec_sem_tpri: &knl_wspec_sem_tfifo;
		knl_ctxtsk->wercd = &ercd;
		knl_ctxtsk->winfo.sem.cnt = cnt;
		knl_gcb_make_wait((GCB*)semcb, tmout);
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_REF_SEM
/**
 * @brief セマフォ状態の参照
 *
 * セマフォの拡張情報・待ちタスクの有無・現在の資源数を取得します。
 *
 * @param semid   対象のセマフォID
 * @param pk_rsem 状態を格納する領域へのポインタ
 * @retval E_OK    正常終了
 * @retval E_NOEXS 対象のセマフォが存在しない
 * @retval E_ID    semid が不正
 */
SYSCALL ER tk_ref_sem( ID semid, T_RSEM *pk_rsem )
{
	SEMCB	*semcb;
	ER	ercd = E_OK;

	CHECK_SEMID(semid);

	semcb = get_semcb(semid);

	BEGIN_CRITICAL_SECTION;
	if ( semcb->semid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rsem->exinf  = semcb->exinf;
		pk_rsem->wtsk   = knl_wait_tskid(&semcb->wait_queue);
		pk_rsem->semcnt = semcb->semcnt;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REF_SEM */

/* ------------------------------------------------------------------------ */
/*
 *	デバッガサポート機能
 */
#if USE_DBGSPT

#if USE_OBJECT_NAME
/**
 * @brief 制御ブロックからのオブジェクト名取得
 *
 * TA_DSNAME 属性で生成されたセマフォの名前へのポインタを返します。
 *
 * @param id   対象のセマフォID
 * @param name 名前へのポインタを格納する領域
 * @retval E_OK    正常終了
 * @retval E_NOEXS 対象のセマフォが存在しない
 * @retval E_OBJ   TA_DSNAME 属性が指定されていない
 * @retval E_ID    id が不正
 */
EXPORT ER knl_semaphore_getname(ID id, UB **name)
{
	SEMCB	*semcb;
	ER	ercd = E_OK;

	CHECK_SEMID(id);

	BEGIN_DISABLE_INTERRUPT;
	semcb = get_semcb(id);
	if ( semcb->semid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (semcb->sematr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = semcb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */

#ifdef USE_FUNC_TD_LST_SEM
/**
 * @brief セマフォID一覧の参照
 *
 * 使用中のセマフォIDを list に最大 nent 個格納します。
 *
 * @param list ID を格納する配列
 * @param nent 配列の要素数
 * @return 使用中のセマフォ総数（nent を超える場合もそのまま返す）
 */
SYSCALL INT td_lst_sem( ID list[], INT nent )
{
	SEMCB	*semcb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_semcb_table + NUM_SEMID;
	for ( semcb = knl_semcb_table; semcb < end; semcb++ ) {
		if ( semcb->semid == 0 ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = semcb->semid;
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_SEM */

#ifdef USE_FUNC_TD_REF_SEM
/**
 * @brief セマフォ状態の参照（デバッガサポート）
 *
 * セマフォの拡張情報・待ちタスクの有無・現在の資源数を取得します。
 *
 * @param semid   対象のセマフォID
 * @param pk_rsem 状態を格納する領域へのポインタ
 * @retval E_OK    正常終了
 * @retval E_NOEXS 対象のセマフォが存在しない
 * @retval E_ID    semid が不正
 */
SYSCALL ER td_ref_sem( ID semid, TD_RSEM *pk_rsem )
{
	SEMCB	*semcb;
	ER	ercd = E_OK;

	CHECK_SEMID(semid);

	semcb = get_semcb(semid);

	BEGIN_DISABLE_INTERRUPT;
	if ( semcb->semid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rsem->exinf  = semcb->exinf;
		pk_rsem->wtsk   = knl_wait_tskid(&semcb->wait_queue);
		pk_rsem->semcnt = semcb->semcnt;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_SEM */

#ifdef USE_FUNC_TD_SEM_QUE
/**
 * @brief セマフォ待ちキューの参照
 *
 * セマフォ待ちキューに並ぶタスクのIDを先頭から順に list に
 * 最大 nent 個格納します。
 *
 * @param semid 対象のセマフォID
 * @param list  タスクID を格納する配列
 * @param nent  配列の要素数
 * @return 正の値または 0: 待ちタスクの総数
 * @retval E_NOEXS 対象のセマフォが存在しない
 * @retval E_ID    semid が不正
 */
SYSCALL INT td_sem_que( ID semid, ID list[], INT nent )
{
	SEMCB	*semcb;
	QUEUE	*q;
	ER	ercd;

	CHECK_SEMID(semid);

	semcb = get_semcb(semid);

	BEGIN_DISABLE_INTERRUPT;
	if ( semcb->semid == 0 ) {
		ercd = E_NOEXS;
	} else {
		INT	n = 0;
		for ( q = semcb->wait_queue.next; q != &semcb->wait_queue; q = q->next ) {
			if ( n++ < nent ) {
				*list++ = ((TCB*)q)->tskid;
			}
		}
		ercd = n;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_SEM_QUE */

#endif /* USE_DBGSPT */
#endif /* USE_SEMAPHORE */
