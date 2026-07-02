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
 * @file	objname.c
 * @brief	オブジェクト名サポート
 *
 * デバッガサポート機能のオブジェクト名参照・設定 API
 * （td_ref_dsname / td_set_dsname）を実装します。オブジェクト種別に
 * 応じて各オブジェクトの名称取得関数へ振り分けます。
 */

#include "kernel.h"

#if USE_DBGSPT

#if USE_OBJECT_NAME
/**
 * @brief オブジェクト名格納領域の取得
 *
 * オブジェクト種別 objtype と ID objid から、管理ブロック内の
 * オブジェクト名格納領域へのポインタを取得します。種別ごとの
 * 名称取得関数（knl_task_getname 等）へ振り分けます。
 *
 * @param objtype	オブジェクト種別（TN_TSK, TN_SEM 等）
 * @param objid	オブジェクト ID
 * @param name	名称格納領域へのポインタの格納先
 * @retval E_OK	正常終了
 * @retval E_PAR	objtype が不正（未サポートの種別を含む）
 * @retval E_NOEXS	対象オブジェクトが存在しない
 * @retval E_OBJ	TA_DSNAME 属性が指定されていない
 */
LOCAL ER knl_object_getname( UINT objtype, ID objid, UB **name)
{
	ER	ercd;

	switch (objtype) {
	  case TN_TSK:
		{
			IMPORT ER knl_task_getname(ID id, UB **name);
			ercd = knl_task_getname(objid, name);
			break;
		}

#if USE_SEMAPHORE
	  case TN_SEM:
		{
			IMPORT ER knl_semaphore_getname(ID id, UB **name);
			ercd = knl_semaphore_getname(objid, name);
			break;
		}
#endif

#if USE_EVENTFLAG
	  case TN_FLG:
		{
			IMPORT ER knl_eventflag_getname(ID id, UB **name);
			ercd = knl_eventflag_getname(objid, name);
			break;
		}
#endif

#if USE_MAILBOX
	  case TN_MBX:
		{
			IMPORT ER knl_mailbox_getname(ID id, UB **name);
			ercd = knl_mailbox_getname(objid, name);
			break;
		}
#endif

#if USE_MESSAGEBUFFER
	  case TN_MBF:
		{
			IMPORT ER knl_messagebuffer_getname(ID id, UB **name);
			ercd = knl_messagebuffer_getname(objid, name);
			break;
		}
#endif

#if USE_LEGACY_API && USE_RENDEZVOUS
	  case TN_POR:
		{
			IMPORT ER knl_rendezvous_getname(ID id, UB **name);
			ercd = knl_rendezvous_getname(objid, name);
			break;
		}
#endif

#if USE_MUTEX
	  case TN_MTX:
		{
			IMPORT ER knl_mutex_getname(ID id, UB **name);
			ercd = knl_mutex_getname(objid, name);
			break;
		}
#endif

#if USE_MEMORYPOOL
	  case TN_MPL:
		{
			IMPORT ER knl_memorypool_getname(ID id, UB **name);
			ercd = knl_memorypool_getname(objid, name);
			break;
		}
#endif

#if USE_FIX_MEMORYPOOL
	  case TN_MPF:
		{
			IMPORT ER knl_fix_memorypool_getname(ID id, UB **name);
			ercd = knl_fix_memorypool_getname(objid, name);
			break;
		}
#endif

#if USE_CYCLICHANDLER
	  case TN_CYC:
		{
			IMPORT ER knl_cyclichandler_getname(ID id, UB **name);
			ercd = knl_cyclichandler_getname(objid, name);
			break;
		}
#endif

#if USE_ALARMHANDLER
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
#endif /* USE_OBJECT_NAME */


/**
 * @brief オブジェクト名の参照
 *
 * 指定したオブジェクトの名称（DS オブジェクト名）を dsname に
 * コピーします。
 *
 * @param type	オブジェクト種別（TN_TSK, TN_SEM 等）
 * @param id	オブジェクト ID
 * @param dsname	名称のコピー先（OBJECT_NAME_LENGTH バイト以上）
 * @retval E_OK	正常終了
 * @retval E_PAR	type が不正
 * @retval E_NOEXS	対象オブジェクトが存在しない
 * @retval E_OBJ	TA_DSNAME 属性が指定されていない
 * @retval E_NOSPT	オブジェクト名機能が未サポート（USE_OBJECT_NAME 無効時）
 */
SYSCALL ER td_ref_dsname( UINT type, ID id, UB *dsname )
{
#if USE_OBJECT_NAME
	UB	*name_cb;
	ER	ercd;

	ercd = knl_object_getname(type, id, &name_cb);
	if (ercd == E_OK) {
		knl_strncpy((char*)dsname, (char*)name_cb, OBJECT_NAME_LENGTH);
	}

	return ercd;
#else
	return E_NOSPT;
#endif /* USE_OBJECT_NAME */
}


/**
 * @brief オブジェクト名の設定
 *
 * 指定したオブジェクトの名称（DS オブジェクト名）を dsname の内容で
 * 書き換えます。
 *
 * @param type	オブジェクト種別（TN_TSK, TN_SEM 等）
 * @param id	オブジェクト ID
 * @param dsname	設定する名称
 * @retval E_OK	正常終了
 * @retval E_PAR	type が不正
 * @retval E_NOEXS	対象オブジェクトが存在しない
 * @retval E_OBJ	TA_DSNAME 属性が指定されていない
 * @retval E_NOSPT	オブジェクト名機能が未サポート（USE_OBJECT_NAME 無効時）
 */
SYSCALL ER td_set_dsname( UINT type, ID id, CONST UB *dsname )
{
#if USE_OBJECT_NAME
	UB	*name_cb;
	ER	ercd;

	ercd = knl_object_getname(type, id, &name_cb);
	if (ercd == E_OK) {
		knl_strncpy((char*)name_cb, (char*)dsname, OBJECT_NAME_LENGTH);
	}

	return ercd;
#else
	return E_NOSPT;
#endif /* USE_OBJECT_NAME */
}

#endif /* USE_DBGSPT */
