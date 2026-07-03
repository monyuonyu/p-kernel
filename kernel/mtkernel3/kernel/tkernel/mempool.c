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
 * @file	mempool.c
 * @brief	可変長メモリプール
 *
 * 可変長メモリプールの生成・削除・ブロックの獲得と返却・状態参照
 * （tk_cre_mpl / tk_del_mpl / tk_get_mpl / tk_rel_mpl / tk_ref_mpl）、
 * およびデバッガサポート機能（td_lst_mpl / td_ref_mpl / td_mpl_que）を
 * 実装します。プール内の領域管理には AreaQue（アドレス順）と
 * FreeQue（サイズ順）の 2 種類のキューを使用します。
 */

#include "kernel.h"
#include "wait.h"
#include "check.h"
#include "memory.h"
#include "mempool.h"

#if USE_MEMORYPOOL


Noinit(EXPORT MPLCB knl_mplcb_table[NUM_MPLID]);	/* 可変長メモリプール管理ブロックテーブル */
Noinit(EXPORT QUEUE knl_free_mplcb);	/* 未使用管理ブロックのキュー（FreeQue） */


/**
 * @brief 可変長メモリプール管理ブロックの初期化
 *
 * すべての管理ブロックを未使用状態にして FreeQue に登録します。
 * カーネルの初期化時に呼び出されます。
 *
 * @retval E_OK	正常終了
 * @retval E_SYS	構成エラー（NUM_MPLID が 1 未満）
 */
EXPORT ER knl_memorypool_initialize( void )
{
	MPLCB	*mplcb, *end;

	if ( NUM_MPLID < 1 ) {
		return E_SYS;
	}

	/* 全管理ブロックを FreeQue に登録 */
	QueInit(&knl_free_mplcb);
	end = knl_mplcb_table + NUM_MPLID;
	for ( mplcb = knl_mplcb_table; mplcb < end; mplcb++ ) {
		mplcb->mplid = 0;
		QueInsert(&mplcb->wait_queue, &knl_free_mplcb);
	}

	return E_OK;
}

/* ------------------------------------------------------------------------ */

/**
 * @brief 空き領域の FreeQue への登録（プール先頭・終端専用版）
 *
 * 空き領域を FreeQue のサイズ順の位置に登録します。プール先頭の領域
 * （aq == &areaque）および終端に接する領域を扱えるように、領域の
 * 実アドレスをプール範囲から補正する特殊版です。同じサイズの空き領域が
 * 既にあれば、その同サイズリストへつなぎます。
 *
 * @param mplcb	メモリプール管理ブロック
 * @param aq	登録する領域の AreaQue エントリ
 */
LOCAL void knl_appendFreeAreaBound( MPLCB *mplcb, QUEUE *aq )
{
	IMACB	*imacb = (IMACB*)&(mplcb->mplsz);
	QUEUE	*fq, *top, *end;
	W	size;

	if ( aq == &(mplcb->areaque) ) {
		top = (QUEUE*)mplcb->mempool;
	} else {
		top = aq + 1;
	}

	if ( aq->next == &(mplcb->areaque_end) ) {
		end = (QUEUE*)((VB*)mplcb->mempool + mplcb->mplsz);
	} else {
		end = aq->next;
	}

	size = (W)((VB*)end - (VB*)top);

	/* 登録位置の探索 */
	/*  サイズが 'blksz' と等しいか、'blksz' より大きく最も近い
	 *  空き領域を探す。
	 *  存在しなければ '&imacb->freeque' を返す。
	 */
	fq = knl_searchFreeArea(imacb, size);

	/* 登録 */
	clrAreaFlag(aq, AREA_USE);
	if ( fq != &imacb->freeque && FreeSize(fq) == size ) {
		/* FreeQue 内の同サイズリストへ登録 */
		(top + 1)->next = (fq + 1)->next;
		(fq  + 1)->next = top + 1;
		(top + 1)->prev = fq + 1;
		if( (top + 1)->next != NULL ) {
			(top + 1)->next->prev = top + 1;
		}
		top->next = NULL;
	} else {
		/* FreeQue のサイズ順の位置へ登録 */
		QueInsert(top, fq);
		(top + 1)->next = NULL;
		(top + 1)->prev = (QUEUE*)(unsigned long)size;	/* p-kernel: LP64 の警告回避（サイズ値の格納） */
	}
}

/**
 * @brief メモリブロックの獲得（内部処理）
 *
 * FreeQue から blksz 以上の空き領域を探して割り当てます。
 * 残りが最小フラグメントサイズ以上あれば領域を分割し、余りを
 * FreeQue に登録し直します。
 *
 * @param mplcb	メモリプール管理ブロック
 * @param blksz	要求ブロックサイズ（最小フラグメントサイズ以上、
 *		ROUNDSZ 単位に調整済みであること）
 * @return 獲得したブロックの先頭アドレス（空きがなければ NULL）
 */
LOCAL void *knl_get_blk( MPLCB *mplcb, W blksz )
{
	QUEUE	*q, *aq, *aq2;
	IMACB*	imacb = (IMACB*)&(mplcb->mplsz);

	/* FreeQue の探索 */
	q = knl_searchFreeArea(imacb, blksz);
	if ( q == &(imacb->freeque) ) {
		return NULL;
	}

	/* 空き領域を FreeQue から外す */
	knl_removeFreeQue(q);
	aq = ((void *)q == mplcb->mempool) ? &(mplcb->areaque) :  q - 1;

	/* 最小フラグメントサイズより小さい余りが出る場合は
	   分割せずまとめて割り当てる */
	if ( FreeSize(q) - (UW)blksz >= MIN_FRAGMENT + sizeof(QUEUE) ) {

		/* 領域を 2 つに分割する */
		aq2 = (QUEUE*)((VB*)q + blksz);
		knl_insertAreaQue(aq, aq2);

		/* 残りの領域を FreeQue に登録 */
		if ( aq2->next == &(mplcb->areaque_end) ) {
			knl_appendFreeAreaBound(mplcb, aq2);
		} else {
			knl_appendFreeArea(imacb, aq2);
		}
	}
	setAreaFlag(aq, AREA_USE);

	return (void *)q;
}

/**
 * @brief メモリブロックの解放（内部処理）
 *
 * ブロックを解放し、前後の空き領域と併合して FreeQue に登録します。
 *
 * @param mplcb	メモリプール管理ブロック
 * @param blk	解放するブロックの先頭アドレス
 * @retval E_OK	正常終了
 * @retval E_PAR	blk が使用中ブロックではない（CHK_PAR 有効時）
 */
LOCAL ER knl_rel_blk( MPLCB *mplcb, void *blk )
{
	QUEUE	*aq;
	IMACB*	imacb = (IMACB*)&(mplcb->mplsz);

	aq = (blk == mplcb->mempool) ? &(mplcb->areaque) : (QUEUE*)blk - 1;

#if CHK_PAR
	if ( !chkAreaFlag(aq, AREA_USE) ) {
		return E_PAR;
	}
#endif
	clrAreaFlag(aq, AREA_USE);

	if ( !chkAreaFlag(aq->next, AREA_USE) ) {
		/* 次の領域と併合 */
		knl_removeFreeQue(aq->next + 1);
		knl_removeAreaQue(aq->next);
	}
	if ( !chkAreaFlag(aq->prev, AREA_USE) ) {
		/* 前の領域と併合 */
		QUEUE *fq;
		aq = aq->prev;
		fq = (aq == &(mplcb->areaque)) ? (QUEUE*)(mplcb->mempool) : aq + 1;

		knl_removeFreeQue(fq);
		knl_removeAreaQue(aq->next);
	}

	/* 空き領域を FreeQue に登録 */
	if ( aq == &(mplcb->areaque) || aq->next == &(mplcb->areaque_end) ) {
		knl_appendFreeAreaBound(mplcb, aq);
	} else {
		knl_appendFreeArea(imacb, aq);
	}

	return E_OK;
}

/* ------------------------------------------------------------------------ */

/**
 * @brief 待ちタスクへのメモリブロック割り当て
 *
 * 待ちキューの先頭タスクから順に、要求サイズを満たす空きがある限り
 * メモリブロックを割り当てて待ち解除します。
 *
 * @param mplcb	メモリプール管理ブロック
 */
EXPORT void knl_mpl_wakeup( MPLCB *mplcb )
{
	TCB	*top;
	void	*blk;
	W	blksz;

	while ( !isQueEmpty(&mplcb->wait_queue) ) {
		top = (TCB*)mplcb->wait_queue.next;
		blksz = top->winfo.mpl.blksz;

		/* 空き領域の確認 */
		if ( blksz > knl_MaxFreeSize(mplcb) ) {
			break;
		}

		/* メモリブロックの獲得 */
		blk = knl_get_blk(mplcb, blksz);
		*top->winfo.mpl.p_blk = blk;

		/* タスクの待ち解除 */
		knl_wait_release_ok(top);
	}
}


/**
 * @brief メモリプールの初期設定
 *
 * AreaQue と FreeQue を初期化し、プール全体を 1 つの空き領域として
 * 登録します。
 *
 * @param mplcb	メモリプール管理ブロック
 */
LOCAL void init_mempool( MPLCB *mplcb )
{
	QueInit(&mplcb->areaque);
	QueInit(&mplcb->freeque);

	/* AreaQue へ登録 */
	knl_insertAreaQue(&mplcb->areaque, &mplcb->areaque_end);

	/* 空き領域にしてはならない箇所に AREA_USE を設定 */
	setAreaFlag(&mplcb->areaque_end, AREA_USE);

	/* FreeQue へ登録 */
	knl_appendFreeAreaBound(mplcb, &mplcb->areaque);
}

/**
 * @brief 可変長メモリプールの生成
 *
 * 生成情報 pk_cmpl に従って可変長メモリプールを生成します。
 * TA_USERBUF 属性を指定した場合は pk_cmpl->bufptr のユーザバッファを
 * プール領域として使用し、指定しない場合はカーネルが領域を確保します。
 *
 * @param pk_cmpl	メモリプール生成情報パケット
 * @return 正の値: 生成したメモリプール ID、負の値: エラーコード
 * @retval E_PAR	パラメータ不正（TA_USERBUF 指定時にプールサイズが
 *			sizeof(QUEUE) の倍数でない等）
 * @retval E_NOMEM	プール領域のメモリ確保失敗
 * @retval E_LIMIT	管理ブロックが不足（プール数の上限超過）
 */
SYSCALL ID tk_cre_mpl( CONST T_CMPL *pk_cmpl )
{
#if CHK_RSATR
	const ATR VALID_MPLATR = {
		 TA_TPRI
		|TA_RNG3
		|TA_USERBUF
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
	};
#endif
	MPLCB	*mplcb;
	ID	mplid;
	W	mplsz;
	void	*mempool;
	ER	ercd;

	CHECK_RSATR(pk_cmpl->mplatr, VALID_MPLATR);
	CHECK_PAR(pk_cmpl->mplsz > 0 && pk_cmpl->mplsz <= MAX_ALLOCATE);
#if !USE_IMALLOC
	/* Imalloc なしの構成では TA_USERBUF の指定が必須 */
	CHECK_PAR((pk_cmpl->mplatr & TA_USERBUF) != 0);
#endif
	CHECK_DISPATCH();

	mplsz = roundSize(pk_cmpl->mplsz);

#if USE_IMALLOC
	if ( (pk_cmpl->mplatr & TA_USERBUF) != 0 ) {
		/* ユーザバッファのサイズは sizeof(QUEUE) の倍数で、
			かつ sizeof(QUEUE)*2 より大きいこと */
		if ( mplsz != pk_cmpl->mplsz ) {
			return E_PAR;
		}
		/* ユーザバッファを使用 */
		mempool = pk_cmpl->bufptr;
	} else {
		/* メモリプール領域を確保 */
		mempool = knl_Imalloc((UW)mplsz);
		if ( mempool == NULL ) {
			return E_NOMEM;
		}
	}
#else
	/* ユーザバッファのサイズは sizeof(QUEUE) の倍数で、
		かつ sizeof(QUEUE)*2 より大きいこと */
	if ( mplsz != pk_cmpl->mplsz ) {
		return E_PAR;
	}
	/* ユーザバッファを使用 */
	mempool = pk_cmpl->bufptr;
#endif

	BEGIN_CRITICAL_SECTION;
	/* FreeQue から管理ブロックを取得 */
	mplcb = (MPLCB*)QueRemoveNext(&knl_free_mplcb);
	if ( mplcb == NULL ) {
		ercd = E_LIMIT;
	} else {
		mplid = ID_MPL(mplcb - knl_mplcb_table);

		/* 管理ブロックの初期化 */
		QueInit(&mplcb->wait_queue);
		mplcb->mplid  = mplid;
		mplcb->exinf  = pk_cmpl->exinf;
		mplcb->mplatr = pk_cmpl->mplatr;
		mplcb->mplsz  = mplsz;
#if USE_OBJECT_NAME
		if ( (pk_cmpl->mplatr & TA_DSNAME) != 0 ) {
			knl_strncpy((char*)mplcb->name, (char*)pk_cmpl->dsname, OBJECT_NAME_LENGTH);
		}
#endif

		mplcb->mempool = mempool;

		/* メモリプールの初期化 */
		init_mempool(mplcb);

		ercd = mplid;
	}
	END_CRITICAL_SECTION;

#if USE_IMALLOC
	if ( (ercd < E_OK) && ((pk_cmpl->mplatr & TA_USERBUF) == 0) ) {
		knl_Ifree(mempool);
	}
#endif

	return ercd;
}

#ifdef USE_FUNC_TK_DEL_MPL
/**
 * @brief 可変長メモリプールの削除
 *
 * 指定したメモリプールを削除します。ブロック獲得待ちのタスクは
 * E_DLT で待ち解除されます。カーネルが確保したプール領域
 * （TA_USERBUF なし）は解放されます。
 *
 * @param mplid	メモリプール ID
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象メモリプールが存在しない
 */
SYSCALL ER tk_del_mpl( ID mplid )
{
	MPLCB	*mplcb;
	void	*mempool = NULL;
	ATR	memattr = 0;
	ER	ercd = E_OK;

	CHECK_MPLID(mplid);
	CHECK_DISPATCH();

	mplcb = get_mplcb(mplid);

	BEGIN_CRITICAL_SECTION;
	if ( mplcb->mplid == 0 ) {
		ercd = E_NOEXS;
	} else {
		mempool = mplcb->mempool;
		memattr = mplcb->mplatr;

		/* 待ちタスクの待ち解除（E_DLT） */
		knl_wait_delete(&mplcb->wait_queue);

		/* 管理ブロックを FreeQue へ返却 */
		QueInsert(&mplcb->wait_queue, &knl_free_mplcb);
		mplcb->mplid = 0;
	}
	END_CRITICAL_SECTION;

#if USE_IMALLOC
	if ( (ercd == E_OK) && ((memattr & TA_USERBUF) == 0) ) {
		knl_Ifree(mempool);
	}
#endif

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_MPL */

/**
 * @brief 待ちタスクの優先度変更時の処理
 *
 * 待ちキュー内の位置を新しい優先度に従って並べ替えたうえで、
 * 新しい先頭タスクから割り当て可能なメモリブロックを割り当てます。
 *
 * @param tcb	優先度が変更されたタスクの TCB
 * @param oldpri	変更前の優先度（負値なら並べ替えを省略）
 * @note 割り込み禁止状態で実行する必要があります。
 */
LOCAL void mpl_chg_pri( TCB *tcb, INT oldpri )
{
	MPLCB	*mplcb;

	mplcb = get_mplcb(tcb->wid);
	if ( oldpri >= 0 ) {
		/* 待ちキューの並べ替え */
		knl_gcb_change_priority((GCB*)mplcb, tcb);
	}

	/* 待ちキューの新しい先頭タスクから、割り当て可能な限り
	   メモリブロック待ちを解除する */
	knl_mpl_wakeup(mplcb);
}

/**
 * @brief 待ちタスクの待ち解除時の処理
 *
 * タスクが待ちから外れた後、残りの待ちタスクへ割り当て可能な
 * メモリブロックを割り当てます。
 *
 * @param tcb	待ち解除されたタスクの TCB
 */
LOCAL void mpl_rel_wai( TCB *tcb )
{
	mpl_chg_pri(tcb, -1);
}

/*
 * 可変長メモリプール待ち仕様の定義
 */
LOCAL CONST WSPEC knl_wspec_mpl_tfifo = { TTW_MPL, NULL,        mpl_rel_wai };
LOCAL CONST WSPEC knl_wspec_mpl_tpri  = { TTW_MPL, mpl_chg_pri, mpl_rel_wai };

/**
 * @brief 可変長メモリブロックの獲得
 *
 * メモリプールから blksz バイト以上のブロックを獲得します。
 * 自タスクより先の待ちタスクがいる場合や空きが不足している場合、
 * タスクは待ち状態となります。
 *
 * @param mplid	メモリプール ID
 * @param blksz	要求ブロックサイズ（バイト数）
 * @param p_blk	獲得したブロックの先頭アドレスの格納先
 * @param tmout	タイムアウト時間（TMO_POL / TMO_FEVR 指定可）
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象メモリプールが存在しない
 * @retval E_PAR	blksz がプールサイズを超えている等
 * @retval E_TMOUT	タイムアウト
 * @retval E_RLWAI	待ち状態の強制解除
 * @retval E_DLT	待ちの間にメモリプールが削除された
 */
SYSCALL ER tk_get_mpl( ID mplid, SZ blksz, void **p_blk, TMO tmout )
{
	MPLCB	*mplcb;
	void	*blk = NULL;
	ER	ercd = E_OK;

	CHECK_MPLID(mplid);
	CHECK_PAR(blksz > 0 && blksz <= MAX_ALLOCATE);
	CHECK_TMOUT(tmout);
	CHECK_DISPATCH();

	mplcb = get_mplcb(mplid);
	blksz = roundSize(blksz);

	BEGIN_CRITICAL_SECTION;
	if ( mplcb->mplid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}

#if CHK_PAR
	if ( blksz > mplcb->mplsz ) {
		ercd = E_PAR;
		goto error_exit;
	}
#endif

	if ( knl_gcb_top_of_wait_queue((GCB*)mplcb, knl_ctxtsk) == knl_ctxtsk
	  && (blk = knl_get_blk(mplcb, blksz)) != NULL ) {
		/* メモリブロックの獲得 */
		*p_blk = blk;
	} else {
		/* 待ち状態への移行 */
		knl_ctxtsk->wspec = ( (mplcb->mplatr & TA_TPRI) != 0 )?
					&knl_wspec_mpl_tpri: &knl_wspec_mpl_tfifo;
		knl_ctxtsk->wercd = &ercd;
		knl_ctxtsk->winfo.mpl.blksz = blksz;
		knl_ctxtsk->winfo.mpl.p_blk = p_blk;
		knl_gcb_make_wait((GCB*)mplcb, tmout);
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

/**
 * @brief 可変長メモリブロックの返却
 *
 * 獲得したブロックをメモリプールへ返却し、待ちタスクがあれば
 * 割り当て可能な限りメモリブロックを割り当てて待ち解除します。
 *
 * @param mplid	メモリプール ID
 * @param blk	返却するメモリブロックの先頭アドレス
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象メモリプールが存在しない
 * @retval E_PAR	blk がプール範囲外、または使用中ブロックではない
 */
SYSCALL ER tk_rel_mpl( ID mplid, void *blk )
{
	MPLCB	*mplcb;
	ER	ercd = E_OK;

	CHECK_MPLID(mplid);
	CHECK_DISPATCH();

	mplcb = get_mplcb(mplid);

	BEGIN_CRITICAL_SECTION;
	if ( mplcb->mplid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
#if CHK_PAR
	if ( (B*)blk < (B*)mplcb->mempool || (B*)blk > (B*)mplcb->mempool + mplcb->mplsz ) {
		ercd = E_PAR;
		goto error_exit;
	}
#endif

	/* メモリブロックの解放 */
	ercd = knl_rel_blk(mplcb, blk);
	if ( ercd < E_OK ) {
		goto error_exit;
	}

	/* 待ちタスクへメモリブロックを割り当てる */
	knl_mpl_wakeup(mplcb);

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_REF_MPL
/**
 * @brief 可変長メモリプールの状態参照
 *
 * 拡張情報、待ちタスクの有無、空き領域の合計サイズと最大サイズを
 * 取得します。
 *
 * @param mplid	メモリプール ID
 * @param pk_rmpl	状態情報の格納先
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象メモリプールが存在しない
 */
SYSCALL ER tk_ref_mpl( ID mplid, T_RMPL *pk_rmpl )
{
	MPLCB	*mplcb;
	QUEUE	*fq, *q;
	W	frsz, blksz;
	ER	ercd = E_OK;

	CHECK_MPLID(mplid);
	CHECK_DISPATCH();

	mplcb = get_mplcb(mplid);

	BEGIN_CRITICAL_SECTION;
	if ( mplcb->mplid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rmpl->exinf = mplcb->exinf;
		pk_rmpl->wtsk  = knl_wait_tskid(&mplcb->wait_queue);
		frsz = 0;
		for ( fq = mplcb->freeque.next; fq != &mplcb->freeque; fq = fq->next ) {
			blksz = FreeSize(fq);
			frsz += blksz;
			for ( q = (fq+1)->next; q != NULL; q = q->next ) {
				frsz += blksz;
			}
		}
		pk_rmpl->frsz  = frsz;
		pk_rmpl->maxsz = knl_MaxFreeSize(mplcb);
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REF_MPL */

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
EXPORT ER knl_memorypool_getname(ID id, UB **name)
{
	MPLCB	*mplcb;
	ER	ercd = E_OK;

	CHECK_MPLID(id);

	BEGIN_DISABLE_INTERRUPT;
	mplcb = get_mplcb(id);
	if ( mplcb->mplid == 0 ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (mplcb->mplatr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = mplcb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */

#ifdef USE_FUNC_TD_LST_MPL
/**
 * @brief 可変長メモリプール ID の一覧取得
 *
 * 使用中のメモリプールの ID を list に格納します。
 *
 * @param list	ID を格納する配列
 * @param nent	list に格納できる最大数
 * @return 使用中のメモリプール数（nent を超える場合も総数を返す）
 */
SYSCALL INT td_lst_mpl( ID list[], INT nent )
{
	MPLCB	*mplcb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_mplcb_table + NUM_MPLID;
	for ( mplcb = knl_mplcb_table; mplcb < end; mplcb++ ) {
		if ( mplcb->mplid == 0 ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = ID_MPL(mplcb - knl_mplcb_table);
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_MPL */

#ifdef USE_FUNC_TD_REF_MPL
/**
 * @brief 可変長メモリプールの状態参照（デバッガサポート）
 *
 * @param mplid	メモリプール ID
 * @param pk_rmpl	状態情報の格納先
 * @retval E_OK	正常終了
 * @retval E_NOEXS	対象メモリプールが存在しない
 */
SYSCALL ER td_ref_mpl( ID mplid, TD_RMPL *pk_rmpl )
{
	MPLCB	*mplcb;
	QUEUE	*fq, *q;
	W	frsz, blksz;
	ER	ercd = E_OK;

	CHECK_MPLID(mplid);

	mplcb = get_mplcb(mplid);

	BEGIN_DISABLE_INTERRUPT;
	if ( mplcb->mplid == 0 ) {
		ercd = E_NOEXS;
	} else {
		pk_rmpl->exinf = mplcb->exinf;
		pk_rmpl->wtsk  = knl_wait_tskid(&mplcb->wait_queue);
		frsz = 0;
		for ( fq = mplcb->freeque.next; fq != &mplcb->freeque; fq = fq->next ) {
			blksz = FreeSize(fq);
			frsz += blksz;
			for ( q = (fq+1)->next; q != NULL; q = q->next ) {
				frsz += blksz;
			}
		}
		pk_rmpl->frsz  = frsz;
		pk_rmpl->maxsz = knl_MaxFreeSize(mplcb);
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_MPL */

#ifdef USE_FUNC_TD_MPL_QUE
/**
 * @brief 可変長メモリプール待ちキューの参照
 *
 * ブロック獲得待ちタスクの ID を待ちキューの並び順に list へ
 * 格納します。
 *
 * @param mplid	メモリプール ID
 * @param list	タスク ID を格納する配列
 * @param nent	list に格納できる最大数
 * @return 待ちタスク数（nent を超える場合も総数）、または E_NOEXS
 */
SYSCALL INT td_mpl_que( ID mplid, ID list[], INT nent )
{
	MPLCB	*mplcb;
	QUEUE	*q;
	ER	ercd = E_OK;

	CHECK_MPLID(mplid);

	mplcb = get_mplcb(mplid);

	BEGIN_DISABLE_INTERRUPT;
	if ( mplcb->mplid == 0 ) {
		ercd = E_NOEXS;
	} else {
		INT n = 0;
		for ( q = mplcb->wait_queue.next; q != &mplcb->wait_queue; q = q->next ) {
			if ( n++ < nent ) {
				*list++ = ((TCB*)q)->tskid;
			}
		}
		ercd = n;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_MPL_QUE */

#endif /* USE_DBGSPT */
#endif /* USE_MEMORYPOOL */
