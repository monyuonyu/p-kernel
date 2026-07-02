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
 * @file	mutex.c
 * @brief	ミューテックス機能の実装
 *
 * 優先度逆転を回避する排他制御機構であるミューテックス機能を提供します。
 * 優先度継承（TA_INHERIT）および優先度上限（TA_CEILING）プロトコルに
 * 対応し、生成・削除・ロック・アンロック・状態参照の各 API
 * （tk_cre_mtx / tk_del_mtx / tk_loc_mtx / tk_unl_mtx / tk_ref_mtx）、
 * タスク終了時の一括解放などのカーネル内部処理、および
 * デバッガサポート機能（td_lst_mtx / td_ref_mtx / td_mtx_que）を
 * 実装しています。
 */

#include "kernel.h"
#include "wait.h"
#include "check.h"
#include "mutex.h"

#if USE_MUTEX == 1

Noinit(EXPORT MTXCB	knl_mtxcb_table[NUM_MTXID]);	/* ミューテックス管理ブロックテーブル */
Noinit(EXPORT QUEUE	knl_free_mtxcb);	/* 未使用管理ブロックのキュー（FreeQue） */


/**
 * @brief ミューテックス管理ブロックの初期化
 *
 * すべてのミューテックス管理ブロックを未使用状態にし、
 * FreeQue に登録します。カーネル起動時に呼び出されます。
 *
 * @retval E_OK	正常終了
 * @retval E_SYS	ミューテックス数（NUM_MTXID）が 1 未満
 */
EXPORT ER knl_mutex_initialize(void)
{
	MTXCB	*mtxcb, *end;

	/* システム情報の確認 */
	if ( NUM_MTXID < 1 ) {
		return E_SYS;
	}

	/* 全管理ブロックを FreeQue に登録 */
	QueInit(&knl_free_mtxcb);
	end = knl_mtxcb_table + NUM_MTXID;
	for( mtxcb = knl_mtxcb_table; mtxcb < end; mtxcb++ ) {
		mtxcb->mtxid = 0;
		QueInsert(&mtxcb->wait_queue, &knl_free_mtxcb);
	}

	return E_OK;
}


/**
 * @brief ミューテックスのロック解放とタスク優先度の再調整
 *
 * relmtxcb をタスクのロック中ミューテックスリストから外し、
 * 以下のうち最も高い優先度をタスクに再設定します。
 *	(A) タスク 'tcb' がロック中の全ミューテックスが要求する最高優先度
 *	    （TA_CEILING は上限優先度、TA_INHERIT は先頭待ちタスクの優先度）
 *	(B) タスク 'tcb' のベース優先度
 *
 * @param tcb	ロックを解放するタスクの TCB
 * @param relmtxcb	解放するミューテックス（NULL なら優先度再計算のみ）
 */
EXPORT void knl_release_mutex( TCB *tcb, MTXCB *relmtxcb )
{
	MTXCB	*mtxcb, **prev;
	INT	newpri, pri;

	/* (B) タスクのベース優先度 */
	newpri = tcb->bpriority;

	/* (A) ロック中のミューテックスが要求する最高優先度 */
	pri = newpri;
	prev = &tcb->mtxlist;
	while ( (mtxcb = *prev) != NULL ) {
		if ( mtxcb == relmtxcb ) {
			/* リストから削除 */
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
			/* 何もしない */
			break;
		}
		if ( newpri > pri ) {
			newpri = pri;
		}

		prev = &mtxcb->mtxlist;
	}

	if ( newpri != tcb->priority ) {
		/* ロック中タスクの優先度を変更 */
		knl_change_task_priority(tcb, newpri);
	}
}

/**
 * @brief タスク終了時のミューテックス一括解放
 *
 * タスクが保持しているすべてのミューテックスを解放します。
 * 待ちタスクがあれば先頭のタスクの待ちを解除して新たなロック
 * 取得タスクとし、TA_CEILING 属性であれば必要に応じてそのタスクの
 * 優先度を上限優先度まで引き上げます。
 * 終了するタスク自身のミューテックスリストや優先度の更新は
 * 行う必要がないため行いません。
 *
 * @param tcb	終了するタスクの TCB
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

			/* 待ちタスクの待ちを解除 */
			knl_wait_release_ok(next_tcb);

			/* ロック取得タスクを変更 */
			mtxcb->mtxtsk = next_tcb;
			mtxcb->mtxlist = next_tcb->mtxlist;
			next_tcb->mtxlist = mtxcb;

			if ( (mtxcb->mtxatr & TA_CEILING) == TA_CEILING ) {
				if ( next_tcb->priority > mtxcb->ceilpri ) {
					/* ロックを取得したタスクの優先度を
					   上限優先度まで引き上げる */
					knl_change_task_priority(next_tcb,
							mtxcb->ceilpri);
				}
			}
		} else {
			/* 待ちタスクなし */
			mtxcb->mtxtsk = NULL;
		}
	}
}

/**
 * @brief タスク優先度変更時のミューテックスによる制限
 *
 *    1. タスク 'tcb' がミューテックスをロックしている場合、ロック中の
 *	全ミューテックスが要求する最高優先度より低い優先度には設定
 *	できません。この場合はその最高優先度を返します。
 *    2. TA_CEILING 属性のミューテックスをロック中またはロック待ち中の
 *	場合、それらの上限優先度のうち最も低いものより高い優先度には
 *	設定できません。この場合は E_ILUSE を返します。
 *    3. 上記以外の場合は指定された 'priority' を返します。
 *
 * @param tcb	優先度を変更するタスクの TCB
 * @param priority	設定しようとする優先度（内部表現）
 * @return 実際に設定すべき優先度、または E_ILUSE（上限優先度違反）
 */
EXPORT INT knl_chg_pri_mutex( TCB *tcb, INT priority )
{
	MTXCB	*mtxcb;
	INT	hi_pri, low_pri, pri;

	hi_pri  = priority;
	low_pri = int_priority(MIN_TSKPRI);

	/* ミューテックスのロック待ち */
	if ( (tcb->state & TS_WAIT) != 0 && (tcb->wspec->tskwait & TTW_MTX) != 0 ) {
		mtxcb = get_mtxcb(tcb->wid);
		if ( (mtxcb->mtxatr & TA_CEILING) == TA_CEILING ) {
			pri = mtxcb->ceilpri;
			if ( pri > low_pri ) {
				low_pri = pri;
			}
		}
	}

	/* ロック中のミューテックス */
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
			/* 何もしない */
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


/**
 * @brief ミューテックスの生成
 *
 * FreeQue から管理ブロックを取り出して初期化し、非ロック状態の
 * ミューテックスを生成します。TA_CEILING 属性の場合は上限優先度
 * （ceilpri）の妥当性を検査します。
 *
 * @param pk_cmtx	ミューテックス生成情報（属性・上限優先度など）
 * @return 正の値ならば生成したミューテックスの ID、負の値ならばエラーコード
 * @retval E_LIMIT	ミューテックス数が上限（NUM_MTXID）を超過
 * @retval E_RSATR	不正な属性が指定された（CHK_RSATR 有効時）
 * @retval E_PAR	上限優先度が不正（CHK_PAR 有効時）
 */
SYSCALL ID tk_cre_mtx( CONST T_CMTX *pk_cmtx )
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
	/* FreeQue から管理ブロックを取得 */
	mtxcb = (MTXCB*)QueRemoveNext(&knl_free_mtxcb);
	if ( mtxcb == NULL ) {
		ercd = E_LIMIT;
	} else {
		mtxid = ID_MTX(mtxcb - knl_mtxcb_table);

		/* 管理ブロックの初期化 */
		QueInit(&mtxcb->wait_queue);
		mtxcb->mtxid   = mtxid;
		mtxcb->exinf   = pk_cmtx->exinf;
		mtxcb->mtxatr  = pk_cmtx->mtxatr;
		mtxcb->ceilpri = ceilpri;
		mtxcb->mtxtsk  = NULL;
		mtxcb->mtxlist = NULL;
#if USE_OBJECT_NAME
		if ( (pk_cmtx->mtxatr & TA_DSNAME) != 0 ) {
			knl_strncpy((char*)mtxcb->name, (char*)pk_cmtx->dsname,
				(UINT)OBJECT_NAME_LENGTH);
		}
#endif
		ercd = mtxid;
	}
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_DEL_MTX
/**
 * @brief ミューテックスの削除
 *
 * ロック中のタスクがあればそのミューテックスリストから外して
 * 優先度を再調整し、ロック待ちのタスクは E_DLT で待ちを解除した
 * うえで、管理ブロックを FreeQue に返却します。
 *
 * @param mtxid	削除するミューテックスの ID
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象のミューテックスが存在しない
 */
SYSCALL ER tk_del_mtx( ID mtxid )
{
	MTXCB	*mtxcb;
	ER	ercd = E_OK;

	CHECK_MTXID(mtxid);

	mtxcb = get_mtxcb(mtxid);

	BEGIN_CRITICAL_SECTION;
	if ( mtxcb->mtxid == 0 ) {
		ercd = E_NOEXS;
	} else {
		/* 削除するミューテックスをロック中のタスクがあれば、
		 * そのタスクのリストからミューテックスを外し、
		 * 必要に応じてタスク優先度を再調整する。
		 */
		if ( mtxcb->mtxtsk != NULL ) {
			knl_release_mutex(mtxcb->mtxtsk, mtxcb);
		}

		/* 待ちタスクの待ち状態を解除（E_DLT を返す） */
		knl_wait_delete(&mtxcb->wait_queue);

		/* FreeQue へ返却 */
		QueInsert(&mtxcb->wait_queue, &knl_free_mtxcb);
		mtxcb->mtxid = 0;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_MTX */


/**
 * @brief 待ちタスクの優先度変更時の処理
 *
 * ロック待ちタスクの優先度変更に合わせて待ちキュー内の並び順を
 * 更新します。TA_INHERIT 属性の場合はさらに、ロック中タスクの
 * 優先度を待ちタスクの最高優先度に追従させます（引き上げ、
 * または再計算による引き下げ）。
 *
 * @param tcb	優先度が変更されたタスクの TCB
 * @param oldpri	変更前の優先度
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
			/* ロック待ちタスクの最高優先度が高くなったため、
			   ロック中タスクの優先度も引き上げる */
			knl_change_task_priority(mtxtsk, tcb->priority);

		} else if ( mtxtsk->priority == oldpri ) {
			/* ロック待ちタスクの最高優先度が低くなった
			   可能性があるため、優先度を再計算する */
			reset_priority(mtxtsk);
		}
	}
}

/**
 * @brief 待ちタスクの待ち解除時の処理（TA_INHERIT 専用）
 *
 * ロック待ちタスクが待ち解除された結果、待ちタスクの最高優先度が
 * 低くなった可能性がある場合に、ロック中タスクの優先度を再計算します。
 *
 * @param tcb	待ちが解除されたタスクの TCB
 */
LOCAL void mtx_rel_wai( TCB *tcb )
{
	MTXCB	*mtxcb;
	TCB	*mtxtsk;

	mtxcb = get_mtxcb(tcb->wid);
	mtxtsk = mtxcb->mtxtsk;

	if ( mtxtsk->priority == tcb->priority ) {
		/* ロック待ちタスクの最高優先度が低くなった
		   可能性があるため、優先度を再計算する */
		reset_priority(mtxtsk);
	}
}

/*
 * ミューテックス待ち仕様の定義
 */
LOCAL CONST WSPEC knl_wspec_mtx_tfifo   = { TTW_MTX, NULL, NULL };
LOCAL CONST WSPEC knl_wspec_mtx_tpri    = { TTW_MTX, mtx_chg_pri, NULL };
LOCAL CONST WSPEC knl_wspec_mtx_inherit = { TTW_MTX, mtx_chg_pri, mtx_rel_wai };

/**
 * @brief ミューテックスのロック
 *
 * 非ロック状態であればロックを取得します。TA_CEILING 属性の場合は
 * 必要に応じて自タスクの優先度を上限優先度まで引き上げます。
 * 他タスクがロック中の場合は tmout で指定した時間までロック待ち
 * 状態に入ります（TA_INHERIT 属性の場合はロック中タスクの優先度を
 * 自タスクの優先度まで引き上げます）。待ち解除条件はロックの獲得
 * （tk_unl_mtx またはロック中タスクの終了）です。
 *
 * @param mtxid	ロックするミューテックスの ID
 * @param tmout	タイムアウト時間（TMO_POL / TMO_FEVR 指定可）
 * @retval E_OK	正常終了（ロック獲得）
 * @retval E_NOEXS	対象のミューテックスが存在しない
 * @retval E_ILUSE	多重ロック、または上限優先度違反
 * @retval E_TMOUT	タイムアウト（ポーリング失敗を含む）
 * @retval E_RLWAI	待ち状態の強制解除
 * @retval E_DLT	待ちの間にミューテックスが削除された
 * @note ディスパッチ禁止中およびタスク独立部からは呼び出せません。
 */
SYSCALL ER tk_loc_mtx( ID mtxid, TMO tmout )
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
		ercd = E_ILUSE;  /* 多重ロック */
		goto error_exit;
	}

	mtxatr = mtxcb->mtxatr & TA_CEILING;
	if ( mtxatr == TA_CEILING ) {
		if ( knl_ctxtsk->bpriority < mtxcb->ceilpri ) {
			/* 上限優先度違反 */
			ercd = E_ILUSE;
			goto error_exit;
		}
	}

	mtxtsk = mtxcb->mtxtsk;
	if ( mtxtsk == NULL ) {
		/* ロックを取得 */
		mtxcb->mtxtsk = knl_ctxtsk;
		mtxcb->mtxlist = knl_ctxtsk->mtxlist;
		knl_ctxtsk->mtxlist = mtxcb;

		if ( mtxatr == TA_CEILING ) {
			if ( knl_ctxtsk->priority > mtxcb->ceilpri ) {
				/* 自タスクの優先度を上限優先度まで
				   引き上げる */
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
				/* ロック中タスクの優先度を自タスクと
				   同じ優先度まで引き上げる */
				knl_change_task_priority(mtxtsk, knl_ctxtsk->priority);
			}
		}

		/* 待ち状態に入る準備 */
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

/**
 * @brief ミューテックスのアンロック
 *
 * 自タスクがロックしているミューテックスを解放し、必要に応じて
 * 自タスクの優先度を再調整します。ロック待ちのタスクがあれば
 * 先頭のタスクの待ちを解除して新たなロック取得タスクとし、
 * TA_CEILING 属性であれば必要に応じてそのタスクの優先度を
 * 上限優先度まで引き上げます。
 *
 * @param mtxid	アンロックするミューテックスの ID
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象のミューテックスが存在しない
 * @retval E_ILUSE	自タスクがロックしていないミューテックスを指定
 * @note タスク独立部からは呼び出せません。
 */
SYSCALL ER tk_unl_mtx( ID mtxid )
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
		ercd = E_ILUSE;  /* 自タスクがロックしていない */
		goto error_exit;
	}

	/* ミューテックスをリストから外し、必要に応じて
	   自タスクの優先度を再調整する */
	knl_release_mutex(knl_ctxtsk, mtxcb);

	if ( mtx_waited(mtxcb) ) {
		tcb = (TCB*)mtxcb->wait_queue.next;

		/* 待ちを解除 */
		knl_wait_release_ok(tcb);

		/* ロック取得タスクを変更 */
		mtxcb->mtxtsk = tcb;
		mtxcb->mtxlist = tcb->mtxlist;
		tcb->mtxlist = mtxcb;

		if ( (mtxcb->mtxatr & TA_CEILING) == TA_CEILING ) {
			if ( tcb->priority > mtxcb->ceilpri ) {
				/* ロックを取得したタスクの優先度を
				   上限優先度まで引き上げる */
				knl_change_task_priority(tcb, mtxcb->ceilpri);
			}
		}
	} else {
		/* 待ちタスクなし */
		mtxcb->mtxtsk = NULL;
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}


#ifdef USE_FUNC_TK_REF_MTX
/**
 * @brief ミューテックスの状態参照
 *
 * 拡張情報・ロック中タスクの ID・待ちタスクの有無
 * （先頭タスク ID）を pk_rmtx に格納します。
 *
 * @param mtxid	参照するミューテックスの ID
 * @param pk_rmtx	ミューテックス状態を返す領域
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象のミューテックスが存在しない
 */
SYSCALL ER tk_ref_mtx( ID mtxid, T_RMTX *pk_rmtx )
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
 *	デバッガサポート機能
 */
#if USE_DBGSPT

#if USE_OBJECT_NAME
/**
 * @brief 管理ブロックからのオブジェクト名取得
 *
 * TA_DSNAME 属性付きで生成されたミューテックスの名前への
 * ポインタを返します。
 *
 * @param id	対象ミューテックスの ID
 * @param name	名前文字列へのポインタを返す領域
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象のミューテックスが存在しない
 * @retval E_OBJ	TA_DSNAME 属性が指定されていない
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

#ifdef USE_FUNC_TD_LST_MTX
/**
 * @brief ミューテックス ID 一覧の参照
 *
 * 使用中のミューテックス ID を list に最大 nent 個まで格納します。
 *
 * @param list	ID を格納する配列
 * @param nent	list に格納できる最大数
 * @return 使用中のミューテックスの総数（nent を超える場合もある）
 */
SYSCALL INT td_lst_mtx( ID list[], INT nent )
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
/**
 * @brief ミューテックスの状態参照（デバッガサポート）
 *
 * 拡張情報・ロック中タスクの ID・待ちタスクの有無
 * （先頭タスク ID）を pk_rmtx に格納します。
 *
 * @param mtxid	参照するミューテックスの ID
 * @param pk_rmtx	ミューテックス状態を返す領域
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象のミューテックスが存在しない
 */
SYSCALL ER td_ref_mtx( ID mtxid, TD_RMTX *pk_rmtx )
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
/**
 * @brief ミューテックス待ちキューの参照
 *
 * ロック待ちキューに並ぶタスクの ID を待ち順に list へ
 * 最大 nent 個まで格納します。
 *
 * @param mtxid	参照するミューテックスの ID
 * @param list	タスク ID を格納する配列
 * @param nent	list に格納できる最大数
 * @return 正の値または 0 ならば待ちタスクの総数、負の値ならばエラーコード
 * @retval E_NOEXS	対象のミューテックスが存在しない
 */
SYSCALL INT td_mtx_que( ID mtxid, ID list[], INT nent )
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
#endif /* USE_MUTEX */
