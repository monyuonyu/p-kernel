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
 * @file memory.c
 * @brief 内部メモリ管理機能
 * 
 * T-Kernelの内部メモリアロケータ（Imalloc）を実装します。
 * カーネル内部で使用するメモリの動的確保・解放を効率的に管理します。
 * 
 * 主な機能：
 * - 可変長メモリブロックの確保・解放
 * - フリーエリアの効率的な管理
 * - フリーエリアの統合とフラグメンテーション対策
 * - サイズ別の最適化されたサーチアルゴリズム
 * 
 * メモリ管理の特徴：
 * - 二重リンクリストによるエリア管理
 * - サイズ順とポジション順の複合キュー構造
 * - 最小フラグメントサイズの保証
 * - 割り込み禁止による排他制御
 * 
 * @note この機能はT-Kernelのカーネル内部専用です
 * @note アプリケーションタスクは標準ライブラリのmalloc/freeを使用してください
 */

/** [BEGIN Common Definitions] */
#include "kernel.h"
#include <imalloc.h>
#include "memory.h"
/** [END Common Definitions] */


#ifdef USE_FUNC_SEARCHFREEAREA
/**
 * @brief フリーエリアの検索
 * 
 * 指定されたサイズ以上の空きエリアを効率的に検索します。
 * 要求サイズと等しいか、要求サイズより大きいが最も近いサイズの
 * 空きエリアを見つけて返します。
 * 
 * @param imacb 内部メモリ管理制御ブロック
 * @param blksz 要求されるブロックサイズ
 * @return QUEUE* 見つかった空きエリアのキューポインタ
 *                 見つからない場合は &imacb->freeque を返す
 * 
 * 検索アルゴリズム：
 * - 要求サイズがメモリプール全体の1/4より大きい場合：大きいサイズから検索
 * - 要求サイズがメモリプール全体の1/4以下の場合：小さいサイズから検索
 * 
 * @note この最適化により、大きな要求には大きなブロックを優先的に割り当て、
 *       小さな要求にはピッタリサイズのブロックを優先的に割り当てます
 * @note フラグメンテーションを抑制し、メモリ利用効率を向上させます
 */
EXPORT QUEUE* knl_searchFreeArea( IMACB *imacb, W blksz )
{
	QUEUE	*q = &imacb->freeque;

	/* For area whose memory pool size is less than 1/4,
	   search from smaller size.
	   Otherwise, search from larger size. */
	if ( blksz > imacb->memsz / 4 ) {
		/* Search from larger size. */
		W fsz = 0;
		while ( (q = q->prev) != &imacb->freeque ) {
			fsz = FreeSize(q);
			if ( fsz <= blksz ) {
				return ( fsz < blksz )? q->next: q;
			}
		}
		return ( fsz >= blksz )? q->next: q;
	} else {
		/* Search from smaller size. */
		while ( (q = q->next) != &imacb->freeque ) {
			if ( FreeSize(q) >= blksz ) {
				break;
			}
		}
		return q;
	}
}
#endif /* USE_FUNC_SEARCHFREEAREA */


#ifdef USE_FUNC_APPENDFREEAREA
/**
 * @brief フリーエリアをフリーキューに登録
 * 
 * 解放されたメモリエリアをフリーキューに登録します。
 * フリーキューは2つのタイプで構成されます：
 * 1. 異なるサイズのエリアをサイズ順にリンクするキュー
 * 2. 同じサイズのエリアをリンクするキュー
 * 
 * キュー構造：
 * ```
 * freeque
 * |
 * |   +-----------------------+       +-----------------------+
 * |   | AreaQue               |       | AreaQue               |
 * |   +-----------------------+       +-----------------------+
 * *---> FreeQue Size order    |       | EmptyQue              |
 * |   | FreeQue Same size   --------->| FreeQue Same size   ----->
 * |   |                       |       |                       |
 * |   |                       |       |                       |
 * |   +-----------------------+       +-----------------------+
 * |   | AreaQue               |       | AreaQue               |
 * v   +-----------------------+       +-----------------------+
 * ```
 * 
 * @param imacb 内部メモリ管理制御ブロック
 * @param aq 登録するエリアのキューポインタ
 * 
 * @note 同じサイズのエリアが既に存在する場合は、同一サイズキューに追加されます
 * @note エリアは使用フラグがクリアされ、フリーエリアとしてマークされます
 */
EXPORT void knl_appendFreeArea( IMACB *imacb, QUEUE *aq )
{
	QUEUE	*fq;
	W	size = AreaSize(aq);

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
		(aq + 2)->next = (fq + 1)->next;
		(fq + 1)->next = aq + 2;
		(aq + 2)->prev = fq + 1;
		if( (aq + 2)->next != NULL ) {
			(aq + 2)->next->prev = aq + 2;
		}
		(aq + 1)->next = NULL;
	} else {
		/* FreeQue Size order */
		QueInsert(aq + 1, fq);
		(aq + 2)->next = NULL;
		(aq + 2)->prev = (QUEUE*)size;
	}
}
#endif /* USE_FUNC_APPENDFREEAREA */

#ifdef USE_FUNC_REMOVEFREEQUE
/**
 * @brief フリーキューからの削除
 * 
 * 指定されたフリーエリアをフリーキューから削除します。
 * サイズ順キューと同一サイズキューの両方に対応しています。
 * 
 * @param fq 削除するフリーエリアのキューポインタ
 * 
 * 削除処理：
 * - 同一サイズキューの場合：同一サイズリンクから削除
 * - サイズ順キューの場合：サイズ順リンクから削除し、同一サイズリンクを再構成
 * 
 * @note キューの構造を保持しながら適切に削除します
 */
EXPORT void knl_removeFreeQue( QUEUE *fq )
{
	if ( fq->next == NULL ) {	/* FreeQue Same size */
		(fq + 1)->prev->next = (fq + 1)->next;
		if ( (fq + 1)->next != NULL ) {
			(fq + 1)->next->prev = (fq + 1)->prev;
		}
	} else {			/* FreeQue Size order */
		if ( (fq + 1)->next != NULL ) {		/* having FreeQue Same size */
			QueInsert((fq + 1)->next - 1, fq);
			(fq + 1)->next->prev = (fq + 1)->prev;
		}
		QueRemove(fq);
	}
}
#endif /* USE_FUNC_REMOVEFREEQUE */

#ifdef USE_FUNC_INSERTAREAQUE
/**
 * @brief エリアの登録
 * 
 * 指定されたエリアをエリアキューに挿入します。
 * 'ent' を 'que' の直後に挿入します。
 * 
 * @param que 挿入位置の基準となるキューエントリ
 * @param ent 挿入するエリアのキューエントリ
 * 
 * @note エリアキューはメモリアドレス順に管理されます
 * @note リンクリストの整合性を保持します
 */
EXPORT void knl_insertAreaQue( QUEUE *que, QUEUE *ent )
{
	ent->prev = que;
	ent->next = que->next;
	Assign(que->next->prev, ent);
	que->next = ent;
}
#endif /* USE_FUNC_INSERTAREAQUE */

#ifdef USE_FUNC_REMOVEAREAQUE
/**
 * @brief エリアの削除
 * 
 * 指定されたエリアをエリアキューから削除します。
 * リンクリストの連結を適切に更新します。
 * 
 * @param aq 削除するエリアのキューポインタ
 * 
 * @note エリアキューのポインタマスキング機能を使用します
 * @note 削除後もリンクリストの整合性を保持します
 */
EXPORT void knl_removeAreaQue( QUEUE *aq )
{
	Mask(aq->prev)->next = aq->next;
	Assign(aq->next->prev, Mask(aq->prev));
}
#endif /* USE_FUNC_REMOVEAREAQUE */

/* ------------------------------------------------------------------------ */

#if USE_IMALLOC


/* ------------------------------------------------------------------------ */

#ifdef USE_FUNC_IMACB
Noinit(EXPORT IMACB *knl_imacb);
#endif /* USE_FUNC_IMACB */

/* ------------------------------------------------------------------------ */

#ifdef USE_FUNC_IMALLOC
/**
 * @brief メモリの取得（内部用malloc）
 * 
 * 指定されたサイズのメモリブロックを確保します。
 * カーネル内部で使用するメモリアロケータです。
 * 
 * @param size 確保するメモリサイズ（バイト数）
 * @return void* 確保したメモリアドレス（NULL = 失敗）
 * 
 * 動作仕様：
 * - 最小フラグメントサイズ未満の場合は最小サイズに切り上げ
 * - サイズは適切なアライメントに丸められる
 * - 余った領域が最小フラグメントサイズ以上なら分割してフリーエリアに戻す
 * 
 * @note 割り込み禁止による排他制御を行います
 * @note メモリ不足の場合はNULLを返します
 */
EXPORT void* knl_Imalloc( size_t size )
{
	QUEUE	*q, *aq, *aq2;
	UINT	imask;

	/* If it is smaller than the minimum fragment size,
	   allocate the minimum size to it. */
	if ( size < MIN_FRAGMENT ) {
		size = MIN_FRAGMENT;
	} else {
		size = ROUND(size);
	}

	DI(imask);  /* Exclusive control by interrupt disable */

	/* Search FreeQue */
	q = knl_searchFreeArea(knl_imacb, size);
	if ( q == &(knl_imacb->freeque) ) {
		q = NULL; /* Insufficient memory */
		goto err_ret;
	}

	/* There is free area: Split from FreeQue once */
	knl_removeFreeQue(q);

	aq = q - 1;

	/* If there are fragments smaller than the minimum fragment size,
	   allocate them also */
	if ( FreeSize(q) - size >= MIN_FRAGMENT + sizeof(QUEUE) ) {

		/* Divide area into 2 */
		aq2 = (QUEUE*)((VB*)(aq + 1) + size);
		knl_insertAreaQue(aq, aq2);

		/* Register remaining area to FreeQue */
		knl_appendFreeArea(knl_imacb, aq2);
	}
	setAreaFlag(aq, AREA_USE);

err_ret:
	EI(imask);

	return (void *)q;
}
#endif /* USE_FUNC_IMALLOC */

#ifdef USE_FUNC_ICALLOC
/**
 * @brief メモリの取得（ゼロクリア版）
 * 
 * 指定されたサイズのメモリブロックを確保し、
 * 全てのバイトを0で初期化します。
 * 標準ライブラリのcalloc相当の機能です。
 * 
 * @param nmemb 要素数
 * @param size 各要素のサイズ（バイト数）
 * @return void* 確保したメモリアドレス（NULL = 失敗）
 * 
 * @note 合計サイズは nmemb * size で計算されます
 * @note メモリの確保に失敗した場合はNULLを返します
 * @note 確保後のメモリは全てゼロで初期化されます
 */
EXPORT void* knl_Icalloc( size_t nmemb, size_t size )
{
	size_t	sz = nmemb * size;
	void	*mem;

	mem = knl_Imalloc(sz);
	if ( mem == NULL ) {
		return NULL;
	}

	memset(mem, 0, sz);

	return mem;
}
#endif /* USE_FUNC_ICALLOC */

#ifdef USE_FUNC_IFREE
/**
 * @brief メモリの解放（内部用free）
 * 
 * 以前にknl_Imallocまたはknl_Icallocで確保したメモリを解放します。
 * 隣接するフリーエリアと自動的に結合してフラグメンテーションを防いでいます。
 * 
 * @param ptr 解放するメモリブロックのポインタ
 * 
 * 解放処理：
 * 1. 後続のフリーエリアと結合
 * 2. 前方のフリーエリアと結合
 * 3. 結合されたエリアをフリーキューに登録
 * 
 * @note 割り込み禁止中に呼び出される場合があります
 * @note 割り込み禁止による排他制御を行います
 * @note NULLポインタの指定は未定義動作です
 */
EXPORT void  knl_Ifree( void *ptr )
{
	QUEUE	*aq;
	UINT	imask;

	DI(imask);  /* Exclusive control by interrupt disable */

	aq = (QUEUE*)ptr - 1;
	clrAreaFlag(aq, AREA_USE);

	if ( !chkAreaFlag(aq->next, AREA_USE) ) {
		/* Merge with free area in after location */
		knl_removeFreeQue(aq->next + 1);
		knl_removeAreaQue(aq->next);
	}

	if ( !chkAreaFlag(aq->prev, AREA_USE) ) {
		/* Merge with free area in front location */
		aq = aq->prev;
		knl_removeFreeQue(aq + 1);
		knl_removeAreaQue(aq->next);
	}

	knl_appendFreeArea(knl_imacb, aq);

	EI(imask);
}
#endif /* USE_FUNC_IFREE */

/* ------------------------------------------------------------------------ */

#ifdef USE_FUNC_INIT_IMALLOC
/**
 * @brief IMACBの初期化
 * 
 * 内部メモリ管理制御ブロックを初期化します。
 * エリアキューとフリーキューを初期状態に設定します。
 * 
 * @note この関数は内部関数で、初期化時のみ使用されます
 */
LOCAL void initIMACB( void )
{
	QueInit(&(knl_imacb->areaque));
	QueInit(&(knl_imacb->freeque));
}

/**
 * @brief Imallocの初期設定
 * 
 * 内部メモリ管理機構を初期化し、使用可能な状態にします。
 * システム起動時に一度だけ呼び出されます。
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * 
 * 初期化処理：
 * 1. メモリ領域の範囲を確定
 * 2. IMACBを適切なアライメントで配置
 * 3. メモリプールの初期設定
 * 4. エリアキューとフリーキューの初期化
 * 5. 初期フリーエリアの登録
 * 
 * @note knl_lowmem_top と knl_lowmem_limit の間の領域を使用します
 * @note メモリアライメントは8バイト単位で行われます
 */
EXPORT ER knl_init_Imalloc( void )
{
/* Low-level memory management information */
IMPORT	void	*knl_lowmem_top, *knl_lowmem_limit;

	void	*memend;
	QUEUE	*top, *end;

	/* Acquire system configuration definition information */
	memend = CFN_REALMEMEND;
	if ( (UW)memend > (UW)knl_lowmem_limit ) {
		memend = knl_lowmem_limit;
	}

	/* Align top with 4 byte unit alignment for IMACB */
	knl_lowmem_top = (void *)(((UW)knl_lowmem_top + 3) & ~0x00000003UL);
	knl_imacb = (IMACB*)knl_lowmem_top;
	knl_lowmem_top = (void *)((UW)knl_lowmem_top + sizeof(IMACB));

	/* Align top with 8 byte unit alignment */
	knl_lowmem_top = (void *)(((UW)knl_lowmem_top + 7) & ~0x00000007UL);
	top = (QUEUE*)knl_lowmem_top;
	knl_imacb->memsz = (W)((UW)memend - (UW)knl_lowmem_top - sizeof(QUEUE)*2);

	knl_lowmem_top = memend;  /* Update memory free space */

	initIMACB();

	/* Register on AreaQue */
	end = (QUEUE*)((VB*)top + knl_imacb->memsz) + 1;
	knl_insertAreaQue(&knl_imacb->areaque, end);
	knl_insertAreaQue(&knl_imacb->areaque, top);
	setAreaFlag(end, AREA_USE);
	setAreaFlag(&knl_imacb->areaque, AREA_USE);

	knl_appendFreeArea(knl_imacb, top);

	return E_OK;
}
#endif /* USE_FUNC_INIT_IMALLOC */

#endif /* USE_IMALLOC */
