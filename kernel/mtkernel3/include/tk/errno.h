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
 * @file	errno.h
 * @brief	micro T-Kernel エラーコード定義
 *
 * API が返すエラーコードと、メインエラーコード／サブエラーコードを
 * 扱うマクロ（ERCD、MERCD、SERCD）を定義します。
 * TK_SUPPORT_SERCD の設定により、サブエラーコード付きの 32 ビット
 * 形式と、メインエラーコードのみの形式を切り替えます。
 */

#ifndef __TK_ERRNO_H__
#define __TK_ERRNO_H__

#if	TK_SUPPORT_SERCD == TRUE
/*
 * エラーコード関連のマクロを使用しているプログラムを T-Kernel から
 * micro T-Kernel へ移植する場合は、以下のマクロ（ERCD、MERCD、SERCD）を
 * 修正して使用できる。
 */
#ifndef _in_asm_source_
#include <tk/typedef.h>

#define MERCD(er)	( (ER)(er) >> 16 )	/* メインエラーコード */
#define SERCD(er)	( (H)(er) )		/* サブエラーコード */
#define ERCD(mer, ser)	( (ER)(((UW)(mer) << 16) | ((UW)(ser) & 0x0000FFFF)) )
#else
#define ERCD(mer, ser)	( ((mer) << 16) | ((ser) & 0xffff) )
#endif /* _in_asm_source_ */

#define E_OK		(0)	/* 正常終了 */

#define E_SYS		ERCD(-5, 0)	/* システムエラー */
#define E_NOCOP		ERCD(-6, 0)	/* コプロセッサ使用不可 */
#define E_NOSPT		ERCD(-9, 0)	/* 未サポート機能 */
#define E_RSFN		ERCD(-10, 0)	/* 予約機能コード番号 */
#define E_RSATR		ERCD(-11, 0)	/* 予約属性 */
#define E_PAR		ERCD(-17, 0)	/* パラメータエラー */
#define E_ID		ERCD(-18, 0)	/* 不正 ID 番号 */
#define E_CTX		ERCD(-25, 0)	/* コンテキストエラー */
#define E_MACV		ERCD(-26, 0)	/* アクセス不可メモリ／メモリアクセス権違反 */
#define E_OACV		ERCD(-27, 0)	/* オブジェクトアクセス権違反 */
#define E_ILUSE		ERCD(-28, 0)	/* システムコール不正使用 */
#define E_NOMEM		ERCD(-33, 0)	/* メモリ不足 */
#define E_LIMIT		ERCD(-34, 0)	/* システム制限超過 */
#define E_OBJ		ERCD(-41, 0)	/* オブジェクト状態不正 */
#define E_NOEXS		ERCD(-42, 0)	/* オブジェクト未存在 */
#define E_QOVR		ERCD(-43, 0)	/* キューイングオーバフロー */
#define E_RLWAI		ERCD(-49, 0)	/* 待ち状態の強制解除 */
#define E_TMOUT		ERCD(-50, 0)	/* ポーリング失敗／タイムアウト */
#define E_DLT		ERCD(-51, 0)	/* 待ち対象オブジェクトの削除 */
#define E_DISWAI	ERCD(-52, 0)	/* 待ち禁止による待ち解除 */

#define E_IO		ERCD(-57, 0)	/* 入出力エラー */
#define E_NOMDA		ERCD(-58, 0)	/* メディアなし */
#define E_BUSY		ERCD(-65, 0)	/* ビジー状態 */
#define E_ABORT		ERCD(-66, 0)	/* 処理中断 */
#define E_RONLY		ERCD(-67, 0)	/* 書き込み禁止 */

#else	/* TK_SUPPORT_SERCD */

#ifndef _in_asm_source_
#include "tk/typedef.h"

#define MERCD(er)	( (ER)(er) )	/* メインエラーコード */
#endif /* _in_asm_source_ */

#define E_OK		(0)	/* 正常終了 */

#define E_SYS		(-5)	/* システムエラー */
#define E_NOCOP		(-6)	/* コプロセッサ使用不可 */
#define E_NOSPT		(-9)	/* 未サポート機能 */
#define E_RSFN		(-10)	/* 予約機能コード番号 */
#define E_RSATR		(-11)	/* 予約属性 */
#define E_PAR		(-17)	/* パラメータエラー */
#define E_ID		(-18)	/* 不正 ID 番号 */
#define E_CTX		(-25)	/* コンテキストエラー */
#define E_MACV		(-26)	/* アクセス不可メモリ／メモリアクセス権違反 */
#define E_OACV		(-27)	/* オブジェクトアクセス権違反 */
#define E_ILUSE		(-28)	/* システムコール不正使用 */
#define E_NOMEM		(-33)	/* メモリ不足 */
#define E_LIMIT		(-34)	/* システム制限超過 */
#define E_OBJ		(-41)	/* オブジェクト状態不正 */
#define E_NOEXS		(-42)	/* オブジェクト未存在 */
#define E_QOVR		(-43)	/* キューイングオーバフロー */
#define E_RLWAI		(-49)	/* 待ち状態の強制解除 */
#define E_TMOUT		(-50)	/* ポーリング失敗／タイムアウト */
#define E_DLT		(-51)	/* 待ち対象オブジェクトの削除 */
#define E_DISWAI	(-52)	/* 待ち禁止による待ち解除 */

#define E_IO		(-57)	/* 入出力エラー */
#define E_NOMDA		(-58)	/* メディアなし */
#define E_BUSY		(-65)	/* ビジー状態 */
#define E_ABORT		(-66)	/* 処理中断 */
#define E_RONLY		(-67)	/* 書き込み禁止 */

#endif	/* TK_SUPPORT_SERCD */


#endif /* __TK_ERRNO_H__ */
