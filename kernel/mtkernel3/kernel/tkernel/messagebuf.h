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
 * @file	messagebuf.h
 * @brief	メッセージバッファ機能のカーネル内部定義
 *
 * メッセージバッファ管理ブロック（MBFCB）、メッセージヘッダ形式、
 * および空き容量・空判定のインライン関数を定義します。
 */

#ifndef _MESSAGEBUF_H_
#define _MESSAGEBUF_H_

/*
 * メッセージバッファ管理ブロック
 *
 *	一つのメッセージバッファでは受信待ちタスク（TTW_MBF）と
 *	送信待ちタスク（TTW_SMBF）が同時に存在することはないため、
 *	待ちキューを共用することも可能です。
 *	ただしバッファサイズが 0 の場合、待ちキューが受信用か送信用かを
 *	判別しにくくなるため、この方法は採用していません。
 */
typedef struct messagebuffer_control_block {
	QUEUE	send_queue;	/* メッセージバッファ送信待ちキュー */
	ID	mbfid;		/* メッセージバッファID */
	void	*exinf;		/* 拡張情報 */
	ATR	mbfatr;		/* メッセージバッファ属性 */
	QUEUE	recv_queue;	/* メッセージバッファ受信待ちキュー */
	W	bufsz;		/* メッセージバッファサイズ */
	INT	maxmsz;		/* メッセージ最大長 */
	W	frbufsz;	/* 空きバッファサイズ */
	W	head;		/* 先頭メッセージの格納位置 */
	W	tail;		/* 最終メッセージの次の格納位置 */
	VB	*buffer;	/* メッセージバッファのアドレス */
#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif
} MBFCB;

IMPORT MBFCB knl_mbfcb_table[];	/* メッセージバッファ管理ブロックテーブル */
IMPORT QUEUE knl_free_mbfcb;	/* 未使用管理ブロックのキュー（FreeQue） */

#define get_mbfcb(id)	( &knl_mbfcb_table[INDEX_MBF(id)] )


/*
 * メッセージヘッダの形式
 */
typedef INT		HEADER;
#define HEADERSZ	(sizeof(HEADER))

#define ROUNDSIZE	(sizeof(HEADER))
#define ROUNDSZ(sz)	(((UW)(sz) + (UW)(ROUNDSIZE-1)) & ~(UW)(ROUNDSIZE-1))

/**
 * @brief メッセージバッファ空き容量の確認
 *
 * ヘッダを含めてサイズ msgsz のメッセージを格納できる空きが
 * あるかを判定します。
 *
 * @param mbfcb	対象メッセージバッファの管理ブロック
 * @param msgsz	格納するメッセージのサイズ（バイト数）
 *
 * @retval TRUE	格納可能
 * @retval FALSE	空き容量不足
 */
Inline BOOL knl_mbf_free( MBFCB *mbfcb, INT msgsz )
{
	return ( HEADERSZ + (UW)msgsz <= (UW)mbfcb->frbufsz );
}

/**
 * @brief メッセージバッファ空判定
 *
 * @param mbfcb	対象メッセージバッファの管理ブロック
 *
 * @retval TRUE	バッファにメッセージがない
 * @retval FALSE	メッセージが存在する
 */
Inline BOOL knl_mbf_empty( MBFCB *mbfcb )
{
	return ( mbfcb->frbufsz == mbfcb->bufsz );
}

#endif /* _MESSAGEBUF_H_ */
