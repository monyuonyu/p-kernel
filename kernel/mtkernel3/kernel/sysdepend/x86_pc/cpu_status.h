/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_status.h
 *	クリティカルセクション・システム状態制御（x86 ベアメタルポート）
 *
 *	割込み禁止は実 cli/sti（EFLAGS.IF）。プリエンプションは
 *	END_CRITICAL_SECTION のディスパッチ判定で発生します。
 */

#ifndef _SYSDEPEND_TARGET_CPUSTATUS_
#define _SYSDEPEND_TARGET_CPUSTATUS_

#include <tk/syslib.h>
#include <sys/sysdef.h>
#include "sysdepend.h"

/*
 * クリティカルセクションの開始/終了
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
#define ENABLE_INTERRUPT		{ __asm__ volatile("sti" ::: "memory"); }
#define DISABLE_INTERRUPT		{ __asm__ volatile("cli" ::: "memory"); }
#define ENABLE_INTERRUPT_UPTO(level)	ENABLE_INTERRUPT

/*
 * タスク独立部（割込みハンドラ実行中）の管理
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
 * 現在の EFLAGS の取得（in_ddsp / in_loc の判定用）
 */
Inline UW knl_getEFLAGS( void )
{
	UW eflags;
	__asm__ volatile ("pushfl\n\tpopl %0" : "=r"(eflags));
	return eflags;
}

/*
 * システム状態の判定
 */
#define in_indp()	( knl_isTaskIndependent() || knl_ctxtsk == NULL )

#define in_ddsp()	( knl_dispatch_disabled		\
			|| in_indp()			\
			|| isDI(knl_getEFLAGS()) )

#define in_loc()	( isDI(knl_getEFLAGS())		\
			|| in_indp() )

#define in_qtsk()	( knl_ctxtsk->sysmode > knl_ctxtsk->isysmode )

/*
 * ディスパッチャの起動（dispatch.S のエントリを直接呼び出す）
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
