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
 * @file mempool.c
 * @brief 可変長メモリプール管理機能
 * 
 * T-Kernelの可変長メモリプール（Variable Size Memory Pool）の実装を提供する。
 * 可変長メモリプールは、異なるサイズのメモリブロックを動的に割り当て・解放する
 * ためのメモリ管理機構である。
 * 
 * 主な機能：
 * - メモリプールの作成・削除（tk_cre_mpl, tk_del_mpl）
 * - メモリブロックの取得・返却（tk_get_mpl, tk_rel_mpl）
 * - メモリプールの状態参照（tk_ref_mpl）
 * - 効率的なメモリ断片化の管理
 * - タスクの待ち状態管理（FIFO/優先度順）
 * 
 * メモリプールは内部的にフリーエリアキューとエリアキューを使用して
 * メモリブロックの効率的な管理を実現している。
 */

/** [BEGIN Common Definitions] */
#include "kernel.h"
#include "task.h"
#include "wait.h"
#include "check.h"
#include "memory.h"
#include "mempool.h"
/** [END Common Definitions] */

#if CFN_MAX_MPLID > 0


#ifdef USE_FUNC_MPLCB_TABLE
Noinit(EXPORT MPLCB knl_mplcb_table[NUM_MPLID]);	/* Variable size memory pool control block */
Noinit(EXPORT QUEUE knl_free_mplcb);	/* FreeQue */
#endif /* USE_FUNC_MPLCB_TABLE */


#ifdef USE_FUNC_MEMORYPOOL_INITIALIZE
/**
 * @brief 可変長メモリプール制御ブロック初期化
 * 
 * システム起動時に可変長メモリプール制御ブロックテーブルを初期化する。
 * 全ての制御ブロックをフリーキューに登録し、使用可能な状態にする。
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_SYS システムエラー（メモリプールID数が無効）
 * 
 * @note システム初期化時に一度だけ呼び出される
 * @note 割り込み禁止状態で呼び出される
 */
EXPORT ER knl_memorypool_initialize( void )
{
	MPLCB	*mplcb, *end;

	if ( NUM_MPLID < 1 ) {
		return E_SYS;
	}

	/* Register all control blocks onto FreeQue */
	QueInit(&knl_free_mplcb);
	end = knl_mplcb_table + NUM_MPLID;
	for ( mplcb = knl_mplcb_table; mplcb < end; mplcb++ ) {
		mplcb->mplid = 0;
		QueInsert(&mplcb->wait_queue, &knl_free_mplcb);
	}

	return E_OK;
}
#endif /* USE_FUNC_MEMORYPOOL_INITIALIZE */

/* ------------------------------------------------------------------------ */

#ifdef USE_FUNC_APPENDFREEAREABOUND
/**
 * @brief フリーエリアをフリーキューに登録（境界専用版）
 * 
 * メモリプールの境界（先頭・末尾）エリアとのマージを考慮した
 * 特殊なフリーエリア登録処理を行う。
 * 
 * @param mplcb メモリプール制御ブロックへのポインタ
 * @param aq エリアキューエントリへのポインタ
 * 
 * @note この関数は境界エリアでの断片化を効率的に管理するために使用される
 * @note フリーエリアのサイズ順でキューに挿入される
 */
EXPORT void knl_appendFreeAreaBound( MPLCB *mplcb, QUEUE *aq )
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

	/* Registration position search */
	/*  Search the free area whose size is equal to 'blksz',
	 *  or larger than 'blksz' but closest.
	 *  If it does not exist, return '&imacb->freeque'.
	 */
	fq = knl_searchFreeArea(imacb, size);

	/* Register */
	clrAreaFlag(aq, AREA_USE);
	if ( fq != &imacb->freeque && FreeSize(fq) == size ) {
		/* FreeQue Same size */
		(top + 1)->next = (fq + 1)->next;
		(fq  + 1)->next = top + 1;
		(top + 1)->prev = fq + 1;
		if( (top + 1)->next != NULL ) {
			(top + 1)->next->prev = top + 1;
		}
		top->next = NULL;
	} else {
		/* FreeQue Size order */
		QueInsert(top, fq);
		(top + 1)->next = NULL;
		(top + 1)->prev = (QUEUE*)size;
	}
}
#endif /* USE_FUNC_APPENDFREEAREABOUND */

#ifdef USE_FUNC_GET_BLK
/**
 * @brief メモリブロック取得
 * 
 * 指定されたサイズのメモリブロックをメモリプールから取得する。
 * フリーキューから適切なサイズのエリアを検索し、必要に応じて分割する。
 * 
 * @param mplcb メモリプール制御ブロックへのポインタ
 * @param blksz 取得するブロックサイズ（ROUNDSZ単位で調整済み）
 * 
 * @return void* 取得したメモリブロックへのポインタ
 * @retval NULL 適切なサイズのメモリブロックが見つからない
 * @retval その他 取得したメモリブロックのアドレス
 * 
 * @note blkszは最小断片サイズより大きく、ROUNDSZ単位で調整されている必要がある
 * @note 残りサイズが最小断片サイズ未満の場合は分割せずに全体を割り当てる
 */
EXPORT void *knl_get_blk( MPLCB *mplcb, W blksz )
{
	QUEUE	*q, *aq, *aq2;
	IMACB*	imacb = (IMACB*)&(mplcb->mplsz);

	/* Search FreeQue */
	q = knl_searchFreeArea(imacb, blksz);
	if ( q == &(imacb->freeque) ) {
		return NULL;
	}

	/* remove free area from FreeQue */
	knl_removeFreeQue(q);
	aq = ((void *)q == mplcb->mempool) ? &(mplcb->areaque) :  q - 1;

	/* If there is a fragment smaller than the minimum fragment size,
	   allocate them together */
	if ( FreeSize(q) - (UW)blksz >= MIN_FRAGMENT + sizeof(QUEUE) ) {

		/* Divide the area into 2. */
		aq2 = (QUEUE*)((VB*)q + blksz);
		knl_insertAreaQue(aq, aq2);

		/* Register the remaining area onto FreeQue */
		if ( aq2->next == &(mplcb->areaque_end) ) {
			knl_appendFreeAreaBound(mplcb, aq2);
		} else {
			knl_appendFreeArea(imacb, aq2);
		}
	}
	setAreaFlag(aq, AREA_USE);

	return (void *)q;
}
#endif /* USE_FUNC_GET_BLK */

#ifdef USE_FUNC_REL_BLK
/**
 * @brief メモリブロック解放
 * 
 * メモリプールに対してメモリブロックを解放し、隣接する
 * フリーエリアがあれば自動的にマージする。
 * 
 * @param mplcb メモリプール制御ブロックへのポインタ
 * @param blk 解放するメモリブロックへのポインタ
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_PAR パラメータエラー（既に解放済みのブロック等）
 * 
 * @note 解放されたエリアは前後の空きエリアと自動的にマージされる
 * @note マージによりメモリの断片化を防ぐ
 */
EXPORT ER knl_rel_blk( MPLCB *mplcb, void *blk )
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
		/* Merge to the next area */
		knl_removeFreeQue(aq->next + 1);
		knl_removeAreaQue(aq->next);
	}
	if ( !chkAreaFlag(aq->prev, AREA_USE) ) {
		/* Merge to the previous area */
		QUEUE *fq;
		aq = aq->prev;
		fq = (aq == &(mplcb->areaque)) ? (QUEUE*)(mplcb->mempool) : aq + 1;

		knl_removeFreeQue(fq);
		knl_removeAreaQue(aq->next);
	}

	/* Register free area onto FreeQue */
	if ( aq == &(mplcb->areaque) || aq->next == &(mplcb->areaque_end) ) {
		knl_appendFreeAreaBound(mplcb, aq);
	} else {
		knl_appendFreeArea(imacb, aq);
	}

	return E_OK;
}
#endif /* USE_FUNC_REL_BLK */

/* ------------------------------------------------------------------------ */

#ifdef USE_FUNC_MPL_WAKEUP
/**
 * @brief メモリプール待ちタスクの起床処理
 * 
 * メモリプールで待機中のタスクに対して、十分な空きメモリがある限り
 * メモリを割り当てて待ち状態を解除する。
 * 
 * @param mplcb メモリプール制御ブロックへのポインタ
 * 
 * @note 待ちキューの先頭から順番に処理される
 * @note 各タスクが要求するサイズのメモリが確保できる場合のみ起床させる
 * @note メモリブロック解放時やメモリプール操作後に呼び出される
 */
EXPORT void knl_mpl_wakeup( MPLCB *mplcb )
{
	TCB	*top;
	void	*blk;
	W	blksz;

	while ( !isQueEmpty(&mplcb->wait_queue) ) {
		top = (TCB*)mplcb->wait_queue.next;
		blksz = top->winfo.mpl.blksz;

		/* Check free space */
		if ( blksz > knl_MaxFreeSize(mplcb) ) {
			break;
		}

		/* Get memory block */
		blk = knl_get_blk(mplcb, blksz);
		*top->winfo.mpl.p_blk = blk;

		/* Release wait task */
		knl_wait_release_ok(top);
	}
}
#endif /* USE_FUNC_MPL_WAKEUP */


#ifdef USE_FUNC_TK_CRE_MPL
/**
 * @brief メモリプール初期設定
 * 
 * 新規作成されたメモリプールの内部構造を初期化する。
 * エリアキューとフリーキューを設定し、全領域を1つのフリーエリアとして登録する。
 * 
 * @param mplcb 初期化するメモリプール制御ブロックへのポインタ
 * 
 * @note この関数はメモリプール作成時に内部的に呼び出される
 */
LOCAL void init_mempool( MPLCB *mplcb )
{
	QueInit(&mplcb->areaque);
	QueInit(&mplcb->freeque);

	/* Register onto AreaQue */
	knl_insertAreaQue(&mplcb->areaque, &mplcb->areaque_end);

	/* Set AREA_USE for locations that must not be free area */
	setAreaFlag(&mplcb->areaque_end, AREA_USE);

	/* Register onto FreeQue */
	knl_appendFreeAreaBound(mplcb, &mplcb->areaque);
}

/**
 * @brief 可変長メモリプール生成
 * 
 * 指定されたパラメータに基づいて可変長メモリプールを生成する。
 * ユーザーバッファまたはシステムが確保したメモリを使用してメモリプールを構築する。
 * 
 * @param pk_cmpl メモリプール生成情報パケットへのポインタ
 * 
 * @return ID 生成されたメモリプールID
 * @retval 正の値 生成されたメモリプールのID
 * @retval E_RSATR 予約属性エラー
 * @retval E_PAR パラメータエラー（サイズが0以下等）
 * @retval E_NOMEM メモリ不足
 * @retval E_LIMIT メモリプール数の上限超過
 * 
 * @note TA_USERBUFが指定された場合はユーザー提供のバッファを使用
 * @note TA_TPRIが指定された場合は優先度順で待ちキューを管理
 */
SYSCALL ID tk_cre_mpl_impl( CONST T_CMPL *pk_cmpl )
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
	CHECK_PAR(pk_cmpl->mplsz > 0);
#if !USE_IMALLOC
	/* TA_USERBUF must be specified if configured in no Imalloc */
	CHECK_PAR((pk_cmpl->mplatr & TA_USERBUF) != 0);
#endif
	CHECK_DISPATCH();

	mplsz = roundSize(pk_cmpl->mplsz);

#if USE_IMALLOC
	if ( (pk_cmpl->mplatr & TA_USERBUF) != 0 ) {
		/* Size of user buffer must be multiples of sizeof(QUEUE)
			and larger than sizeof(QUEUE)*2 */
		if ( mplsz != pk_cmpl->mplsz ) {
			return E_PAR;
		}
		/* Use user buffer */
		mempool = pk_cmpl->bufptr;
	} else {
		/* Allocate memory for memory pool */
		mempool = knl_Imalloc((UW)mplsz);
		if ( mempool == NULL ) {
			return E_NOMEM;
		}
	}
#else
	/* Size of user buffer must be multiples of sizeof(QUEUE)
		and larger than sizeof(QUEUE)*2 */
	if ( mplsz != pk_cmpl->mplsz ) {
		return E_PAR;
	}
	/* Use user buffer */
	mempool = pk_cmpl->bufptr;
#endif

	BEGIN_CRITICAL_SECTION;
	/* Get control block from FreeQue */
	mplcb = (MPLCB*)QueRemoveNext(&knl_free_mplcb);
	if ( mplcb == NULL ) {
		ercd = E_LIMIT;
	} else {
		mplid = ID_MPL(mplcb - knl_mplcb_table);

		/* Initialize control block */
		QueInit(&mplcb->wait_queue);
		mplcb->mplid  = mplid;
		mplcb->exinf  = pk_cmpl->exinf;
		mplcb->mplatr = pk_cmpl->mplatr;
		mplcb->mplsz  = mplsz;
#if USE_OBJECT_NAME
		if ( (pk_cmpl->mplatr & TA_DSNAME) != 0 ) {
			strncpy((char*)mplcb->name, (char*)pk_cmpl->dsname, OBJECT_NAME_LENGTH);
		}
#endif

		mplcb->mempool = mempool;

		/* Initialize memory pool */
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
#endif /* USE_FUNC_TK_CRE_MPL */

#ifdef USE_FUNC_TK_DEL_MPL
/**
 * @brief 可変長メモリプール削除
 * 
 * 指定されたメモリプールを削除し、関連リソースを解放する。
 * 待機中のタスクがある場合は全て起床させる（E_DLTエラーで）。
 * 
 * @param mplid 削除するメモリプールのID
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正ID
 * @retval E_NOEXS オブジェクト未生成
 * 
 * @note TA_USERBUFでない場合はシステムが確保したメモリも解放される
 * @note 削除後、該当IDのメモリプールは使用できなくなる
 */
SYSCALL ER tk_del_mpl_impl( ID mplid )
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

		/* Free wait state of task (E_DLT) */
		knl_wait_delete(&mplcb->wait_queue);

		/* Return to FreeQue */
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

#ifdef USE_FUNC_TK_GET_MPL
/**
 * @brief 待ちタスクの優先度変更時処理
 * 
 * メモリプールで待機中のタスクの優先度が変更された場合の処理を行う。
 * 優先度順の待ちキューを再構築し、新しい優先度に基づいてメモリ割り当てを試行する。
 * 
 * @param tcb 優先度が変更されたタスクの制御ブロック
 * @param oldpri 変更前の優先度（負の値の場合は新規待ち）
 * 
 * @note 割り込み禁止状態で実行する必要がある
 * @note 優先度変更により新たにメモリ割り当て可能なタスクが生じる可能性がある
 */
LOCAL void mpl_chg_pri( TCB *tcb, INT oldpri )
{
	MPLCB	*mplcb;

	mplcb = get_mplcb(tcb->wid);
	if ( oldpri >= 0 ) {
		/* Reorder wait line */
		knl_gcb_change_priority((GCB*)mplcb, tcb);
	}

	/* From the new top task of a wait queue, free the assign
	   wait of memory blocks as much as possible. */
	knl_mpl_wakeup(mplcb);
}

/**
 * @brief 待ちタスク解放時処理
 * 
 * メモリプールで待機中のタスクが解放される際の処理を行う。
 * 
 * @param tcb 解放されるタスクの制御ブロック
 * 
 * @note 内部的にmpl_chg_pri()を呼び出して処理を委譲する
 */
LOCAL void mpl_rel_wai( TCB *tcb )
{
	mpl_chg_pri(tcb, -1);
}

/*
 * Definition of variable size memory pool wait specification
 */
LOCAL CONST WSPEC knl_wspec_mpl_tfifo = { TTW_MPL, NULL,        mpl_rel_wai };
LOCAL CONST WSPEC knl_wspec_mpl_tpri  = { TTW_MPL, mpl_chg_pri, mpl_rel_wai };

/**
 * @brief 可変長メモリブロック取得
 * 
 * 指定されたメモリプールから指定サイズのメモリブロックを取得する。
 * 適切なサイズのブロックがない場合は指定されたタイムアウト時間まで待機する。
 * 
 * @param mplid メモリプールID
 * @param blksz 取得するメモリブロックサイズ
 * @param p_blk 取得したメモリブロックのアドレスを格納する変数へのポインタ
 * @param tmout タイムアウト時間（TMO_POL:ポーリング、TMO_FEVR:永久待ち）
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正ID
 * @retval E_PAR パラメータエラー（サイズが0以下またはプールサイズ超過等）
 * @retval E_NOEXS オブジェクト未生成
 * @retval E_TMOUT タイムアウト発生
 * @retval E_RLWAI 待ち状態の強制解除
 * 
 * @note 取得されるメモリブロックのアドレスは*p_blkに格納される
 * @note メモリプールの属性によりFIFO順または優先度順で待機する
 */
SYSCALL ER tk_get_mpl_impl( ID mplid, SZ blksz, void **p_blk, TMO tmout )
{
	MPLCB	*mplcb;
	void	*blk = NULL;
	ER	ercd = E_OK;

	CHECK_MPLID(mplid);
	CHECK_PAR(blksz > 0);
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
		/* Get memory block */
		*p_blk = blk;
	} else {
		/* Ready for wait */
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
#endif /* USE_FUNC_TK_GET_MPL */

#ifdef USE_FUNC_TK_REL_MPL
/**
 * @brief 可変長メモリブロック返却
 * 
 * 指定されたメモリプールに対してメモリブロックを返却する。
 * 返却されたメモリは他のタスクから再利用可能になる。
 * 
 * @param mplid メモリプールID
 * @param blk 返却するメモリブロックへのポインタ
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正ID
 * @retval E_PAR パラメータエラー（不正なメモリアドレス等）
 * @retval E_NOEXS オブジェクト未生成
 * 
 * @note 返却されたメモリは隣接する空き領域と自動的にマージされる
 * @note 返却により待機中のタスクが起床する可能性がある
 */
SYSCALL ER tk_rel_mpl_impl( ID mplid, void *blk )
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

	/* Free memory block */
	ercd = knl_rel_blk(mplcb, blk);
	if ( ercd < E_OK ) {
		goto error_exit;
	}

	/* Assign memory block to waiting task */
	knl_mpl_wakeup(mplcb);

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REL_MPL */

#ifdef USE_FUNC_TK_REF_MPL
/**
 * @brief 可変長メモリプール状態参照
 * 
 * 指定されたメモリプールの現在の状態情報を取得する。
 * 空きメモリサイズ、最大連続空きサイズ、待機タスク数等の情報を提供する。
 * 
 * @param mplid メモリプールID
 * @param pk_rmpl メモリプール状態情報を格納するパケットへのポインタ
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_ID 不正ID
 * @retval E_NOEXS オブジェクト未生成
 * 
 * @note 返される情報には待機タスクID、総空きサイズ、最大連続空きサイズが含まれる
 * @note 状態参照時点でのスナップショット情報である
 */
SYSCALL ER tk_ref_mpl_impl( ID mplid, T_RMPL *pk_rmpl )
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
 *	Debugger support function
 */
#if USE_DBGSPT

#ifdef USE_FUNC_MEMORYPOOL_GETNAME
#if USE_OBJECT_NAME
/*
 * Get object name from control block
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
#endif /* USE_FUNC_FIX_MEMORYPOOL_GETNAME */

#ifdef USE_FUNC_TD_LST_MPL
/*
 * Refer variable size memory pool usage state
 */
SYSCALL INT td_lst_mpl_impl( ID list[], INT nent )
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
/*
 * Refer variable size memory pool state
 */
SYSCALL ER td_ref_mpl_impl( ID mplid, TD_RMPL *pk_rmpl )
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
/*
 * Refer variable size memory pool wait queue 
 */
SYSCALL INT td_mpl_que_impl( ID mplid, ID list[], INT nent )
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
#endif /* CFN_MAX_MPLID */
