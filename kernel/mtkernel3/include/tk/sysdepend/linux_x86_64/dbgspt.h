/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux x86-64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dbgspt.h
 *	デバッガサポート機能 定義（Linux x86-64 ユーザモードポート依存部）
 */

#ifndef __TK_DBGSPT_DEPEND_H__
#define __TK_DBGSPT_DEPEND_H__

/*
 * システムコール／拡張 SVC 呼び出し時情報
 */
typedef struct td_calinf {
	void	*sp;		/* 呼び出し時のスタックポインタ */
	void	*pc;		/* 呼び出し時のプログラムカウンタ */
} TD_CALINF;

#endif /* __TK_DBGSPT_DEPEND_H__ */
