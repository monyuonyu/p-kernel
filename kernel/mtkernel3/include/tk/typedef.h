/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.07.B0
 *
 *    Copyright (C) 2006-2023 by Ken Sakamura.
 *    This software is distributed under the T-License 2.1.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2023/11.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	typedef.h
 * @brief	T-Kernel 標準データ型定義
 *
 * T-Kernel の API で共通に使用する汎用データ型、ブール値、
 * T-Kernel/OS 仕様で意味が定義されるデータ型、および共通定数を
 * 定義します。
 */

#ifndef	__TK_TYPEDEF_H__
#define __TK_TYPEDEF_H__

#ifdef CHK_TKERNEL_CONST
#define CONST	const
#else
#define CONST
#endif

/*
 * 汎用データ型
 */
#if USE_STDINC_STDINT	/* <stdint.h> を使用する場合 */
typedef int8_t			B;		/* 符号付き 8 ビット整数 */
typedef int16_t			H;		/* 符号付き 16 ビット整数 */
typedef int32_t			W;		/* 符号付き 32 ビット整数 */
typedef int64_t			D;		/* 符号付き 64 ビット整数 */
typedef uint8_t			UB;		/* 符号無し 8 ビット整数 */
typedef uint16_t	  	UH;		/* 符号無し 16 ビット整数 */
typedef uint32_t		UW;		/* 符号無し 32 ビット整数 */
typedef uint64_t		UD;		/* 符号無し 64 ビット整数 */

typedef int8_t			VB;		/* 内容が一定の型を持たない 8 ビットデータ */
typedef int16_t			VH;		/* 内容が一定の型を持たない 16 ビットデータ */
typedef int32_t			VW;		/* 内容が一定の型を持たない 32 ビットデータ */
typedef int64_t			VD;		/* 内容が一定の型を持たない 64 ビットデータ */

#else		/* <stdint.h> を使用しない場合 */

typedef signed char		B;		/* 符号付き 8 ビット整数 */
typedef signed short		H;		/* 符号付き 16 ビット整数 */
typedef signed long		W;		/* 符号付き 32 ビット整数 */
typedef signed long long	D;		/* 符号付き 64 ビット整数 */
typedef unsigned char		UB;		/* 符号無し 8 ビット整数 */
typedef unsigned short  	UH;		/* 符号無し 16 ビット整数 */
typedef unsigned long		UW;		/* 符号無し 32 ビット整数 */
typedef unsigned long long	UD;		/* 符号無し 64 ビット整数 */

typedef signed char		VB;		/* 内容が一定の型を持たない 8 ビットデータ */
typedef signed short		VH;		/* 内容が一定の型を持たない 16 ビットデータ */
typedef signed long		VW;		/* 内容が一定の型を持たない 32 ビットデータ */
typedef signed long long	VD;		/* 内容が一定の型を持たない 64 ビットデータ */

#endif	/* USE_STDINC_STDINT */

typedef signed int		INT;		/* プロセッサのビット幅の符号付き整数 */
typedef unsigned int		UINT;		/* プロセッサのビット幅の符号無し整数 */

typedef volatile B		_B;		/* volatile 修飾付きの型 */
typedef volatile H		_H;
typedef volatile W		_W;
typedef volatile D		_D;
typedef volatile UB		_UB;
typedef volatile UH		_UH;
typedef volatile UW		_UW;
typedef volatile UD		_UD;

typedef W			SZ;		/* サイズ一般 */

typedef INT			ID;		/* ID 番号一般 */
typedef	W			MSEC;		/* 時間一般（ミリ秒） */

typedef void			(*FP)();	/* 関数アドレス一般 */
typedef INT			(*FUNCP)();	/* 関数アドレス一般 */

#define LOCAL			static		/* ローカルシンボル定義 */
#define EXPORT					/* グローバルシンボル定義 */
#define IMPORT			extern		/* グローバルシンボル参照 */

/*
 * ブール値
 *	TRUE = 1 と定義されるが、0 以外はすべて真として扱われる。
 *	そのため bool == TRUE のような比較は行ってはならず、
 *	bool != FALSE の形式で判定すること。
 */
typedef UINT			BOOL;
#define TRUE			1		/* 真 */
#define FALSE			0		/* 偽 */

/*
 * T-Kernel/OS 仕様で意味が定義されるデータ型
 */
typedef INT			FN;		/* 機能コード */
typedef INT			RNO;		/* ランデブ番号 */
typedef UW			ATR;		/* オブジェクト／ハンドラ属性 */
typedef INT			ER;		/* エラーコード */
typedef INT			PRI;		/* 優先度 */
typedef W			TMO;		/* タイムアウト指定 */
typedef UW			RELTIM;		/* 相対時間 */

typedef struct systim {				/* システム時刻 */
	W			hi;		/* 上位 32 ビット */
	UW			lo;		/* 下位 32 ビット */
} SYSTIM;

typedef D			SYSTIM_U;	/* システム時刻（64 ビット） */

/*
 * 共通定数
 */
#ifndef NULL
#define NULL		0
#endif

#define TA_NULL		0U		/* 特別な属性の指定なし */
#define TMO_POL		0		/* ポーリング */
#define TMO_FEVR	(-1)		/* 永久待ち */

/* ------------------------------------------------------------------------ */

#endif /* __TK_TYPEDEF_H__ */
