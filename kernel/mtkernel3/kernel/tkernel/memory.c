/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.02
 *
 *    Copyright (C) 2006-2020 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2020/10/21 .
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	memory.c
 * @brief	カーネル内動的メモリ管理
 *
 * カーネル内部で使用する動的メモリ（Imalloc）の実装です。
 * 確保済み領域を連結する AreaQue と、空き領域をサイズ順に連結する
 * FreeQue の 2 種類のキューで空間を管理します。
 */

#include "kernel.h"
#include "memory.h"

/**
 * @brief FreeQue の探索
 *
 * サイズが blksz と等しい、または blksz より大きく最も近い
 * 空き領域を探索します。
 * 見つからない場合は &imacb->freeque を返します。
 *
 * @param imacb メモリ確保管理情報
 * @param blksz 要求ブロックサイズ（バイト数）
 * @return 該当する空き領域のキューエントリ。無ければ &imacb->freeque
 */
EXPORT QUEUE* knl_searchFreeArea( IMACB *imacb, W blksz )
{
	QUEUE	*q = &imacb->freeque;

	/* 要求サイズがメモリプールサイズの 1/4 未満なら小さい方から、
	   それ以外は大きい方から探索する */
	if ( blksz > imacb->memsz / 4 ) {
		/* 大きいサイズ側から探索 */
		W fsz = 0;
		while ( (q = q->prev) != &imacb->freeque ) {
			fsz = FreeSize(q);
			if ( fsz <= blksz ) {
				return ( fsz < blksz )? q->next: q;
			}
		}
		return ( fsz >= blksz )? q->next: q;
	} else {
		/* 小さいサイズ側から探索 */
		while ( (q = q->next) != &imacb->freeque ) {
			if ( FreeSize(q) >= blksz ) {
				break;
			}
		}
		return q;
	}
}


/**
 * @brief 空き領域の FreeQue への登録
 *
 * FreeQue は、サイズの異なる領域をサイズ順に連結するキューと、
 * 同一サイズの領域どうしを連結するキューの 2 種類で構成されます。
 *
 *	freeque
 *	|
 *	|   +-----------------------+	    +-----------------------+
 *	|   | AreaQue		    |	    | AreaQue		    |
 *	|   +-----------------------+	    +-----------------------+
 *	*---> FreeQue サイズ順      |	    | EmptyQue		    |
 *	|   | FreeQue 同一サイズ  --------->| FreeQue 同一サイズ  ----->
 *	|   |			    |	    |			    |
 *	|   |			    |	    |			    |
 *	|   +-----------------------+	    +-----------------------+
 *	|   | AreaQue		    |	    | AreaQue		    |
 *	v   +-----------------------+	    +-----------------------+
 *
 * @param imacb メモリ確保管理情報
 * @param aq 登録する領域の AreaQue エントリ
 */
EXPORT void knl_appendFreeArea( IMACB *imacb, QUEUE *aq )
{
	QUEUE	*fq;
	W	size = AreaSize(aq);

	/* 登録位置の探索 */
	/*  サイズが等しい、またはより大きく最も近い空き領域を探す。
	 *  見つからない場合は &imacb->freeque が返る。
	 */
	fq = knl_searchFreeArea(imacb, size);

	/* 登録 */
	clrAreaFlag(aq, AREA_USE);
	if ( fq != &imacb->freeque && FreeSize(fq) == size ) {
		/* FreeQue 同一サイズのキューへ連結 */
		(aq + 2)->next = (fq + 1)->next;
		(fq + 1)->next = aq + 2;
		(aq + 2)->prev = fq + 1;
		if( (aq + 2)->next != NULL ) {
			(aq + 2)->next->prev = aq + 2;
		}
		(aq + 1)->next = NULL;
	} else {
		/* FreeQue サイズ順のキューへ連結 */
		QueInsert(aq + 1, fq);
		(aq + 2)->next = NULL;
		(aq + 2)->prev = (QUEUE*)(KNL_UPTR)size;	/* p-kernel: LP64/LLP64 の警告回避（サイズ値の格納） */
	}
}

/**
 * @brief FreeQue からの削除
 *
 * @param fq 削除する空き領域の FreeQue エントリ
 */
EXPORT void knl_removeFreeQue( QUEUE *fq )
{
	if ( fq->next == NULL ) {	/* FreeQue 同一サイズのキュー */
		(fq + 1)->prev->next = (fq + 1)->next;
		if ( (fq + 1)->next != NULL ) {
			(fq + 1)->next->prev = (fq + 1)->prev;
		}
	} else {			/* FreeQue サイズ順のキュー */
		if ( (fq + 1)->next != NULL ) {		/* 同一サイズのキューを持つ場合 */
			QueInsert((fq + 1)->next - 1, fq);
			(fq + 1)->next->prev = (fq + 1)->prev;
		}
		QueRemove(fq);
	}
}

/**
 * @brief 領域の登録
 *
 * ent を que の直後に挿入します。
 *
 * @param que 挿入位置となる AreaQue エントリ
 * @param ent 挿入する AreaQue エントリ
 */
EXPORT void knl_insertAreaQue( QUEUE *que, QUEUE *ent )
{
	ent->prev = que;
	ent->next = que->next;
	Assign(que->next->prev, ent);
	que->next = ent;
}

/**
 * @brief 領域の削除
 *
 * @param aq 削除する AreaQue エントリ
 */
EXPORT void knl_removeAreaQue( QUEUE *aq )
{
	Mask(aq->prev)->next = aq->next;
	Assign(aq->next->prev, Mask(aq->prev));
}

/* ------------------------------------------------------------------------ */

#if USE_IMALLOC
/* ------------------------------------------------------------------------ */

Noinit(EXPORT IMACB *knl_imacb);

/* ------------------------------------------------------------------------ */

/**
 * @brief メモリの確保
 *
 * FreeQue から size バイト以上の空き領域を探して確保します。
 * 残りが最小フラグメントサイズ以上あれば領域を分割し、
 * 残余を FreeQue へ再登録します。
 *
 * @param size 要求サイズ（バイト数）
 * @return 確保したメモリの先頭アドレス。size が 0 以下、
 *         またはメモリ不足の場合は NULL
 * @note 割込み禁止により排他制御を行います。
 */
EXPORT void* knl_Imalloc( SZ size )
{
	QUEUE	*q, *aq, *aq2;
	UINT	imask;

	/* 最小フラグメントサイズより小さい場合は、
	   最小サイズに切り上げて確保する */
	if( size <= 0 ) {
		return (void *)NULL;
	} else 	if ( size < MIN_FRAGMENT ) {
		size = MIN_FRAGMENT;
	} else {
		size = ROUND(size);
	}

	DI(imask);  /* 割込み禁止による排他制御 */

	/* FreeQue の探索 */
	q = knl_searchFreeArea(knl_imacb, size);
	if ( q == &(knl_imacb->freeque) ) {
		q = NULL; /* メモリ不足 */
		goto err_ret;
	}

	/* 空き領域あり: いったん FreeQue から切り離す */
	knl_removeFreeQue(q);

	aq = q - 1;

	/* 残余が最小フラグメントサイズに満たない場合は、
	   その分も含めて確保する */
	if ( FreeSize(q) - size >= MIN_FRAGMENT + sizeof(QUEUE) ) {

		/* 領域を 2 つに分割 */
		aq2 = (QUEUE*)((VB*)(aq + 1) + size);
		knl_insertAreaQue(aq, aq2);

		/* 残りの領域を FreeQue へ登録 */
		knl_appendFreeArea(knl_imacb, aq2);
	}
	setAreaFlag(aq, AREA_USE);

err_ret:
	EI(imask);

	return (void *)q;
}

/**
 * @brief メモリの確保とゼロクリア
 *
 * nmemb * size バイトのメモリを確保し、全体を 0 で初期化します。
 *
 * @param nmemb 要素数
 * @param size 1 要素のサイズ（バイト数）
 * @return 確保したメモリの先頭アドレス。確保できない場合は NULL
 */
EXPORT void* knl_Icalloc( SZ nmemb, SZ size )
{
	SZ	sz = nmemb * size;
	void	*mem;

	mem = knl_Imalloc(sz);
	if ( mem == NULL ) {
		return NULL;
	}

	knl_memset(mem, 0, sz);

	return mem;
}


/**
 * @brief メモリ確保サイズの変更
 *
 * 新しいサイズの領域を確保して旧領域の内容をコピーし、
 * 旧領域を解放します。size が 0 の場合は解放のみ行います。
 *
 * @param ptr 変更対象のメモリ。NULL の場合は新規確保と同じ
 * @param size 変更後のサイズ（バイト数）
 * @return 新しいメモリの先頭アドレス。確保できない場合は NULL
 *         （このとき旧領域は解放されません）
 */
EXPORT void* knl_Irealloc( void *ptr, SZ size )
{
	void	*newptr;
	QUEUE	*aq;
	SZ	oldsz;

	if(size != 0) {
		newptr = knl_Imalloc(size);
		if(newptr == NULL) {
			return NULL;
		}
	} else {
		newptr = NULL;
	}

	if(ptr != NULL) {
		if(newptr != NULL) {
			aq = (QUEUE*)ptr - 1;
			oldsz = (SZ)AreaSize(aq);
			knl_memcpy(newptr, ptr, (size > oldsz)?oldsz:size);
		}
		knl_Ifree(ptr);
	}

	return newptr;
}


/**
 * @brief メモリの解放
 *
 * 領域を解放し、前後に空き領域があれば併合して
 * FreeQue へ登録します。
 *
 * @param ptr 解放するメモリの先頭アドレス
 * @note 割込み禁止により排他制御を行います。
 */
EXPORT void  knl_Ifree( void *ptr )
{
	QUEUE	*aq;
	UINT	imask;

	DI(imask);  /* 割込み禁止による排他制御 */

	aq = (QUEUE*)ptr - 1;
	clrAreaFlag(aq, AREA_USE);

	if ( !chkAreaFlag(aq->next, AREA_USE) ) {
		/* 直後の空き領域と併合 */
		knl_removeFreeQue(aq->next + 1);
		knl_removeAreaQue(aq->next);
	}

	if ( !chkAreaFlag(aq->prev, AREA_USE) ) {
		/* 直前の空き領域と併合 */
		aq = aq->prev;
		knl_removeFreeQue(aq + 1);
		knl_removeAreaQue(aq->next);
	}

	knl_appendFreeArea(knl_imacb, aq);

	EI(imask);
}


/* ------------------------------------------------------------------------ */

/**
 * @brief IMACB の初期化
 */
LOCAL void initIMACB( void )
{
	QueInit(&(knl_imacb->areaque));
	QueInit(&(knl_imacb->freeque));
}

/**
 * @brief Imalloc の初期設定
 *
 * カーネル用メモリ空間（knl_lowmem_top ～ knl_lowmem_limit）に
 * IMACB を配置し、残りの全空間を 1 つの空き領域として
 * AreaQue / FreeQue に登録します。
 *
 * @return 常に E_OK
 */
EXPORT ER knl_init_Imalloc( void )
{
	QUEUE	*top, *end;

	/* p-kernel 変更: ポインタ演算のキャストを UW（32bit）から
	 * ポインタ幅の KNL_UPTR（memory.h）へ変更。LP64 ホスト
	 * （Linux x86-64・bare-metal 等）では unsigned long と同一（64bit）
	 * のため既存コードとバイト一致。LLP64（Windows x86_64）では
	 * unsigned long が 32bit のため 64bit 側を使う。32bit MCU では
	 * いずれも 32bit で従来と同じ。マスクも ~(KNL_UPTR) で 64bit 化。 */

	/* IMACB 用に先頭を 4 バイト境界へ整列 */
	knl_lowmem_top = (void *)(((KNL_UPTR)knl_lowmem_top + 3) & ~(KNL_UPTR)0x3);
	knl_imacb = (IMACB*)knl_lowmem_top;
	knl_lowmem_top = (void *)((KNL_UPTR)knl_lowmem_top + sizeof(IMACB));

	/* 先頭を 8 バイト境界へ整列 */
	knl_lowmem_top = (void *)(((KNL_UPTR)knl_lowmem_top + 7) & ~(KNL_UPTR)0x7);
	top = (QUEUE*)knl_lowmem_top;
	knl_imacb->memsz = (W)((KNL_UPTR)knl_lowmem_limit - (KNL_UPTR)knl_lowmem_top - sizeof(QUEUE)*2);

	knl_lowmem_top = knl_lowmem_limit;  /* 空きメモリ領域の更新 */

	initIMACB();

	/* AreaQue へ登録 */
	end = (QUEUE*)((VB*)top + knl_imacb->memsz) + 1;
	knl_insertAreaQue(&knl_imacb->areaque, end);
	knl_insertAreaQue(&knl_imacb->areaque, top);
	setAreaFlag(end, AREA_USE);
	setAreaFlag(&knl_imacb->areaque, AREA_USE);

	knl_appendFreeArea(knl_imacb, top);

	return E_OK;
}

#endif /* USE_IMALLOC */
