/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.06
 *
 *    Copyright (C) 2006-2022 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2022/10.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	task_manage.c
 * @brief	タスク管理機能
 *
 * タスクの生成・削除・起動・終了、優先度変更、実行可能キューの
 * 回転、タスク状態参照、待ち解除など、タスク管理系システムコール
 * （tk_cre_tsk、tk_del_tsk、tk_sta_tsk、tk_ext_tsk、tk_exd_tsk、
 * tk_ter_tsk、tk_chg_pri、tk_rot_rdq、tk_get_tid、tk_ref_tsk、
 * tk_rel_wai）とデバッグ支援機能を実装します。
 */

#include "kernel.h"
#include "wait.h"
#include "check.h"
#include <tm/tmonitor.h>

#include "../sysdepend/cpu_task.h"

/**
 * @brief タスクの生成
 *
 * 未使用キューから TCB を獲得してタスクを生成し、休止状態にします。
 * TA_USERBUF 指定時はユーザ指定バッファをシステムスタックとして
 * 使用し、未指定時はシステムメモリからスタック領域を確保します。
 *
 * @param pk_ctsk タスク生成情報
 * @return 生成したタスクの ID（正の値）、またはエラーコード
 * @retval E_RSATR	不正なタスク属性
 * @retval E_PAR	スタックサイズ等のパラメータ不正
 * @retval E_NOMEM	スタック領域の確保失敗
 * @retval E_LIMIT	タスク数が上限（NUM_TSKID）を超過
 */
SYSCALL ID tk_cre_tsk( CONST T_CTSK *pk_ctsk )
{
#if CHK_RSATR
	const ATR VALID_TSKATR = {	/* 有効なタスク属性値 */
		 TA_HLNG
		|TA_RNG3
		|TA_USERBUF
		|TA_COPS
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
	};
#endif
	TCB	*tcb;
	W	sstksz;
	void	*stack;
	ER	ercd;

	CHECK_RSATR(pk_ctsk->tskatr, VALID_TSKATR);
#if !USE_IMALLOC
	/* Imalloc なしの構成では TA_USERBUF の指定が必須 */
	CHECK_PAR((pk_ctsk->tskatr & TA_USERBUF) != 0);
#endif
	CHECK_PAR(pk_ctsk->stksz >= 0);
	CHECK_PRI(pk_ctsk->itskpri);

	if ( (pk_ctsk->tskatr & TA_USERBUF) != 0 ) {
		/* ユーザ指定バッファを使用 */
		sstksz = pk_ctsk->stksz;
		CHECK_PAR(sstksz >= MIN_SYS_STACK_SIZE);
		stack = pk_ctsk->bufptr;
	} else {
#if USE_IMALLOC
		/* システムスタック領域の確保 */
		sstksz = pk_ctsk->stksz + DEFAULT_SYS_STKSZ;
		sstksz  = (sstksz  + 7) / 8 * 8;	/* 8 の倍数に切り上げ */
		stack = knl_Imalloc((UW)sstksz);
		if ( stack == NULL ) {
			return E_NOMEM;
		}
#endif
	}

	BEGIN_CRITICAL_SECTION;
	/* 未使用キューから制御ブロックを獲得 */
	tcb = (TCB*)QueRemoveNext(&knl_free_tcb);
	if ( tcb == NULL ) {
		ercd = E_LIMIT;
		goto error_exit;
	}

	/* 制御ブロックの初期化 */
	tcb->exinf     = pk_ctsk->exinf;
	tcb->tskatr    = pk_ctsk->tskatr;
	tcb->task      = pk_ctsk->task;
	tcb->ipriority = (UB)int_priority(pk_ctsk->itskpri);
	tcb->sstksz    = sstksz;
#if USE_OBJECT_NAME
	if ( (pk_ctsk->tskatr & TA_DSNAME) != 0 ) {
		knl_strncpy((char*)tcb->name, (char*)pk_ctsk->dsname, OBJECT_NAME_LENGTH);
	}
#endif

	/* スタックポインタの設定 */
	tcb->isstack = (VB*)stack + sstksz;

	/* タスク動作モードの初期値設定 */
	tcb->isysmode = 1;
	tcb->sysmode  = 1;

	/* 休止状態へ遷移 */
	knl_make_dormant(tcb);

	ercd = tcb->tskid;

    error_exit:
	END_CRITICAL_SECTION;

#if USE_IMALLOC
	if ( (ercd < E_OK) && ((pk_ctsk->tskatr & TA_USERBUF) == 0) ) {
		knl_Ifree(stack);
	}
#endif

	return ercd;
}

/**
 * @brief タスク削除の内部処理
 *
 * システムスタック領域を解放（ユーザバッファ未使用時のみ）し、
 * TCB を未使用キューへ返却して未登録状態にします。
 *
 * @param tcb 対象タスクの TCB
 * @note クリティカルセクション内から呼び出すこと。
 */
LOCAL void knl_del_tsk( TCB *tcb )
{
#if USE_IMALLOC
	if ( (tcb->tskatr & TA_USERBUF) == 0 ) {
		/* ユーザバッファ未使用の場合 */
		/* システムスタックの解放 */
		void *stack = (VB*)tcb->isstack - tcb->sstksz;
		knl_Ifree(stack);
	}
#endif

	/* 制御ブロックを未使用キューへ返却 */
	QueInsert(&tcb->tskque, &knl_free_tcb);
	tcb->state = TS_NONEXIST;
}

#ifdef USE_FUNC_TK_DEL_TSK
/**
 * @brief タスクの削除
 *
 * 休止状態のタスクを削除し、TCB を未使用キューへ返却します。
 * 自タスクは指定できません。
 *
 * @param tskid 対象タスクの ID
 * @retval E_OK		正常終了
 * @retval E_ID		不正なタスク ID（自タスク指定を含む）
 * @retval E_NOEXS	タスクが存在しない
 * @retval E_OBJ	タスクが休止状態でない
 */
SYSCALL ER tk_del_tsk( ID tskid )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;

	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	state = (TSTAT)tcb->state;
	if ( state != TS_DORMANT ) {
		ercd = ( state == TS_NONEXIST )? E_NOEXS: E_OBJ;
	} else {
		knl_del_tsk(tcb);
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_TSK */

/* ------------------------------------------------------------------------ */

/**
 * @brief タスクの起動
 *
 * 休止状態のタスクにタスク起動コード 'stacd' を渡して起動し、
 * 実行可能状態にします。自タスクは指定できません。
 *
 * @param tskid 対象タスクの ID
 * @param stacd タスク起動コード
 * @retval E_OK		正常終了
 * @retval E_ID		不正なタスク ID（自タスク指定を含む）
 * @retval E_NOEXS	タスクが存在しない
 * @retval E_OBJ	タスクが休止状態でない
 */
SYSCALL ER tk_sta_tsk( ID tskid, INT stacd )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;

	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	state = (TSTAT)tcb->state;
	if ( state != TS_DORMANT ) {
		ercd = ( state == TS_NONEXIST )? E_NOEXS: E_OBJ;
	} else {
		knl_setup_stacd(tcb, stacd);
		knl_make_ready(tcb);
	}
	END_CRITICAL_SECTION;

	return ercd;
}

/**
 * @brief タスク終了の内部処理
 *
 * タスクを実行可能キューまたは待ち状態から外し、待ち解除フックが
 * あれば実行します。保有しているミューテックスをすべて解放し、
 * タスクコンテキストの後始末を行います。
 *
 * @param tcb 対象タスクの TCB
 * @note クリティカルセクション内から呼び出すこと。
 */
LOCAL void knl_ter_tsk( TCB *tcb )
{
	TSTAT	state;

	state = (TSTAT)tcb->state;
	if ( state == TS_READY ) {
		knl_make_non_ready(tcb);

	} else if ( (state & TS_WAIT) != 0 ) {
		knl_wait_cancel(tcb);
		if ( tcb->wspec->rel_wai_hook != NULL ) {
			(*tcb->wspec->rel_wai_hook)(tcb);
		}
	}

#if USE_MUTEX == 1
	/* 保有ミューテックスの解放 */
	knl_signal_all_mutex(tcb);
#endif

	knl_cleanup_context(tcb);
}

#ifdef USE_FUNC_TK_EXT_TSK
/**
 * @brief 自タスクの終了
 *
 * 自タスクを終了して休止状態にし、強制ディスパッチを行います。
 * 本関数からは復帰しません。タスク独立部からの呼び出しは
 * コンテキストエラーです（CHK_CTX2 有効時は無限ループで停止）。
 *
 * @note ディスパッチ禁止中の呼び出しは CHK_CTX1 有効時に警告
 *       メッセージを出力しますが、処理は続行されます。
 */
SYSCALL void tk_ext_tsk( void )
{
#ifdef DORMANT_STACK_SIZE
	/* 'knl_make_dormant' 内で使用するスタックの破壊を避けるため、
	   スタック上にダミー領域を確保する。 */
	volatile VB _dummy[DORMANT_STACK_SIZE];
#endif

	/* コンテキストエラーの確認 */
#if CHK_CTX2
	if ( in_indp() ) {
		SYSTEM_MESSAGE("tk_ext_tsk was called in the task independent\n");
		while(1);
		return;
	}
#endif
#if CHK_CTX1
	if ( in_ddsp() ) {
		SYSTEM_MESSAGE("tk_ext_tsk was called in the dispatch disabled\n");
	}
#endif

	DISABLE_INTERRUPT;
	knl_ter_tsk(knl_ctxtsk);
	knl_make_dormant(knl_ctxtsk);

	knl_force_dispatch();
	/* ここへは戻らない */

#ifdef DORMANT_STACK_SIZE
	/* 警告の回避（このコードは実行されない） */
	_dummy[0] = _dummy[0];
#endif
}
#endif /* USE_FUNC_TK_EXT_TSK */

#ifdef USE_FUNC_TK_EXD_TSK
/**
 * @brief 自タスクの終了と削除
 *
 * 自タスクを終了・削除して TCB を未使用キューへ返却し、
 * 強制ディスパッチを行います。本関数からは復帰しません。
 * タスク独立部からの呼び出しはコンテキストエラーです。
 *
 * @note ディスパッチ禁止中の呼び出しは CHK_CTX1 有効時に警告
 *       メッセージを出力しますが、処理は続行されます。
 */
SYSCALL void tk_exd_tsk( void )
{
	/* コンテキストエラーの確認 */
#if CHK_CTX2
	if ( in_indp() ) {
		SYSTEM_MESSAGE("tk_exd_tsk was called in the task independent\n");
		return;
	}
#endif
#if CHK_CTX1
	if ( in_ddsp() ) {
		SYSTEM_MESSAGE("tk_exd_tsk was called in the dispatch disabled\n");
	}
#endif

	DISABLE_INTERRUPT;
	knl_ter_tsk(knl_ctxtsk);
	knl_del_tsk(knl_ctxtsk);

	knl_force_dispatch();
	/* ここへは戻らない */
}
#endif /* USE_FUNC_TK_EXD_TSK */

#ifdef USE_FUNC_TK_TER_TSK
/**
 * @brief 他タスクの強制終了
 *
 * 指定タスクを強制終了して休止状態にします。自タスクは指定
 * できません。
 *
 * @param tskid 対象タスクの ID
 * @retval E_OK		正常終了
 * @retval E_ID		不正なタスク ID（自タスク指定を含む）
 * @retval E_NOEXS	タスクが存在しない
 * @retval E_OBJ	タスクが休止状態、またはカーネルロック中で
 *			終了できない状態
 */
SYSCALL ER tk_ter_tsk( ID tskid )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;

	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	state = (TSTAT)tcb->state;
	if ( !knl_task_alive(state) ) {
		ercd = ( state == TS_NONEXIST )? E_NOEXS: E_OBJ;
	} else if ( tcb->klocked ) {
		/* 通常はこの状態にはならない。
		 * 仮想記憶システムでページイン待ちの状態にある
		 * タスクを終了させようとした場合に、この状態と
		 * なることがある。
		 */
		ercd = E_OBJ;
	} else {
		knl_ter_tsk(tcb);
		knl_make_dormant(tcb);
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_TER_TSK */

/* ------------------------------------------------------------------------ */

#ifdef USE_FUNC_TK_CHG_PRI
/**
 * @brief タスク優先度の変更
 *
 * 指定タスクのベース優先度を 'tskpri' に変更します。TPRI_INI
 * 指定時はタスク起動時の初期優先度に戻します。ミューテックス使用時
 * は上限優先度の制約を検査し、優先度継承を考慮した現在優先度を
 * 適用します。
 *
 * @param tskid  対象タスクの ID（TSK_SELF で自タスク）
 * @param tskpri 新しい優先度（TPRI_INI で初期優先度）
 * @retval E_OK		正常終了
 * @retval E_ID		不正なタスク ID
 * @retval E_PAR	不正な優先度
 * @retval E_NOEXS	タスクが存在しない
 * @retval E_ILUSE	ミューテックスの上限優先度違反
 */
SYSCALL ER tk_chg_pri( ID tskid, PRI tskpri )
{
	TCB	*tcb;
	INT	priority;
	ER	ercd;

	CHECK_TSKID_SELF(tskid);
	CHECK_PRI_INI(tskpri);

	tcb = get_tcb_self(tskid);

	BEGIN_CRITICAL_SECTION;
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
		goto error_exit;
	}

	/* 優先度を内部表現へ変換 */
	if ( tskpri == TPRI_INI ) {
		priority = tcb->ipriority;
	} else {
		priority = int_priority(tskpri);
	}

#if USE_MUTEX == 1
	/* ミューテックスによる優先度変更の制限 */
	ercd = knl_chg_pri_mutex(tcb, priority);
	if ( ercd < E_OK ) {
		goto error_exit;
	}

	tcb->bpriority = (UB)priority;
	priority = ercd;
#else
	tcb->bpriority = priority;
#endif

	/* 優先度の変更 */
	knl_change_task_priority(tcb, priority);

	ercd = E_OK;
    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_CHG_PRI */

#ifdef USE_FUNC_TK_ROT_RDQ
/**
 * @brief 実行可能キューの回転
 *
 * 優先度 'tskpri' の実行可能キューを回転します。TPRI_RUN 指定時は
 * 実行状態タスクの優先度（タスク独立部からの呼び出し時は最高
 * 優先度）のキューを回転します。
 *
 * @param tskpri 回転対象の優先度（TPRI_RUN で実行状態タスクの優先度）
 * @retval E_OK	 正常終了
 * @retval E_PAR 不正な優先度
 */
SYSCALL ER tk_rot_rdq( PRI tskpri )
{
	CHECK_PRI_RUN(tskpri);

	BEGIN_CRITICAL_SECTION;
	if ( tskpri == TPRI_RUN ) {
		if ( in_indp() ) {
			knl_rotate_ready_queue_run();
		} else {
			knl_rotate_ready_queue(knl_ctxtsk->priority);
		}
	} else {
		knl_rotate_ready_queue(int_priority(tskpri));
	}
	END_CRITICAL_SECTION;

	return E_OK;
}
#endif /* USE_FUNC_TK_ROT_RDQ */

/* ------------------------------------------------------------------------ */

#ifdef USE_FUNC_TK_GET_TID
/**
 * @brief 実行状態タスクの ID 取得
 *
 * 実行中タスクの ID を返します。実行中タスクが存在しない場合は
 * 0 を返します。
 *
 * @return 実行中タスクの ID、または 0
 */
SYSCALL ID tk_get_tid( void )
{
	return ( knl_ctxtsk == NULL )? 0: knl_ctxtsk->tskid;
}
#endif /* USE_FUNC_TK_GET_TID */

#ifdef USE_FUNC_TK_REF_TSK
/**
 * @brief タスク状態の参照
 *
 * 指定タスクの状態（タスク状態、待ち要因、優先度、起床要求数、
 * 強制待ち要求数など）を 'pk_rtsk' に格納します。
 *
 * @param tskid   対象タスクの ID（TSK_SELF で自タスク）
 * @param pk_rtsk タスク状態を格納するパケットへのポインタ
 * @retval E_OK		正常終了
 * @retval E_ID		不正なタスク ID
 * @retval E_NOEXS	タスクが存在しない
 */
SYSCALL ER tk_ref_tsk( ID tskid, T_RTSK *pk_rtsk )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;

	CHECK_TSKID_SELF(tskid);

	tcb = get_tcb_self(tskid);

	knl_memset(pk_rtsk, 0, sizeof(*pk_rtsk));

	BEGIN_CRITICAL_SECTION;
	state = (TSTAT)tcb->state;
	if ( state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else {
		if ( ( state == TS_READY ) && ( tcb == knl_ctxtsk ) ) {
			pk_rtsk->tskstat = TTS_RUN;
		} else {
			pk_rtsk->tskstat = (UINT)state << 1;
		}
		if ( (state & TS_WAIT) != 0 ) {
			pk_rtsk->tskwait = tcb->wspec->tskwait;
			pk_rtsk->wid     = tcb->wid;
		}
		pk_rtsk->exinf     = tcb->exinf;
		pk_rtsk->tskpri    = ext_tskpri(tcb->priority);
		pk_rtsk->tskbpri   = ext_tskpri(tcb->bpriority);
		pk_rtsk->wupcnt    = tcb->wupcnt;
		pk_rtsk->suscnt    = tcb->suscnt;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REF_TSK */

/* ------------------------------------------------------------------------ */


#ifdef USE_FUNC_TK_REL_WAI
/**
 * @brief 他タスクの待ち状態の強制解除
 *
 * 待ち状態にあるタスクの待ちを強制的に解除します。待ち解除された
 * タスク側には E_RLWAI が返されます。強制待ち状態（SUSPEND）は
 * 解除しません。
 *
 * @param tskid 対象タスクの ID
 * @retval E_OK		正常終了
 * @retval E_ID		不正なタスク ID
 * @retval E_NOEXS	タスクが存在しない
 * @retval E_OBJ	タスクが待ち状態でない
 */
SYSCALL ER tk_rel_wai( ID tskid )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;

	CHECK_TSKID(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	state = (TSTAT)tcb->state;
	if ( (state & TS_WAIT) == 0 ) {
		ercd = ( state == TS_NONEXIST )? E_NOEXS: E_OBJ;
	} else {
		knl_wait_release_ng(tcb, E_RLWAI);
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REL_WAI */

/* ------------------------------------------------------------------------ */
/*
 *	デバッグ支援機能
 */
#if USE_DBGSPT

#if USE_OBJECT_NAME
/**
 * @brief 制御ブロックからのオブジェクト名取得
 *
 * 指定タスクの TCB に登録されたオブジェクト名へのポインタを
 * '*name' に格納します。
 *
 * @param id   対象タスクの ID（TSK_SELF で自タスク）
 * @param name オブジェクト名へのポインタを格納する変数のアドレス
 * @retval E_OK		正常終了
 * @retval E_ID		不正なタスク ID
 * @retval E_NOEXS	タスクが存在しない
 * @retval E_OBJ	TA_DSNAME 属性が指定されていない
 */
EXPORT ER knl_task_getname(ID id, UB **name)
{
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_TSKID_SELF(id);

	BEGIN_DISABLE_INTERRUPT;
	tcb = get_tcb_self(id);
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (tcb->tskatr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = tcb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */

#ifdef USE_FUNC_TD_LST_TSK
/**
 * @brief タスク ID 一覧の参照
 *
 * 登録済み（未登録状態以外）のタスクの ID を最大 'nent' 個まで
 * 'list' に格納します。
 *
 * @param list タスク ID を格納する配列
 * @param nent 'list' に格納可能なエントリ数
 * @return 登録済みタスク数（nent を超える場合あり）
 */
SYSCALL INT td_lst_tsk( ID list[], INT nent )
{
	TCB	*tcb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_tcb_table + NUM_TSKID;
	for ( tcb = knl_tcb_table; tcb < end; tcb++ ) {
		if ( tcb->state == TS_NONEXIST ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = tcb->tskid;
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_TSK */

#ifdef USE_FUNC_TD_REF_TSK
/**
 * @brief タスク状態の参照（デバッグ支援）
 *
 * 指定タスクの状態を 'pk_rtsk' に格納します。tk_ref_tsk の情報に
 * 加え、タスク起動アドレス・スタックサイズ・スタック初期値も
 * 取得します。
 *
 * @param tskid   対象タスクの ID（TSK_SELF で自タスク）
 * @param pk_rtsk タスク状態を格納するパケットへのポインタ
 * @retval E_OK		正常終了
 * @retval E_ID		不正なタスク ID
 * @retval E_NOEXS	タスクが存在しない
 */
SYSCALL ER td_ref_tsk( ID tskid, TD_RTSK *pk_rtsk )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;

	CHECK_TSKID_SELF(tskid);

	tcb = get_tcb_self(tskid);

	knl_memset(pk_rtsk, 0, sizeof(*pk_rtsk));

	BEGIN_DISABLE_INTERRUPT;
	state = (TSTAT)tcb->state;
	if ( state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else {
		if ( ( state == TS_READY ) && ( tcb == knl_ctxtsk ) ) {
			pk_rtsk->tskstat = TTS_RUN;
		} else {
			pk_rtsk->tskstat = (UINT)state << 1;
		}
		if ( (state & TS_WAIT) != 0 ) {
			pk_rtsk->tskwait = tcb->wspec->tskwait;
			pk_rtsk->wid     = tcb->wid;
		}
		pk_rtsk->exinf     = tcb->exinf;
		pk_rtsk->tskpri    = ext_tskpri(tcb->priority);
		pk_rtsk->tskbpri   = ext_tskpri(tcb->bpriority);
		pk_rtsk->wupcnt    = tcb->wupcnt;
		pk_rtsk->suscnt    = tcb->suscnt;

		pk_rtsk->task      = tcb->task;
		pk_rtsk->stksz     = tcb->sstksz;
		pk_rtsk->istack    = tcb->isstack;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_TSK */

#ifdef USE_FUNC_TD_INF_TSK
/**
 * @brief タスク統計情報の取得
 *
 * 指定タスクの累積動作時間（システムレベル・ユーザレベル）を
 * 'pk_itsk' に格納します。'clr' が TRUE の場合は取得後に統計情報を
 * クリアします。
 *
 * @param tskid   対象タスクの ID（TSK_SELF で自タスク）
 * @param pk_itsk 統計情報を格納するパケットへのポインタ
 * @param clr     取得後に統計情報をクリアするかどうか
 * @retval E_OK		正常終了
 * @retval E_ID		不正なタスク ID
 * @retval E_NOEXS	タスクが存在しない
 */
SYSCALL ER td_inf_tsk( ID tskid, TD_ITSK *pk_itsk, BOOL clr )
{
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_TSKID_SELF(tskid);

	tcb = get_tcb_self(tskid);

	BEGIN_DISABLE_INTERRUPT;
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else {
		pk_itsk->stime = tcb->stime;
		pk_itsk->utime = tcb->utime;
		if ( clr ) {
			tcb->stime = 0;
			tcb->utime = 0;
		}
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_INF_TSK */

#endif /* USE_DBGSPT */
