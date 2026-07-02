/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux AArch64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_status.h
 *	クリティカルセクション・システム状態制御
 *	（Linux AArch64 ユーザモードポート）
 *
 *	割込み禁止はソフトウェアフラグ方式（syslib.h の disint/enaint）。
 *	プリエンプションは END_CRITICAL_SECTION の判定でのみ発生する
 *	「安全点方式」です。SIGALRM ハンドラはコンテキストを直接
 *	書き換えません。
 */

#ifndef _SYSDEPEND_TARGET_CPUSTATUS_
#define _SYSDEPEND_TARGET_CPUSTATUS_

#include <tk/syslib.h>
#include <sys/sysdef.h>
#include "sysdepend.h"

/*
 * クリティカルセクションの開始/終了
 *	終了時、以下の全条件が成立すればディスパッチを実行します:
 *	  - 開始時点で割込みが許可されていた（最外殻の END である）
 *	  - 実行中タスクと次に実行すべきタスクが異なる
 *	  - タスク独立部（擬似割込みハンドラ内）でない
 *	  - ディスパッチ禁止状態でない
 */
#define BEGIN_CRITICAL_SECTION	{ UINT _intsts_ = disint();
#define END_CRITICAL_SECTION	if ( !isDI(_intsts_)			\
				  && knl_ctxtsk != knl_schedtsk		\
				  && !knl_isTaskIndependent()		\
				  && !knl_dispatch_disabled ) {		\
					knl_dispatch();			\
				}					\
				enaint(_intsts_); }

/*
 * 割込み禁止区間の開始/終了（ディスパッチ判定なし）
 */
#define BEGIN_DISABLE_INTERRUPT	{ UINT _intsts_ = disint();
#define END_DISABLE_INTERRUPT	enaint(_intsts_); }

/*
 * 割込みの許可/禁止
 */
#define ENABLE_INTERRUPT		{ enaint(0); }
#define DISABLE_INTERRUPT		{ disint(); }
#define ENABLE_INTERRUPT_UPTO(level)	{ enaint(0); }

/*
 * タスク独立部（擬似割込みハンドラ実行中）の管理
 */
IMPORT	W	knl_taskindp;		/* タスク独立部ネストカウンタ */

Inline BOOL knl_isTaskIndependent( void )
{
	return ( knl_taskindp > 0 )? TRUE: FALSE;
}

Inline void knl_EnterTaskIndependent( void )
{
	knl_taskindp++;
}

Inline void knl_LeaveTaskIndependent( void )
{
	knl_taskindp--;
}

#define ENTER_TASK_INDEPENDENT	{ knl_EnterTaskIndependent(); }
#define LEAVE_TASK_INDEPENDENT	{ knl_LeaveTaskIndependent(); }

/*
 * システム状態の判定
 *	in_indp() : タスク独立部実行中か
 *	in_ddsp() : ディスパッチ禁止状態か（割込み禁止も含む）
 *	in_loc()  : 割込み禁止状態か
 *	in_qtsk() : 準タスク部実行中か
 */
#define in_indp()	( knl_isTaskIndependent() || knl_ctxtsk == NULL )

#define in_ddsp()	( knl_dispatch_disabled		\
			|| in_indp()			\
			|| isDI(arch_irq_disabled_flag) )

#define in_loc()	( isDI(arch_irq_disabled_flag)	\
			|| in_indp() )

#define in_qtsk()	( knl_ctxtsk->sysmode > knl_ctxtsk->isysmode )

/*
 * ディスパッチャの起動
 *	SVC 命令の代わりに dispatch.S のエントリを直接呼び出します。
 */
Inline void knl_force_dispatch( void )
{
	knl_dispatch_to_schedtsk();
}

Inline void knl_dispatch( void )
{
	knl_dispatch_entry();
}

#endif /* _SYSDEPEND_TARGET_CPUSTATUS_ */
