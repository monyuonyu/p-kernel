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
 * @file	mempfix.c
 * @brief	固定長メモリプール
 *
 * 固定長メモリプールの生成・削除・ブロックの獲得と返却・状態参照
 * （tk_cre_mpf / tk_del_mpf / tk_get_mpf / tk_rel_mpf / tk_ref_mpf）、
 * およびデバッガサポート機能（td_lst_mpf / td_ref_mpf / td_mpf_que）を
 * 実装します。
 */

#include "kernel.h"
#include "klock.h"
#include "wait.h"
#include "check.h"
#include "mempfix.h"

#if USE_FIX_MEMORYPOOL

Noinit(EXPORT MPFCB	knl_mpfcb_table[NUM_MPFID]);	/* 固定長メモリプール管理ブロックテーブル */
Noinit(EXPORT QUEUE	knl_free_mpfcb);	/* 未使用管理ブロックのキュー（FreeQue） */


/**
 * @brief 固定長メモリプール管理ブロックの初期化
 *
 * すべての管理ブロックを未使用状態にして FreeQue に登録します。
 * カーネルの初期化時に呼び出されます。
 *
 * @retval E_OK	正常終了
 * @retval E_SYS	構成エラー（NUM_MPFID が 1 未満）
 */
EXPORT ER knl_fix_memorypool_initialize( void )
{
	MPFCB	*mpfcb, *end;

	/* 構成の確認 */
	if ( NUM_MPFID < 1 ) {
		return E_SYS;
	}

	/* 全管理ブロックを FreeQue に登録 */
	QueInit(&knl_free_mpfcb);
	end = knl_mpfcb_table + NUM_MPFID;
	for ( mpfcb = knl_mpfcb_table; mpfcb < end; mpfcb++ ) {
		mpfcb->mpfid = 0;
		knl_InitOBJLOCK(&mpfcb->lock);
		QueInsert(&mpfcb->wait_queue, &knl_free_mpfcb);
	}

	return E_OK;
}


/**
 * @brief 固定長メモリプールの生成
 *
 * 生成情報 pk_cmpf に従って固定長メモリプールを生成します。
 * ブロックサイズは sizeof(FREEL) の倍数に切り上げられます。
 * TA_USERBUF 属性を指定した場合は pk_cmpf->bufptr のユーザバッファを
 * プール領域として使用し、指定しない場合はカーネルが領域を確保します。
 *
 * @param pk_cmpf	メモリプール生成情報パケット
 * @return 正の値: 生成したメモリプール ID、負の値: エラーコード
 * @retval E_PAR	パラメータ不正（TA_USERBUF 指定時にブロックサイズが
 *			sizeof(FREEL) の倍数でない等）
 * @retval E_NOMEM	プール領域のメモリ確保失敗
 * @retval E_LIMIT	管理ブロックが不足（プール数の上限超過）
 */
SYSCALL ID tk_cre_mpf( CONST T_CMPF *pk_cmpf )
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
	/* Imalloc なしの構成では TA_USERBUF の指定が必須 */
	CHECK_PAR((pk_cmpf->mpfatr & TA_USERBUF) != 0);
#endif
	CHECK_DISPATCH();

	blfsz = (W)MINSZ(pk_cmpf->blfsz);
	mpfsz = blfsz * pk_cmpf->mpfcnt;

#if USE_IMALLOC
	if ( (pk_cmpf->mpfatr & TA_USERBUF) != 0 ) {
		/* ユーザバッファ使用時、ブロックサイズは sizeof(FREEL) の倍数であること */
		if ( blfsz != pk_cmpf->blfsz ) {
			return E_PAR;
		}
		/* ユーザバッファを使用 */
		mempool = pk_cmpf->bufptr;
	} else {
		/* メモリプール領域を確保 */
		mempool = knl_Imalloc((UW)mpfsz);
		if ( mempool == NULL ) {
			return E_NOMEM;
		}
	}
#else
	/* ユーザバッファのブロックサイズは sizeof(FREEL) の倍数であること */
	if ( blfsz != pk_cmpf->blfsz ) {
		return E_PAR;
	}
	/* ユーザバッファを使用 */
	mempool = pk_cmpf->bufptr;
#endif

	/* FreeQue から管理ブロックを取得 */
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

	/* 管理ブロックの初期化 */
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
		knl_strncpy((char*)mpfcb->name, (char*)pk_cmpf->dsname, OBJECT_NAME_LENGTH);
	}
#endif

	mpfcb->mpfid    = mpfid;  /* 初期化完了後に ID を設定 */
	knl_UnlockOBJ(&mpfcb->lock);

	return mpfid;
}

#ifdef USE_FUNC_TK_DEL_MPF
/**
 * @brief 固定長メモリプールの削除
 *
 * 指定したメモリプールを削除します。ブロック獲得待ちのタスクは
 * E_DLT で待ち解除されます。カーネルが確保したプール領域
 * （TA_USERBUF なし）は解放されます。
 *
 * @param mpfid	メモリプール ID
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象メモリプールが存在しない
 */
SYSCALL ER tk_del_mpf( ID mpfid )
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

		/* 待ちタスクの待ち解除（E_DLT） */
		knl_wait_delete(&mpfcb->wait_queue);

		/* 管理ブロックを FreeQue へ返却 */
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

/**
 * @brief 待ちタスクの優先度変更時の処理
 *
 * ブロック獲得待ちタスクの優先度が変更されたとき、待ちキュー内の
 * 位置を新しい優先度に従って並べ替えます。
 *
 * @param tcb	優先度が変更されたタスクの TCB
 * @param oldpri	変更前の優先度（未使用）
 */
LOCAL void knl_mpf_chg_pri( TCB *tcb, INT oldpri )
{
	MPFCB	*mpfcb;

	mpfcb = get_mpfcb(tcb->wid);
	knl_gcb_change_priority((GCB*)mpfcb, tcb);
}

/*
 * 固定長メモリプール待ち仕様の定義
 */
LOCAL CONST WSPEC knl_wspec_mpf_tfifo = { TTW_MPF, NULL, NULL };
LOCAL CONST WSPEC knl_wspec_mpf_tpri  = { TTW_MPF, knl_mpf_chg_pri, NULL };

/**
 * @brief 固定長メモリブロックの獲得
 *
 * メモリプールからブロックを 1 つ獲得します。空きブロックがない場合、
 * タスクは待ち状態となり、他タスクのブロック返却により待ち解除
 * されます。
 *
 * @param mpfid	メモリプール ID
 * @param p_blf	獲得したブロックの先頭アドレスの格納先
 * @param tmout	タイムアウト時間（TMO_POL / TMO_FEVR 指定可）
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象メモリプールが存在しない
 * @retval E_TMOUT	タイムアウト（TMO_POL 指定時のポーリング失敗を含む）
 * @retval E_RLWAI	待ち状態の強制解除
 * @retval E_DLT	待ちの間にメモリプールが削除された
 */
SYSCALL ER tk_get_mpf( ID mpfid, void **p_blf, TMO tmout )
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

	/* 空きブロックがなければ待ち状態へ */
	if ( mpfcb->frbcnt <= 0 ) {
		goto wait_mpf;
	} else {
		/* メモリブロックの取得 */
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
	/* 待ち状態への移行 */
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

/**
 * @brief 固定長メモリブロックの返却
 *
 * 獲得したブロックをメモリプールへ返却します。ブロック獲得待ちの
 * タスクがあれば、返却したブロックを先頭の待ちタスクへ渡して
 * 待ち解除します。
 *
 * @param mpfid	メモリプール ID
 * @param blf	返却するメモリブロックの先頭アドレス
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象メモリプールが存在しない
 * @retval E_PAR	blf がプール範囲外、またはブロック境界に一致しない
 */
SYSCALL ER tk_rel_mpf( ID mpfid, void *blf )
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
		/* 待ちタスクへメモリブロックを渡し、
		   そのタスクを待ち解除する */
		tcb = (TCB*)mpfcb->wait_queue.next;
		*tcb->winfo.mpf.p_blf = blf;
		knl_wait_release_ok(tcb);
		ENABLE_INTERRUPT;
	} else {
		ENABLE_INTERRUPT;
		/* メモリブロックをフリーリストへ返却 */
		free = (FREEL*)blf;
		free->next = mpfcb->freelist;
		mpfcb->freelist = free;
		mpfcb->frbcnt++;
	}

error_exit:
	knl_UnlockOBJ(&mpfcb->lock);

	return ercd;
}

#ifdef USE_FUNC_TK_REF_MPF
/**
 * @brief 固定長メモリプールの状態参照
 *
 * 待ちタスクの有無、拡張情報、空きブロック数を取得します。
 *
 * @param mpfid	メモリプール ID
 * @param pk_rmpf	状態情報の格納先
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象メモリプールが存在しない
 */
SYSCALL ER tk_ref_mpf( ID mpfid, T_RMPF *pk_rmpf )
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
/*
 *	デバッガサポート機能
 */
#if USE_DBGSPT

#if USE_OBJECT_NAME
/**
 * @brief 管理ブロックからのオブジェクト名取得
 *
 * TA_DSNAME 属性付きで生成されたメモリプールの名称への
 * ポインタを返します。
 *
 * @param id	メモリプール ID
 * @param name	オブジェクト名へのポインタの格納先
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象メモリプールが存在しない
 * @retval E_OBJ	TA_DSNAME 属性が指定されていない
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

#ifdef USE_FUNC_TD_LST_MPF
/**
 * @brief 固定長メモリプール ID の一覧取得
 *
 * 使用中のメモリプールの ID を list に格納します。
 *
 * @param list	ID を格納する配列
 * @param nent	list に格納できる最大数
 * @return 使用中のメモリプール数（nent を超える場合も総数を返す）
 */
SYSCALL INT td_lst_mpf( ID list[], INT nent )
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
 * @brief 固定長メモリプールの状態参照（デバッガサポート）
 *
 * @param mpfid	メモリプール ID
 * @param pk_rmpf	状態情報の格納先
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象メモリプールが存在しない
 * @retval E_CTX	オブジェクトがロックされている（コンテキストエラー）
 */
SYSCALL ER td_ref_mpf( ID mpfid, TD_RMPF *pk_rmpf )
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
 * ブロック獲得待ちタスクの ID を待ちキューの並び順に list へ
 * 格納します。
 *
 * @param mpfid	メモリプール ID
 * @param list	タスク ID を格納する配列
 * @param nent	list に格納できる最大数
 * @return 待ちタスク数（nent を超える場合も総数）、または E_NOEXS
 */
SYSCALL INT td_mpf_que( ID mpfid, ID list[], INT nent )
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
#endif /* USE_FIX_MEMORYPOOL */
