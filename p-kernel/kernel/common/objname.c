/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 2.0 Software Package
 *
 *    Copyright (C) 2006-2014 by Ken Sakamura.
 *    This software is distributed under the T-License 2.0.
 *----------------------------------------------------------------------
 *
 *    Released by T-Engine Forum(http://www.t-engine.org/) at 2014/09/01.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file objname.c
 * @brief オブジェクト名管理機能
 * 
 * T-Kernelのデバッガサポート機能として、各種カーネルオブジェクトの
 * 名前（DS名）を管理する機能を提供する。オブジェクト名はデバッグや
 * システム監視において、オブジェクトの識別を容易にするために使用される。
 * 
 * 主な機能：
 * - オブジェクト名の取得（knl_object_getname）
 * - オブジェクト名の参照（td_ref_dsname）
 * - オブジェクト名の設定（td_set_dsname）
 * - 各種オブジェクトタイプのサポート（タスク、セマフォ、イベントフラグ等）
 * 
 * サポートされるオブジェクトタイプ：
 * TN_TSK, TN_SEM, TN_FLG, TN_MBX, TN_MBF, TN_POR, TN_MTX,
 * TN_MPL, TN_MPF, TN_CYC, TN_ALM
 * 
 * @note USE_OBJECT_NAMEが有効な場合のみ機能する
 * @note USE_DBGSPTが有効な場合のみコンパイルされる
 */

/** [BEGIN Common Definitions] */
#include "kernel.h"
#include <libstr.h>
/** [END Common Definitions] */

#if USE_DBGSPT

#if USE_OBJECT_NAME
#ifdef USE_FUNC_OBJECT_GETNAME
/**
 * @brief オブジェクト名取得
 * 
 * 指定されたオブジェクトタイプとIDに基づいて、オブジェクトの名前を取得する。
 * 各オブジェクトタイプに応じて適切な取得関数を呼び出す。
 * 
 * @param objtype オブジェクトタイプ（TN_TSK, TN_SEM, TN_FLG等）
 * @param objid オブジェクトID
 * @param name 取得した名前へのポインタを格納する変数のアドレス
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_PAR パラメータエラー（未サポートのオブジェクトタイプ）
 * @retval E_ID 不正ID
 * @retval E_NOEXS オブジェクト未生成
 * @retval E_OBJ オブジェクトに名前が設定されていない
 * 
 * @note 対応するオブジェクトタイプのCFN_MAX_xxxIDが0より大きい場合のみ処理される
 * @note 取得された名前ポインタはオブジェクトの制御ブロック内を指す
 */
EXPORT ER knl_object_getname( UINT objtype, ID objid, UB **name)
{
	ER	ercd;

	switch (objtype) {
#if CFN_MAX_TSKID > 0
	  case TN_TSK:
		{
			IMPORT ER knl_task_getname(ID id, UB **name);
			ercd = knl_task_getname(objid, name);
			break;
		}
#endif

#if CFN_MAX_SEMID > 0
	  case TN_SEM:
		{
			IMPORT ER knl_semaphore_getname(ID id, UB **name);
			ercd = knl_semaphore_getname(objid, name);
			break;
		}
#endif

#if CFN_MAX_FLGID > 0
	  case TN_FLG:
		{
			IMPORT ER knl_eventflag_getname(ID id, UB **name);
			ercd = knl_eventflag_getname(objid, name);
			break;
		}
#endif

#if CFN_MAX_MBXID > 0
	  case TN_MBX:
		{
			IMPORT ER knl_mailbox_getname(ID id, UB **name);
			ercd = knl_mailbox_getname(objid, name);
			break;
		}
#endif

#if CFN_MAX_MBFID > 0
	  case TN_MBF:
		{
			IMPORT ER knl_messagebuffer_getname(ID id, UB **name);
			ercd = knl_messagebuffer_getname(objid, name);
			break;
		}
#endif

#if CFN_MAX_PORID > 0
	  case TN_POR:
		{
			IMPORT ER knl_rendezvous_getname(ID id, UB **name);
			ercd = knl_rendezvous_getname(objid, name);
			break;
		}
#endif

#if CFN_MAX_MTXID > 0
	  case TN_MTX:
		{
			IMPORT ER knl_mutex_getname(ID id, UB **name);
			ercd = knl_mutex_getname(objid, name);
			break;
		}
#endif

#if CFN_MAX_MPLID > 0
	  case TN_MPL:
		{
			IMPORT ER knl_memorypool_getname(ID id, UB **name);
			ercd = knl_memorypool_getname(objid, name);
			break;
		}
#endif

#if CFN_MAX_MPFID > 0
	  case TN_MPF:
		{
			IMPORT ER knl_fix_memorypool_getname(ID id, UB **name);
			ercd = knl_fix_memorypool_getname(objid, name);
			break;
		}
#endif

#if CFN_MAX_CYCID > 0
	  case TN_CYC:
		{
			IMPORT ER knl_cyclichandler_getname(ID id, UB **name);
			ercd = knl_cyclichandler_getname(objid, name);
			break;
		}
#endif

#if CFN_MAX_ALMID > 0
	  case TN_ALM:
		{
			IMPORT ER knl_alarmhandler_getname(ID id, UB **name);
			ercd = knl_alarmhandler_getname(objid, name);
			break;
		}
#endif

	  default:
		ercd = E_PAR;

	}

	return ercd;
}
#endif /* USE_FUNC_OBJECT_GETNAME */
#endif /* USE_OBJECT_NAME */

#ifdef USE_FUNC_TD_REF_DSNAME
#if USE_OBJECT_NAME
IMPORT ER knl_object_getname( UINT objtype, ID objid, UB **name);
#endif /* USE_OBJECT_NAME */

/**
 * @brief オブジェクト名参照
 * 
 * 指定されたオブジェクトのDS名（デバッガサポート名）を参照し、
 * 指定されたバッファにコピーする。デバッガやシステム監視ツールで使用される。
 * 
 * @param type オブジェクトタイプ（TN_TSK, TN_SEM, TN_FLG等）
 * @param id オブジェクトID
 * @param dsname DS名を格納するバッファ（OBJECT_NAME_LENGTH分の領域が必要）
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_NOSPT サポートしていない（USE_OBJECT_NAMEが無効）
 * @retval E_PAR パラメータエラー（未サポートのオブジェクトタイプ）
 * @retval E_ID 不正ID
 * @retval E_NOEXS オブジェクト未生成
 * @retval E_OBJ オブジェクトに名前が設定されていない
 * 
 * @note USE_OBJECT_NAMEが無効な場合はE_NOSPTを返す
 * @note 取得した名前はdsnameバッファにコピーされる
 */
SYSCALL ER td_ref_dsname_impl( UINT type, ID id, UB *dsname )
{
#if USE_OBJECT_NAME
	UB	*name_cb;
	ER	ercd;

	ercd = knl_object_getname(type, id, &name_cb);
	if (ercd == E_OK) {
		strncpy((char*)dsname, (char*)name_cb, OBJECT_NAME_LENGTH);
	}

	return ercd;
#else
	return E_NOSPT;
#endif /* USE_OBJECT_NAME */
}
#endif /* USE_FUNC_TD_REF_DSNAME */

#ifdef USE_FUNC_TD_SET_DSNAME
#if USE_OBJECT_NAME
IMPORT ER knl_object_getname( UINT objtype, ID objid, UB **name);
#endif /* USE_OBJECT_NAME */

/**
 * @brief オブジェクト名設定
 * 
 * 指定されたオブジェクトにDS名（デバッガサポート名）を設定する。
 * デバッグやシステム監視において、オブジェクトの識別を容易にするために使用される。
 * 
 * @param type オブジェクトタイプ（TN_TSK, TN_SEM, TN_FLG等）
 * @param id オブジェクトID
 * @param dsname 設定するDS名（最大OBJECT_NAME_LENGTH-1文字）
 * 
 * @return ER エラーコード
 * @retval E_OK 正常終了
 * @retval E_NOSPT サポートしていない（USE_OBJECT_NAMEが無効）
 * @retval E_PAR パラメータエラー（未サポートのオブジェクトタイプ）
 * @retval E_ID 不正ID
 * @retval E_NOEXS オブジェクト未生成
 * @retval E_OBJ オブジェクトに名前設定領域がない
 * 
 * @note USE_OBJECT_NAMEが無効な場合はE_NOSPTを返す
 * @note オブジェクト生成時にTA_DSNAME属性が指定されている必要がある
 * @note 名前はOBJECT_NAME_LENGTH分の領域にコピーされる
 */
SYSCALL ER td_set_dsname_impl( UINT type, ID id, CONST UB *dsname )
{
#if USE_OBJECT_NAME
	UB	*name_cb;
	ER	ercd;

	ercd = knl_object_getname(type, id, &name_cb);
	if (ercd == E_OK) {
		strncpy((char*)name_cb, (char*)dsname, OBJECT_NAME_LENGTH);
	}

	return ercd;
#else
	return E_NOSPT;
#endif /* USE_OBJECT_NAME */
}
#endif /* USE_FUNC_TD_SET_DSNAME */

#endif /* USE_DBGSPT */
