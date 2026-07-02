/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.04
 *
 *    Copyright (C) 2006-2021 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2021/05/17.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	config_tm.h
 * @brief	T-Monitor コンフィグレーション定義
 *
 * T-Monitor 互換ライブラリが使用する通信ポートと、
 * tm_printf() 関連の設定を定義します。
 */

#ifndef __TM_CONFIG_H__
#define __TM_CONFIG_H__

/*---------------------------------------------------------------------- */
/* 通信ポートの選択
 *      T-Monitor が使用する通信ポートを選択する。
 *         1: 有効  0: 無効（いずれか 1 つのみ有効にできる）
 */
#define	TM_COM_SERIAL_DEV	(1)	/* シリアル通信デバイスを使用 */
#define	TM_COM_NO_DEV		(0)	/* 通信ポートを使用しない */

/*---------------------------------------------------------------------- */
/* tm_printf() 呼び出しの設定
 *         1: 有効  0: 無効
 */
#define	USE_TM_PRINTF		(1)	/* tm_printf()・tm_sprintf() の使用 */
#define	TM_OUTBUF_SZ		(0)	/* スタック上に確保する出力バッファのサイズ */

#endif /* __TM_CONFIG_H__ */
