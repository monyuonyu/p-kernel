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
 * @file	rendezvous.h
 * @brief	ランデブ機能のカーネル内部定義
 *
 * ランデブポート管理ブロック（PORCB）、ランデブ番号の生成・分解を行う
 * インライン関数、およびランデブ待ちの待ち仕様の宣言を定義します。
 */

#ifndef _RENDEZVOUS_H_
#define _RENDEZVOUS_H_

/*
 * ランデブポート管理ブロック
 */
typedef struct port_control_block {
	QUEUE	call_queue;	/* ポート呼出待ちキュー */
	ID	porid;		/* ポートID */
	void	*exinf;		/* 拡張情報 */
	ATR	poratr;		/* ポート属性 */
	QUEUE	accept_queue;	/* ポート受付待ちキュー */
	INT	maxcmsz;	/* 呼出メッセージ最大長 */
	INT	maxrmsz;	/* 返答メッセージ最大長 */
#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} PORCB;

IMPORT PORCB knl_porcb_table[];	/* ランデブポート管理ブロックテーブル */
IMPORT QUEUE knl_free_porcb;	/* 未使用管理ブロックのキュー（FreeQue） */

#define get_porcb(id)	( &knl_porcb_table[INDEX_POR(id)] )

#if USE_LEGACY_API

#define RDVNO_SHIFT	(sizeof(RNO)*8/2)

/**
 * @brief ランデブ番号の生成
 *
 * 呼出タスクの TCB が保持する連番（wrdvno）を返し、次回に備えて
 * 上位ビット側の連番部を 1 進めます。下位ビット側にはタスク ID が
 * 保持されます。
 *
 * @param tcb	呼出タスクの TCB
 *
 * @return 生成したランデブ番号
 */
Inline RNO knl_gen_rdvno( TCB *tcb )
{
	RNO	rdvno;

	rdvno = tcb->wrdvno;
	tcb->wrdvno += (1U << RDVNO_SHIFT);

	return rdvno;
}

/**
 * @brief ランデブ番号からのタスク ID 取得
 *
 * ランデブ番号の下位ビットに保持された呼出タスクの ID を
 * 取り出します。
 *
 * @param rdvno	ランデブ番号
 *
 * @return 呼出タスクの ID
 */
Inline ID knl_get_tskid_rdvno( RNO rdvno )
{
	return (ID)((UINT)rdvno & ((1U << RDVNO_SHIFT) - 1));
}

/*
 * ランデブ番号の正当性チェック
 */
#define CHECK_RDVNO(rdvno) {					\
	if ( !CHK_TSKID(knl_get_tskid_rdvno(rdvno)) ) {		\
		return E_OBJ;					\
	}							\
}

#endif	/* USE_LEGACY_API */

/*
 * ランデブ待ちの待ち仕様定義
 */
IMPORT CONST WSPEC knl_wspec_cal_tfifo;
IMPORT CONST WSPEC knl_wspec_cal_tpri;
IMPORT CONST WSPEC knl_wspec_rdv;


#endif /* _RENDEZVOUS_H_ */
