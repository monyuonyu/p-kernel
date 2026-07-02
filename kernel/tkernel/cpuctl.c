/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.03
 *
 *    Copyright (C) 2006-2021 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2021/03/31.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	cpuctl.c
 * @brief	CPU 制御機能
 *
 * ディスパッチの禁止・許可、タスクのレジスタ操作
 * （汎用レジスタ・コプロセッサレジスタ）の各 API を提供します。
 */

#include "kernel.h"
#include "task.h"
#include "check.h"


#ifdef USE_FUNC_TK_DIS_DSP
/* ------------------------------------------------------------------------ */
/**
 * @brief ディスパッチの禁止
 *
 * タスクのディスパッチ（切り替え）を禁止します。
 * すでに禁止状態でも重ねて呼び出せます（ネスト管理はされません）。
 *
 * @retval E_OK	正常終了
 * @retval E_CTX	コンテキストエラー（タスク独立部または割込み禁止中からの呼び出し）
 */
SYSCALL ER tk_dis_dsp( void )
{
	CHECK_CTX(!in_loc());

	knl_dispatch_disabled = DDS_DISABLE;

	return E_OK;
}
#endif /* USE_FUNC_TK_DIS_DSP */


/* ------------------------------------------------------------------------ */
/**
 * @brief ディスパッチの許可
 *
 * ディスパッチ禁止状態を解除します。解除時点で実行中タスクと
 * 最高優先タスクが異なる場合は、直ちにディスパッチを行います。
 *
 * @retval E_OK	正常終了
 * @retval E_CTX	コンテキストエラー（タスク独立部または割込み禁止中からの呼び出し）
 */
#ifdef USE_FUNC_TK_ENA_DSP
SYSCALL ER tk_ena_dsp( void )
{
	CHECK_CTX(!in_loc());

	knl_dispatch_disabled = DDS_ENABLE;
	if ( knl_ctxtsk != knl_schedtsk ) {
		knl_dispatch();
	}

	return E_OK;
}
#endif /* USE_FUNC_TK_ENA_DSP */

#if TK_SUPPORT_REGOPS
#ifdef USE_FUNC_TK_SET_REG
/* ------------------------------------------------------------------------ */
/**
 * @brief タスクレジスタ内容の設定
 *
 * 指定タスクの汎用レジスタ・例外関連レジスタ・制御レジスタを設定します。
 * 自タスクは指定できません。
 *
 * @param tskid	対象タスクの ID
 * @param pk_regs	設定する汎用レジスタの内容（NULL 可、その場合は設定しない）
 * @param pk_eit	設定する例外関連レジスタの内容（NULL 可）
 * @param pk_cregs	設定する制御レジスタの内容（NULL 可）
 * @retval E_OK	正常終了
 * @retval E_ID	tskid が不正
 * @retval E_OBJ	自タスクを指定した
 * @retval E_NOEXS	対象タスクが存在しない
 * @retval E_CTX	タスク独立部からの呼び出し
 */
SYSCALL ER tk_set_reg( ID tskid,
		CONST T_REGS *pk_regs, CONST T_EIT *pk_eit, CONST T_CREGS *pk_cregs )
{
	TCB		*tcb;
	ER		ercd = E_OK;

	CHECK_INTSK();
	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else {
		knl_set_reg(tcb, pk_regs, pk_eit, pk_cregs);
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_SET_REG */

#ifdef USE_FUNC_TK_GET_REG
/* ------------------------------------------------------------------------ */
/**
 * @brief タスクレジスタ内容の取得
 *
 * 指定タスクの汎用レジスタ・例外関連レジスタ・制御レジスタの内容を
 * 取得します。自タスクは指定できません。
 *
 * @param tskid	対象タスクの ID
 * @param pk_regs	汎用レジスタの内容を返す領域（NULL 可）
 * @param pk_eit	例外関連レジスタの内容を返す領域（NULL 可）
 * @param pk_cregs	制御レジスタの内容を返す領域（NULL 可）
 * @retval E_OK	正常終了
 * @retval E_ID	tskid が不正
 * @retval E_OBJ	自タスクを指定した
 * @retval E_NOEXS	対象タスクが存在しない
 * @retval E_CTX	タスク独立部からの呼び出し
 */
SYSCALL ER tk_get_reg( ID tskid, T_REGS *pk_regs, T_EIT *pk_eit, T_CREGS *pk_cregs )
{
	TCB		*tcb;
	ER		ercd = E_OK;

	CHECK_INTSK();
	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else {
		knl_get_reg(tcb, pk_regs, pk_eit, pk_cregs);
	}
	END_CRITICAL_SECTION;

	return ercd;
}

#endif /* USE_FUNC_TK_GET_REG */
#endif /* TK_SUPPORT_REGOPS */

#if NUM_COPROCESSOR > 0
#ifdef USE_FUNC_TK_SET_CPR
/* ------------------------------------------------------------------------ */
/**
 * @brief コプロセッサレジスタ内容の設定
 *
 * 指定タスクのコプロセッサレジスタを設定します。対象タスクが
 * 該当コプロセッサの使用属性（TA_COPn）を持たない場合はエラーです。
 *
 * @param tskid	対象タスクの ID（自タスクは指定不可）
 * @param copno	コプロセッサ番号（0 〜 NUM_COPROCESSOR-1）
 * @param pk_copregs	設定するコプロセッサレジスタの内容
 * @retval E_OK	正常終了
 * @retval E_ID	tskid が不正
 * @retval E_OBJ	自タスクを指定した
 * @retval E_PAR	copno が不正、または対象タスクに該当コプロセッサ属性がない
 * @retval E_NOEXS	対象タスクが存在しない
 * @retval E_CTX	タスク独立部からの呼び出し
 */
SYSCALL ER tk_set_cpr( ID tskid, INT copno, CONST T_COPREGS *pk_copregs )
{
	TCB		*tcb;
	ER		ercd = E_OK;

	CHECK_INTSK();
	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);
	if((copno < 0) || (copno >= NUM_COPROCESSOR)
		|| !(tcb->tskatr & (TA_COP0 << copno))) {
		return E_PAR;
	}

	BEGIN_CRITICAL_SECTION;
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else {
		ercd = knl_set_cpr(tcb, copno, pk_copregs);
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_SET_CPR */

#ifdef USE_FUNC_TK_GET_CPR
/* ------------------------------------------------------------------------ */
/**
 * @brief コプロセッサレジスタ内容の取得
 *
 * 指定タスクのコプロセッサレジスタの内容を取得します。対象タスクが
 * 該当コプロセッサの使用属性（TA_COPn）を持たない場合はエラーです。
 *
 * @param tskid	対象タスクの ID（自タスクは指定不可）
 * @param copno	コプロセッサ番号（0 〜 NUM_COPROCESSOR-1）
 * @param pk_copregs	コプロセッサレジスタの内容を返す領域
 * @retval E_OK	正常終了
 * @retval E_ID	tskid が不正
 * @retval E_OBJ	自タスクを指定した
 * @retval E_PAR	copno が不正、または対象タスクに該当コプロセッサ属性がない
 * @retval E_NOEXS	対象タスクが存在しない
 * @retval E_CTX	タスク独立部からの呼び出し
 */
SYSCALL ER tk_get_cpr( ID tskid, INT copno, T_COPREGS *pk_copregs )
{
	TCB		*tcb;
	ER		ercd = E_OK;

	CHECK_INTSK();
	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);
	if((copno < 0) || (copno >= NUM_COPROCESSOR)
		|| !(tcb->tskatr & (TA_COP0 << copno))) {
		return E_PAR;
	}

	BEGIN_CRITICAL_SECTION;
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else {
		ercd = knl_get_cpr(tcb, copno, pk_copregs);
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_GET_CPR */
#endif /* NUM_COPROCESSOR > 0 */

#if USE_DBGSPT
/* ------------------------------------------------------------------------ */
/*
 *	デバッガサポート機能
 */

#if TK_SUPPORT_REGOPS
#ifdef USE_FUNC_TD_SET_REG
/* ------------------------------------------------------------------------ */
/**
 * @brief タスクレジスタの設定（デバッガサポート）
 *
 * 指定タスクのレジスタ内容を設定します。実行中タスク
 * （knl_ctxtsk）は対象にできません。
 *
 * @param tskid	対象タスクの ID
 * @param regs	設定する汎用レジスタの内容（NULL 可）
 * @param eit	設定する例外関連レジスタの内容（NULL 可）
 * @param cregs	設定する制御レジスタの内容（NULL 可）
 * @retval E_OK	正常終了
 * @retval E_ID	tskid が不正
 * @retval E_OBJ	対象タスクが実行中
 * @retval E_NOEXS	対象タスクが存在しない
 */
SYSCALL ER td_set_reg( ID tskid, CONST T_REGS *regs, CONST T_EIT *eit, CONST T_CREGS *cregs )
{
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_TSKID(tskid);

	tcb = get_tcb(tskid);
	if ( tcb == knl_ctxtsk ) {
		return E_OBJ;
	}

	BEGIN_DISABLE_INTERRUPT;
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else {
		knl_set_reg(tcb, regs, eit, cregs);
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_SET_REG */

#ifdef USE_FUNC_TD_GET_REG
/* ------------------------------------------------------------------------ */
/**
 * @brief タスクレジスタの取得（デバッガサポート）
 *
 * 指定タスクのレジスタ内容を取得します。実行中タスク
 * （knl_ctxtsk）は対象にできません。
 *
 * @param tskid	対象タスクの ID
 * @param regs	汎用レジスタの内容を返す領域（NULL 可）
 * @param eit	例外関連レジスタの内容を返す領域（NULL 可）
 * @param cregs	制御レジスタの内容を返す領域（NULL 可）
 * @retval E_OK	正常終了
 * @retval E_ID	tskid が不正
 * @retval E_OBJ	対象タスクが実行中
 * @retval E_NOEXS	対象タスクが存在しない
 */
SYSCALL ER td_get_reg( ID tskid, T_REGS *regs, T_EIT *eit, T_CREGS *cregs )
{
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_TSKID(tskid);

	tcb = get_tcb(tskid);
	if ( tcb == knl_ctxtsk ) {
		return E_OBJ;
	}

	BEGIN_DISABLE_INTERRUPT;
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else {
		knl_get_reg(tcb, regs, eit, cregs);
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_GET_REG */
#endif /* TK_SUPPORT_REGOPS */

#endif /* USE_DBGSPT */
