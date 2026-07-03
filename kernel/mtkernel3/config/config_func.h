/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.01
 *
 *    Copyright (C) 2006-2020 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2020/05/29.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	config_func.h
 * @brief	使用機能のユーザコンフィグレーション定義
 *
 * カーネルに組み込む機能単位（セマフォ、ミューテックスなど）と、
 * 個々の API（USE_FUNC_XXX）の使用有無を定義します。
 */

#ifndef _CONFIG_FUNC_H_
#define _CONFIG_FUNC_H_

#define USE_SEMAPHORE		(1)
#define	USE_MUTEX		(1)
#define	USE_EVENTFLAG		(1)
#define	USE_MAILBOX		(1)
#define	USE_MESSAGEBUFFER	(1)
#define USE_RENDEZVOUS		(1)
#define USE_MEMORYPOOL		(1)
#define	USE_FIX_MEMORYPOOL	(1)
#define	USE_TIMEMANAGEMENT	(1)
#define	USE_CYCLICHANDLER	(1)
#define USE_ALARMHANDLER	(1)
#define USE_DEVICE		(1)
#define USE_FAST_LOCK		(1)
#define USE_MULTI_LOCK		(1)

/* タスク管理 */
#define USE_FUNC_TK_DEL_TSK
#define USE_FUNC_TK_EXT_TSK
#define USE_FUNC_TK_EXD_TSK
#define USE_FUNC_TK_TER_TSK
#define USE_FUNC_TK_CHG_PRI
#define USE_FUNC_TK_REL_WAI
#define USE_FUNC_TK_GET_REG
#define USE_FUNC_TK_SET_REG
#define USE_FUNC_TK_GET_CPR
#define USE_FUNC_TK_SET_CPR
#define USE_FUNC_TK_REF_TSK
#define USE_FUNC_TK_SUS_TSK
#define USE_FUNC_TK_RSM_TSK
#define USE_FUNC_TK_FRSM_TSK
#define USE_FUNC_TK_SLP_TSK
#define USE_FUNC_TK_WUP_TSK
#define USE_FUNC_TK_CAN_WUP
#define USE_FUNC_TK_DLY_TSK
#define USE_FUNC_TD_LST_TSK
#define USE_FUNC_TD_REF_TSK
#define USE_FUNC_TD_INF_TSK
#define USE_FUNC_TD_GET_REG
#define USE_FUNC_TD_SET_REG

/* セマフォ管理 API */
#define USE_FUNC_TK_DEL_SEM
#define USE_FUNC_TK_REF_SEM
#define USE_FUNC_TD_LST_SEM
#define USE_FUNC_TD_REF_SEM
#define USE_FUNC_TD_SEM_QUE

/* ミューテックス管理 API */
#define USE_FUNC_TK_DEL_MTX
#define USE_FUNC_TK_REF_MTX
#define USE_FUNC_TD_LST_MTX
#define USE_FUNC_TD_REF_MTX
#define USE_FUNC_TD_MTX_QUE

/* イベントフラグ管理 API */
#define USE_FUNC_TK_DEL_FLG
#define USE_FUNC_TK_REF_FLG
#define USE_FUNC_TD_LST_FLG
#define USE_FUNC_TD_REF_FLG
#define USE_FUNC_TD_FLG_QUE

/* メールボックス管理 API */
#define USE_FUNC_TK_DEL_MBX
#define USE_FUNC_TK_REF_MBX
#define USE_FUNC_TD_LST_MBX
#define USE_FUNC_TD_REF_MBX
#define USE_FUNC_TD_MBX_QUE

/* メッセージバッファ管理 API */
#define USE_FUNC_TK_DEL_MBF
#define USE_FUNC_TK_REF_MBF
#define USE_FUNC_TD_LST_MBF
#define USE_FUNC_TD_REF_MBF
#define USE_FUNC_TD_SMBF_QUE
#define USE_FUNC_TD_RMBF_QUE

/* ランデブ管理 API（レガシー API） */
#define USE_FUNC_TK_DEL_POR
#define USE_FUNC_TK_FWD_POR
#define USE_FUNC_TK_REF_POR
#define USE_FUNC_TD_LST_POR
#define USE_FUNC_TD_REF_POR
#define USE_FUNC_TD_CAL_QUE
#define USE_FUNC_TD_ACP_QUE

/* 可変長メモリプール管理 API */
#define USE_FUNC_TK_DEL_MPL
#define USE_FUNC_TK_REF_MPL
#define USE_FUNC_TD_LST_MPL
#define USE_FUNC_TD_REF_MPL
#define USE_FUNC_TD_MPL_QUE

/* 固定長メモリプール管理 API */
#define USE_FUNC_TK_DEL_MPF
#define USE_FUNC_TK_REF_MPF
#define USE_FUNC_TD_LST_MPF
#define USE_FUNC_TD_REF_MPF
#define USE_FUNC_TD_MPF_QUE

/* 時間管理 API */
#define USE_FUNC_TK_SET_UTC
#define USE_FUNC_TK_GET_UTC
#define USE_FUNC_TK_SET_TIM
#define USE_FUNC_TK_GET_TIM
#define USE_FUNC_TK_GET_OTM
#define USE_FUNC_TD_GET_TIM
#define USE_FUNC_TD_GET_OTM

/* 周期ハンドラ管理 API */
#define USE_FUNC_TK_DEL_CYC
#define USE_FUNC_TK_STA_CYC
#define USE_FUNC_TK_STP_CYC
#define USE_FUNC_TK_REF_CYC
#define USE_FUNC_TD_LST_CYC
#define USE_FUNC_TD_REF_CYC

/* アラームハンドラ管理 API */
#define USE_FUNC_TK_DEL_ALM
#define USE_FUNC_TK_STP_ALM
#define USE_FUNC_TK_REF_ALM
#define USE_FUNC_TD_LST_ALM
#define USE_FUNC_TD_REF_ALM

/* システム状態管理 API */
#define USE_FUNC_TK_ROT_RDQ
#define USE_FUNC_TK_GET_TID
#define USE_FUNC_TK_DIS_DSP
#define USE_FUNC_TK_ENA_DSP
#define USE_FUNC_TK_REF_SYS
#define USE_FUNC_TK_REF_VER
#define USE_FUNC_TD_REF_SYS
#define USE_FUNC_TD_RDY_QUE

#endif /* _CONFIG_FUNC_H_ */
