/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.07.B0
 *
 *    Copyright (C) 2006-2023 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2023/11.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	syslib.h
 * @brief	micro T-Kernel システムライブラリ
 *
 * 割込みコントローラ制御、微小時間待ち、高速ロック（FastLock）、
 * マルチロック（FastMLock）、メモリ割り当て、物理タイマなど、
 * システムライブラリの API を宣言します。
 */


#ifndef __TK_SYSLIB_H__
#define __TK_SYSLIB_H__

#include <tk/typedef.h>


/* ------------------------------------------------------------------------ */
/*
 * システム依存部（CPU 割込み制御、I/O ポートアクセス）
 */
#define SYSLIB_PATH_(a)		#a
#define SYSLIB_PATH(a)		SYSLIB_PATH_(a)
#define SYSLIB_SYSDEP()		SYSLIB_PATH(sysdepend/TARGET_DIR/syslib.h)
#include SYSLIB_SYSDEP()


/*----------------------------------------------------------------------*/
/*
 * 割込みコントローラ制御
 *
 */
#if TK_SUPPORT_INTCTRL
#if TK_HAS_ENAINTLEVEL
IMPORT void EnableInt( UINT intno, INT level );	/* intno で指定した割込みの許可 */
#else
IMPORT void EnableInt( UINT intno );		/* intno で指定した割込みの許可 */
#endif /* TK_HAS_ENAINTLEVEL */

IMPORT void DisableInt( UINT intno );	/* intno で指定した割込みの禁止 */
IMPORT void ClearInt(UINT intno);	/* 指定割込みの発生状態のクリア */
IMPORT void EndOfInt(UINT intno);	/* 割込みコントローラへの EOI 発行 */
IMPORT BOOL CheckInt( UINT intno );	/* 指定割込みの発生状態の確認 */

#endif /* TK_SUPPORT_INTCTRL */

#if TK_SUPPORT_INTMODE
IMPORT void SetIntMode(UINT intno, UINT mode);	/* 割込みモードの設定 */
#endif /* TK_SUPPORT_INTMODE */

#if TK_SUPPORT_CPUINTLEVEL
IMPORT void SetCpuIntLevel( INT level );
IMPORT INT GetCpuIntLevel( void );
#endif /* TK_SUPPORT_CPUINTLEVEL */

#if TK_SUPPORT_CTRLINTLEVEL
IMPORT void SetCtrlIntLevel(INT level);	/* 割込みコントローラの割込みマスクレベルの設定 */
IMPORT INT  GetCtrlIntLevel(void);	/* 割込みコントローラの割込みマスクレベルの取得 */
#endif /* TK_SUPPORT_CTRLINTLEVEL */


/* ------------------------------------------------------------------------ */
/*
 * 微小時間待ち
 */

IMPORT void WaitUsec( UW usec );	/* マイクロ秒単位の待ち */
IMPORT void WaitNsec( UW nsec );	/* ナノ秒単位の待ち */


/* ------------------------------------------------------------------------ */
/*
 * 高速ロック
 */
typedef struct {
	INT		cnt;
	ID		id;
	CONST UB	*name;
} FastLock;

IMPORT ER CreateLock( FastLock *lock, CONST UB *name );
IMPORT void DeleteLock( FastLock *lock );
IMPORT void Lock( FastLock *lock );
IMPORT void Unlock( FastLock *lock );


/* ------------------------------------------------------------------------ */
/*
 * マルチロック
 *	1 つの FastMLock で最大 16 個または 32 個の独立したロックを使用できる。
 *	各ロックはロック番号（no）で区別し、no には 0〜15 または 0〜31 を
 *	指定できる。
 *	（FastLock よりやや効率が劣る）
 */
typedef struct {
	UINT		flg;
	INT		wai;
	ID		id;
	CONST UB	*name;
} FastMLock;

IMPORT ER CreateMLock( FastMLock *lock, CONST UB *name );
IMPORT ER DeleteMLock( FastMLock *lock );
IMPORT ER MLockTmo( FastMLock *lock, INT no, TMO tmout );
IMPORT ER MLock( FastMLock *lock, INT no );
IMPORT ER MUnlock( FastMLock *lock, INT no );


/* ------------------------------------------------------------------------ */
/*
 * メモリ割り当て
 */
#if TK_SUPPORT_MEMLIB

#ifndef PROHIBIT_DEF_SIZE_T
typedef SZ		size_t;
#endif

IMPORT void *Kmalloc( size_t size );
IMPORT void *Kcalloc( size_t nmemb, size_t size );
IMPORT void *Krealloc( void *ptr, size_t size);
IMPORT void Kfree( void *ptr );

#endif /* TK_SUPPORT_MEMLIB */


/* ------------------------------------------------------------------------ */
/*
 * 物理タイマ
 */
#if TK_SUPPORT_PTIMER

#define TA_ALM_PTMR	0
#define TA_CYC_PTMR	1

typedef struct {
	void	*exinf;		/* 拡張情報 */
	ATR	ptmratr;	/* 物理タイマ属性 */
	FP	ptmrhdr;	/* 物理タイマハンドラのアドレス */
} T_DPTMR;

typedef struct {
	UW	ptmrclk;	/* 物理タイマのクロック周波数 */
	UW	maxcount;	/* 最大カウント値 */
	BOOL	defhdr;		/* ハンドラサポートの有無 */
} T_RPTMR;

IMPORT ER StartPhysicalTimer( UINT ptmrno, UW limit, UINT mode);
IMPORT ER StopPhysicalTimer( UINT ptmrno );
IMPORT ER GetPhysicalTimerCount( UINT ptmrno, UW *p_count );
IMPORT ER DefinePhysicalTimerHandler( UINT ptmrno, CONST T_DPTMR *pk_dptmr );
IMPORT ER GetPhysicalTimerConfig(UINT ptmrno, T_RPTMR *pk_rptmr);

#endif /* TK_SUPPORT_PTIMER */


/* ------------------------------------------------------------------------ */
/*
 * 4 文字のオブジェクト名
 *	（使用例）
 *	T_CTSK	ctsk;
 *	SetOBJNAME(ctsk.exinf, "TEST");
 */
union objname {
	char	s[4];
	void	*i;
};

#define SetOBJNAME(exinf, name)					\
	{							\
		UB *d, *s; INT i;				\
		d = (UB*)&(exinf);				\
		s = (UB*)name;					\
		for(i=0; i<4; i++) *d++ = *s++;			\
	}

#endif /* __TK_SYSLIB_H__ */
