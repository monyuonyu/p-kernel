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
 * @file mempfix.c
 * @brief 固定長メモリプール管理機能
 * 
 * T-Kernelの固定長メモリプール機能を実装します。
 * 固定長メモリプールは、同一サイズのメモリブロックを効率的に管理し、
 * 高速な確保・解放を実現する仕組みです。
 * 
 * 主な機能：
 * - 固定長メモリプールの作成・削除
 * - メモリブロックの取得・返却
 * - タスク待ちキューの管理（FIFO/優先度順）
 * - メモリプール状態の参照
 * - デバッグサポート機能
 * 
 * 固定長メモリプールの特徴：
 * - 同一サイズのブロックによる高速なメモリ管理
 * - フラグメンテーションが発生しない
 * - O(1)の高速な確保・解放処理
 * - ユーザ指定メモリ領域またはシステム自動確保領域を選択可能
 * - フリーリストによる効率的な空きブロック管理
 * 
 * @note リアルタイム性を要求するシステムに最適なメモリ管理機構です
 */

/** [BEGIN Common Definitions] */
#include "kernel.h"
#include "task.h"
#include "wait.h"
#include "check.h"
#include "mempfix.h"
/** [END Common Definitions] */

#if CFN_MAX_MPFID > 0

#ifdef USE_FUNC_MPFCB_TABLE
Noinit(EXPORT MPFCB	knl_mpfcb_table[NUM_MPFID]);	/* Fixed size memory pool control block */
Noinit(EXPORT QUEUE	knl_free_mpfcb);	/* FreeQue */
#endif /* USE_FUNC_MPFCB_TABLE */


#ifdef USE_FUNC_FIX_MEMORYPOOL_INITIALIZE
/**
 * @brief 固定長メモリプール制御ブロックの初期化
 * 
 * システム起動時に固定長メモリプール管理機構を初期化します。
 * 全ての制御ブロックを空きキューに登録し、使用可能な状態にします。
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_SYS システムエラー（メモリプール数が不正）
 * 
 * @note この関数はシステム初期化時に一度だけ呼び出されます
 * @note NUM_MPFID が1未満の場合はエラーとなります
 */
EXPORT ER knl_fix_memorypool_initialize( void )
{
	MPFCB	*mpfcb, *end;

	/* Get system information */
	if ( NUM_MPFID < 1 ) {
		return E_SYS;
	}

	/* Register all control blocks onto FreeQue */
	QueInit(&knl_free_mpfcb);
	end = knl_mpfcb_table + NUM_MPFID;
	for ( mpfcb = knl_mpfcb_table; mpfcb < end; mpfcb++ ) {
		mpfcb->mpfid = 0;
		knl_InitOBJLOCK(&mpfcb->lock);
		QueInsert(&mpfcb->wait_queue, &knl_free_mpfcb);
	}

	return E_OK;
}
#endif /* USE_FUNC_FIX_MEMORYPOOL_INITIALIZE */


#ifdef USE_FUNC_TK_CRE_MPF
/**
 * @brief 固定長メモリプールの作成
 * 
 * 指定された属性で固定長メモリプールを作成します。
 * 作成されたメモリプールには一意のIDが割り当てられます。
 * 
 * @param pk_cmpf メモリプール作成情報パケットへのポインタ
 * @return ID 作成されたメモリプールID（正の値）、またはエラーコード（負の値）
 * @retval E_LIMIT 利用可能なメモリプールがない
 * @retval E_NOMEM メモリ不足
 * @retval E_RSATR 予約属性が指定された
 * @retval E_PAR パラメータエラー
 * 
 * 対応する属性：
 * - TA_TPRI: タスク優先度順待ち
 * - TA_RNG3: ユーザモードでのアクセス許可
 * - TA_USERBUF: ユーザ指定バッファ使用
 * - TA_DSNAME: オブジェクト名の指定
 * 
 * @note メモリプールIDは1以上の値が割り当てられます
 * @note TA_USERBUF未指定時はImallocでメモリを自動確保します
 */
SYSCALL ID tk_cre_mpf_impl( CONST T_CMPF *pk_cmpf )
{
#if CHK_RSATR
	const ATR VALID_MPFATR = {
		 TA_TPRI
		|TA_RNG3
		|TA_USERBUF
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
	};
#endif
	MPFCB	*mpfcb;
	ID	mpfid;
	W	blfsz, mpfsz;
	void	*mempool;

	CHECK_RSATR(pk_cmpf->mpfatr, VALID_MPFATR);
	CHECK_PAR(pk_cmpf->mpfcnt > 0);
	CHECK_PAR(pk_cmpf->blfsz > 0);
#if !USE_IMALLOC
	/* TA_USERBUF must be specified if configured in no Imalloc */
	CHECK_PAR((pk_cmpf->mpfatr & TA_USERBUF) != 0);
#endif
	CHECK_DISPATCH();

	blfsz = (W)MINSZ(pk_cmpf->blfsz);
	mpfsz = blfsz * pk_cmpf->mpfcnt;

#if USE_IMALLOC
	if ( (pk_cmpf->mpfatr & TA_USERBUF) != 0 ) {
		/* Size of user buffer must be multiples of sizeof(FREEL) */
		if ( blfsz != pk_cmpf->blfsz ) {
			return E_PAR;
		}
		/* Use user buffer */
		mempool = pk_cmpf->bufptr;
	} else {
		/* Allocate memory for memory pool */
		mempool = knl_Imalloc((UW)mpfsz);
		if ( mempool == NULL ) {
			return E_NOMEM;
		}
	}
#else
	/* Size of user buffer must be larger than sizeof(FREEL) */
	if ( blfsz != pk_cmpf->blfsz ) {
		return E_PAR;
	}
	/* Use user buffer */
	mempool = pk_cmpf->bufptr;
#endif

	/* Get control block from FreeQue */
	DISABLE_INTERRUPT;
	mpfcb = (MPFCB*)QueRemoveNext(&knl_free_mpfcb);
	ENABLE_INTERRUPT;

	if ( mpfcb == NULL ) {
#if USE_IMALLOC
		if ( (pk_cmpf->mpfatr & TA_USERBUF) == 0 ) {
			knl_Ifree(mempool);
		}
#endif
		return E_LIMIT;
	}

	knl_LockOBJ(&mpfcb->lock);
	mpfid = ID_MPF(mpfcb - knl_mpfcb_table);

	/* Initialize control block */
	QueInit(&mpfcb->wait_queue);
	mpfcb->exinf    = pk_cmpf->exinf;
	mpfcb->mpfatr   = pk_cmpf->mpfatr;
	mpfcb->mpfcnt   = mpfcb->frbcnt = pk_cmpf->mpfcnt;
	mpfcb->blfsz    = blfsz;
	mpfcb->mpfsz    = mpfsz;
	mpfcb->unused   = mpfcb->mempool = mempool;
	mpfcb->freelist = NULL;
#if USE_OBJECT_NAME
	if ( (pk_cmpf->mpfatr & TA_DSNAME) != 0 ) {
		strncpy((char*)mpfcb->name, (char*)pk_cmpf->dsname, OBJECT_NAME_LENGTH);
	}
#endif

	mpfcb->mpfid    = mpfid;  /* Set ID after completion */
	knl_UnlockOBJ(&mpfcb->lock);

	return mpfid;
}
#endif /* USE_FUNC_TK_CRE_MPF */

#ifdef USE_FUNC_TK_DEL_MPF
/**
 * @brief 固定長メモリプールの削除
 * 
 * 指定された固定長メモリプールを削除し、リソースを解放します。
 * 削除時に待ちタスクがある場合は、E_DLTエラーで待ち解除されます。
 * 
 * @param mpfid 削除対象のメモリプールID
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正なメモリプールID
 * @retval E_NOEXS 指定のメモリプールが存在しない
 * 
 * @note 削除されたメモリプールIDは再利用される可能性があります
 * @note TA_USERBUF未指定の場合、メモリ領域はImallocで解放されます
 * @note 待ちタスクは全てE_DLTエラーで待ち解除されます
 */
SYSCALL ER tk_del_mpf_impl( ID mpfid )
{
	MPFCB	*mpfcb;
	void	*mempool = NULL;
	ATR	memattr = 0;
	ER	ercd = E_OK;

	CHECK_MPFID(mpfid);
	CHECK_DISPATCH();

	mpfcb = get_mpfcb(mpfid);

	knl_LockOBJ(&mpfcb->lock);
	if ( mpfcb->mpfid == 0 ) {
		ercd = E_NOEXS;
	} else {
		DISABLE_INTERRUPT;
		mempool = mpfcb->mempool;
		memattr = mpfcb->mpfatr;

		/* Release wait state of task (E_DLT) */
		knl_wait_delete(&mpfcb->wait_queue);

		/* Return to FreeQue */
		QueInsert(&mpfcb->wait_queue, &knl_free_mpfcb);
		mpfcb->mpfid = 0;
		ENABLE_INTERRUPT;
	}
	knl_UnlockOBJ(&mpfcb->lock);

#if USE_IMALLOC
	if ( (mempool != NULL) && ((memattr & TA_USERBUF) == 0) ) {
		knl_Ifree(mempool);
	}
#endif

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_MPF */

#ifdef USE_FUNC_TK_GET_MPF
/**
 * @brief 待ちタスクの優先度変更時の処理
 * 
 * メモリプール取得待ち中のタスクの優先度が変更された場合に
 * 呼び出される内部関数です。待ちキューの順序を再調整します。
 * 
 * @param tcb 優先度が変更されたタスクの制御ブロック
 * @param oldpri 変更前の優先度（未使用）
 * 
 * @note この関数はTA_TPRI属性のメモリプールでのみ使用されます
 */
LOCAL void knl_mpf_chg_pri( TCB *tcb, INT oldpri )
{
	MPFCB	*mpfcb;

	mpfcb = get_mpfcb(tcb->wid);
	knl_gcb_change_priority((GCB*)mpfcb, tcb);
}

/**
 * @brief 固定長メモリプール待ち仕様の定義
 * 
 * FIFO順待ち（TA_TPRI未指定）と優先度順待ち（TA_TPRI指定）の
 * 待ち仕様を定義します。
 */
LOCAL CONST WSPEC knl_wspec_mpf_tfifo = { TTW_MPF, NULL, NULL };
LOCAL CONST WSPEC knl_wspec_mpf_tpri  = { TTW_MPF, knl_mpf_chg_pri, NULL };

/**
 * @brief 固定長メモリブロックの取得
 * 
 * 指定された固定長メモリプールからメモリブロックを取得します。
 * 空きブロックがある場合は即座に取得し、ない場合は指定時間まで待機します。
 * 
 * @param mpfid メモリプールID
 * @param p_blf 取得したメモリブロックポインタを格納する領域
 * @param tmout タイムアウト時間（TMO_FEVR=無限待ち、TMO_POL=ポーリング）
 * @return ER エラーコード
 * @retval E_OK 正常終了（メモリブロック取得成功）
 * @retval E_ID 不正なメモリプールID
 * @retval E_NOEXS 指定のメモリプールが存在しない
 * @retval E_TMOUT タイムアウト
 * @retval E_RLWAI 待ち状態の強制解除
 * @retval E_DLT 待ち対象の削除
 * @retval E_CTX コンテキストエラー
 * 
 * @note 取得したメモリブロックは呼び出し側が管理し、使用後はtk_rel_mpfで返却する必要があります
 * @note TA_TPRI指定時は優先度順、未指定時はFIFO順で待ちキューが管理されます
 */
SYSCALL ER tk_get_mpf_impl( ID mpfid, void **p_blf, TMO tmout )
{
	MPFCB	*mpfcb;
	FREEL	*free;
	ER	ercd = E_OK;

	CHECK_MPFID(mpfid);
	CHECK_TMOUT(tmout);
	CHECK_DISPATCH();

	mpfcb = get_mpfcb(mpfid);

	knl_LockOBJ(&mpfcb->lock);
	if ( mpfcb->mpfid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}

	/* If there is no space, ready for wait */
	if ( mpfcb->frbcnt <= 0 ) {
		goto wait_mpf;
	} else {
		/* Get memory block */
		if ( mpfcb->freelist != NULL ) {
			free = mpfcb->freelist;
			mpfcb->freelist = free->next;
			*p_blf = free;
		} else {
			*p_blf = mpfcb->unused;
			mpfcb->unused = (VB*)mpfcb->unused + mpfcb->blfsz;
		}
		mpfcb->frbcnt--;
	}

    error_exit:
	knl_UnlockOBJ(&mpfcb->lock);

	return ercd;

wait_mpf:
	/* Ready for wait */
	BEGIN_CRITICAL_SECTION;
	knl_ctxtsk->wspec = ( (mpfcb->mpfatr & TA_TPRI) != 0 )?
				&knl_wspec_mpf_tpri: &knl_wspec_mpf_tfifo;
	knl_ctxtsk->wercd = &ercd;
	knl_ctxtsk->winfo.mpf.p_blf = p_blf;
	knl_gcb_make_wait((GCB*)mpfcb, tmout);

	knl_UnlockOBJ(&mpfcb->lock);
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_GET_MPF */

#ifdef USE_FUNC_TK_REL_MPF
/**
 * @brief 固定長メモリブロックの返却
 * 
 * 以前にtk_get_mpfで取得した固定長メモリブロックをメモリプールに返却します。
 * 待ちタスクがある場合は直接渡し、ない場合はフリーリストに追加します。
 * 
 * @param mpfid メモリプールID
 * @param blf 返却するメモリブロックのポインタ
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正なメモリプールID
 * @retval E_NOEXS 指定のメモリプールが存在しない
 * @retval E_PAR 不正なメモリブロックポインタ
 * 
 * @note 返却するブロックは指定されたメモリプールから取得したものである必要があります
 * @note 無効なポインタや他のメモリプールのブロックを指定した場合はE_PARエラーとなります
 */
SYSCALL ER tk_rel_mpf_impl( ID mpfid, void *blf )
{
	MPFCB	*mpfcb;
	TCB	*tcb;
	FREEL	*free;
	ER	ercd = E_OK;

	CHECK_MPFID(mpfid);
	CHECK_DISPATCH();

	mpfcb = get_mpfcb(mpfid);

	knl_LockOBJ(&mpfcb->lock);
	if ( mpfcb->mpfid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
#if CHK_PAR
	if ( blf < mpfcb->mempool || blf >= knl_mempool_end(mpfcb) || (((VB*)blf - (VB*)mpfcb->mempool) % mpfcb->blfsz) != 0 ) {
		ercd = E_PAR;
		goto error_exit;
	}
#endif

	DISABLE_INTERRUPT;
	if ( !isQueEmpty(&mpfcb->wait_queue) ) {
		/* Send memory block to waiting task,
		   and then release the task */
		tcb = (TCB*)mpfcb->wait_queue.next;
		*tcb->winfo.mpf.p_blf = blf;
		knl_wait_release_ok(tcb);
		ENABLE_INTERRUPT;
	} else {
		ENABLE_INTERRUPT;
		/* Free memory block */
		free = (FREEL*)blf;
		free->next = mpfcb->freelist;
		mpfcb->freelist = free;
		mpfcb->frbcnt++;
	}

error_exit:
	knl_UnlockOBJ(&mpfcb->lock);

	return ercd;
}
#endif /* USE_FUNC_TK_REL_MPF */

#ifdef USE_FUNC_TK_REF_MPF
/**
 * @brief 固定長メモリプール状態の参照
 * 
 * 指定された固定長メモリプールの現在の状態を取得します。
 * 拡張情報、待ちタスクID、空きブロック数を返します。
 * 
 * @param mpfid 参照対象のメモリプールID
 * @param pk_rmpf メモリプール状態を格納する領域
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正なメモリプールID
 * @retval E_NOEXS 指定のメモリプールが存在しない
 * 
 * 取得される情報：
 * - exinf: 拡張情報
 * - wtsk: メモリブロック取得待ちタスクのID（待ちタスクがない場合は0）
 * - frbcnt: 空きブロック数
 * 
 * @note この関数は状態参照のみで、メモリプールの動作には影響しません
 */
SYSCALL ER tk_ref_mpf_impl( ID mpfid, T_RMPF *pk_rmpf )
{
	MPFCB	*mpfcb;
	ER	ercd = E_OK;

	CHECK_MPFID(mpfid);
	CHECK_DISPATCH();

	mpfcb = get_mpfcb(mpfid);

	knl_LockOBJ(&mpfcb->lock);
	if ( mpfcb->mpfid == 0 ) {
		ercd = E_NOEXS;
	} else {
		DISABLE_INTERRUPT;
		pk_rmpf->wtsk = knl_wait_tskid(&mpfcb->wait_queue);
		ENABLE_INTERRUPT;
		pk_rmpf->exinf = mpfcb->exinf;
		pk_rmpf->frbcnt = mpfcb->frbcnt;
	}
	knl_UnlockOBJ(&mpfcb->lock);

	return ercd;
}
#endif /* USE_FUNC_TK_REF_MPF */

/* ------------------------------------------------------------------------ */
/**
 * @brief デバッガサポート機能
 * 
 * デバッグ時の固定長メモリプール情報取得機能を提供します。
 */
#if USE_DBGSPT

#ifdef USE_FUNC_FIX_MEMORYPOOL_GETNAME
#if USE_OBJECT_NAME
/**
 * @brief メモリプールオブジェクト名の取得
 * 
 * デバッガ用：指定されたメモリプールIDからオブジェクト名を取得します。
 * 
 * @param id メモリプールID
 * @param name オブジェクト名を格納するポインタ変数
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正なメモリプールID
 * @retval E_NOEXS 指定のメモリプールが存在しない
 * @retval E_OBJ オブジェクト名が設定されていない
 * 
 * @note この関数はデバッグサポート機能が有効な場合のみ利用可能です
 * @note TA_DSNAMEが指定されていないメモリプールではE_OBJエラーとなります
 */
EXPORT ER knl_fix_memorypool_getname(ID id, UB **name)
{
	MPFCB	*mpfcb;
	ER	ercd = E_OK;

	CHECK_MPFID(id);

	BEGIN_DISABLE_INTERRUPT;
	mpfcb = get_mpfcb(id);
	if ( mpfcb->mpfid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (mpfcb->mpfatr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = mpfcb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */
#endif /* USE_FUNC_FIX_MEMORYPOOL_GETNAME */

#ifdef USE_FUNC_TD_LST_MPF
/**
 * @brief 固定長メモリプール使用状況の参照
 * 
 * デバッガ用：現在作成されている固定長メモリプールのIDリストを取得します。
 * 
 * @param list メモリプールIDを格納する配列
 * @param nent 配列の要素数
 * @return INT 実際のメモリプール数
 * 
 * @note 戻り値が nent より大きい場合、全てのIDを取得するには
 *       より大きな配列が必要です
 * @note この関数はデバッグサポート機能が有効な場合のみ利用可能です
 */
SYSCALL INT td_lst_mpf_impl( ID list[], INT nent )
{
	MPFCB	*mpfcb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_mpfcb_table + NUM_MPFID;
	for ( mpfcb = knl_mpfcb_table; mpfcb < end; mpfcb++ ) {
		if ( mpfcb->mpfid == 0 ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = ID_MPF(mpfcb - knl_mpfcb_table);
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_MPF */

#ifdef USE_FUNC_TD_REF_MPF
/**
 * @brief 固定長メモリプール状態の参照（デバッガ用）
 * 
 * デバッガ用：指定された固定長メモリプールの詳細な状態を取得します。
 * tk_ref_mpf と同様の情報を取得しますが、デバッガ用の追加情報も含みます。
 * 
 * @param mpfid 参照対象のメモリプールID
 * @param pk_rmpf メモリプール状態を格納する領域
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正なメモリプールID
 * @retval E_NOEXS 指定のメモリプールが存在しない
 * @retval E_CTX コンテキストエラー
 * 
 * @note この関数はデバッグサポート機能が有効な場合のみ利用可能です
 */
SYSCALL ER td_ref_mpf_impl( ID mpfid, TD_RMPF *pk_rmpf )
{
	MPFCB	*mpfcb;
	ER	ercd = E_OK;

	CHECK_MPFID(mpfid);

	mpfcb = get_mpfcb(mpfid);

	BEGIN_DISABLE_INTERRUPT;
	if ( mpfcb->mpfid == 0 ) {
		ercd = E_NOEXS;
	} else if ( knl_isLockedOBJ(&mpfcb->lock) ) {
		ercd = E_CTX;
	} else {
		pk_rmpf->wtsk = knl_wait_tskid(&mpfcb->wait_queue);
		pk_rmpf->exinf = mpfcb->exinf;
		pk_rmpf->frbcnt = mpfcb->frbcnt;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_MPF */

#ifdef USE_FUNC_TD_MPF_QUE
/**
 * @brief 固定長メモリプール待ちキューの参照
 * 
 * デバッガ用：指定された固定長メモリプールでメモリブロック取得待ちしているタスクのIDリストを取得します。
 * 
 * @param mpfid 対象のメモリプールID
 * @param list 待ちタスクIDを格納する配列
 * @param nent 配列の要素数
 * @return INT 実際の待ちタスク数（正の値）、またはエラーコード（負の値）
 * @retval E_ID 不正なメモリプールID
 * @retval E_NOEXS 指定のメモリプールが存在しない
 * 
 * @note 戻り値が nent より大きい場合、全てのタスクIDを取得するには
 *       より大きな配列が必要です
 * @note この関数はデバッグサポート機能が有効な場合のみ利用可能です
 */
SYSCALL INT td_mpf_que_impl( ID mpfid, ID list[], INT nent )
{
	MPFCB	*mpfcb;
	QUEUE	*q;
	ER	ercd = E_OK;

	CHECK_MPFID(mpfid);

	mpfcb = get_mpfcb(mpfid);

	BEGIN_DISABLE_INTERRUPT;
	if ( mpfcb->mpfid == 0 ) {
		ercd = E_NOEXS;
	} else {
		INT n = 0;
		for ( q = mpfcb->wait_queue.next; q != &mpfcb->wait_queue; q = q->next ) {
			if ( n++ < nent ) {
				*list++ = ((TCB*)q)->tskid;
			}
		}
		ercd = n;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_MPF_QUE */

#endif /* USE_DBGSPT */
#endif /* CFN_MAX_MPFID */
