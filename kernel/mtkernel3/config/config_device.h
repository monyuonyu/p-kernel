/*
 *----------------------------------------------------------------------
 *    Device Driver for μT-Kernel 3.00.05
 *
 *    Copyright (C) 2020-2021 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2021/11.
 *
 *----------------------------------------------------------------------
 */


/**
 * @file	config_device.h
 * @brief	デバイスコンフィグレーション定義
 *
 * サンプルデバイスドライバの各デバイスの使用有無を定義します。
 */

#ifndef	__DEV_CONFIG_H__
#define	__DEV_CONFIG_H__

/* ------------------------------------------------------------------------ */
/* デバイス使用設定
 *	1: 使用する   0: 使用しない
 */

#define DEVCNF_USE_SER		1		// シリアル通信デバイス
#define DEVCNF_USE_ADC		1		// A/D 変換デバイス
#define DEVCNF_USE_IIC		1		// I2C 通信デバイス

#endif	/* __DEV_CONFIG_H__ */