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
 * @file	mailbox.h
 * @brief	メールボックス機能のカーネル内部定義
 *
 * メールボックス管理ブロック（MBXCB）の定義と、メッセージキュー
 * 操作用のマクロ・インライン関数を提供します。
 */

#ifndef _MAILBOX_H_
#define _MAILBOX_H_
/*
 * メールボックス管理ブロック
 *
 *	'mq_head' はメッセージキューの先頭メッセージを指す
 *	ポインタです。メッセージキューが空のときは NULL になります。
 *	'mq_tail' は空でないメッセージキューの末尾を指す
 *	ポインタです。メッセージキューが空のときの値は保証されません。
 *	'mq_tail' はメッセージキューが FIFO（TA_MFIFO）の場合にのみ
 *	使用します。
 */
typedef struct mailbox_control_block {
	QUEUE	wait_queue;	/* メールボックス待ちキュー */
	ID	mbxid;		/* メールボックス ID */
	void	*exinf;		/* 拡張情報 */
	ATR	mbxatr;		/* メールボックス属性 */
	T_MSG	mq_head;	/* メッセージキューの先頭 */
	T_MSG	*mq_tail;	/* メッセージキューの末尾 */
#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} MBXCB;

IMPORT MBXCB knl_mbxcb_table[];	/* メールボックス管理ブロックテーブル */
IMPORT QUEUE knl_free_mbxcb;	/* 未使用管理ブロックのキュー（FreeQue） */

#define get_mbxcb(id)	( &knl_mbxcb_table[INDEX_MBX(id)] )

/*
 * 先頭メッセージの取得
 */
#define headmsg(mbxcb)	( (mbxcb)->mq_head.msgque[0] )

/*
 * 次のメッセージの取得
 */
#define nextmsg(msg)	( ((T_MSG*)(msg))->msgque[0] )

/**
 * @brief メッセージ優先度順のキュー挿入
 *
 * メッセージ pk_msg を、head を先頭とするメッセージキューへ
 * 優先度順（同一優先度では末尾）に挿入します。
 *
 * @param pk_msg	挿入するメッセージ（優先度付き）
 * @param head	メッセージキューの先頭
 */
Inline void knl_queue_insert_mpri( T_MSG_PRI *pk_msg, T_MSG *head )
{
	T_MSG_PRI	*msg;
	T_MSG		*prevmsg = head;

	while ( (msg = (T_MSG_PRI*)nextmsg(prevmsg)) != NULL ) {
		if ( msg->msgpri > pk_msg->msgpri ) {
			break;
		}
		prevmsg = (T_MSG*)msg;
	}
	nextmsg(pk_msg) = msg;
	nextmsg(prevmsg) = pk_msg;
}

#endif /* _MAILBOX_H_ */
