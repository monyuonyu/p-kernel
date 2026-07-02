/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel 互換拡張
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	subsystem.h
 *	サブシステム管理の定義（p-kernel 互換拡張）
 *
 *	μT-Kernel 3.0 ではサブシステム（拡張 SVC）機構が廃止されたが、
 *	p-kernel の x86 ベアメタルポートは ring3 ユーザ空間への
 *	syscall 橋渡し（fs/net/blk サブシステム）に knl_svc_ientry を
 *	使用しているため、micro T-Kernel 2.0 から最小限を移植する。
 *	USE_SUBSYSTEM を定義したターゲットでのみ有効。
 */

#ifndef _SUBSYSTEM_H_
#define _SUBSYSTEM_H_

#ifndef USE_SUBSYSTEM
#define USE_SUBSYSTEM		0
#endif

#if USE_SUBSYSTEM

/* サブシステム数の上限（ターゲット config で上書き可能） */
#ifndef CNF_MAX_SSYID
#define CNF_MAX_SSYID		(4)
#endif

#define MIN_SSYID		(1)
#define MAX_SSYID		(CNF_MAX_SSYID)
#define NUM_SSYID		(CNF_MAX_SSYID)
#define INDEX_SSY(id)		((id) - (MIN_SSYID))
#define ID_SSY(index)		((index) + (MIN_SSYID))

typedef INT  (*SVC)( void *pk_para, FN fncd );	/* 拡張 SVC ハンドラ */
typedef void (*SSYCLEANUP)( ID tskid );		/* タスク終了時クリーンアップ */

/*
 * サブシステム制御ブロック（SSYCB）
 */
typedef struct subsystem_control_block {
	SVC		svchdr;		/* 拡張 SVC ハンドラ */
	SSYCLEANUP	cleanupfn;	/* タスク終了時フック（NULL = なし） */
} SSYCB;

IMPORT SSYCB knl_ssycb_table[];	/* サブシステム制御ブロックテーブル */

#define get_ssycb(id)	( &knl_ssycb_table[INDEX_SSY(id)] )

/* 未登録スロットのダミーハンドラ（常に E_RSFN を返す） */
IMPORT INT knl_no_support( void *pk_para, FN fncd );

/* サブシステム管理の初期化（ターゲットの初期化処理から呼ぶ） */
IMPORT ER knl_subsystem_initialize( void );

/* 全サブシステムのクリーンアップ関数を呼ぶ（タスク終了時） */
IMPORT void knl_ssy_cleanup( ID tskid );

/* 拡張 SVC ハンドラへの分岐（機能コード下位 8bit = SSID） */
IMPORT ER knl_svc_ientry( void *pk_para, FN fncd );

#endif /* USE_SUBSYSTEM */
#endif /* _SUBSYSTEM_H_ */
