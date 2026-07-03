/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.07.B0
 *
 *    Copyright (C) 2006-2023 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2023/11.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	kernel.h
 * @brief	micro T-Kernel 共通定義
 *
 * カーネル内部で共通に使用する定義を集約したヘッダです。
 * タスク制御ブロック（TCB）の定義、ディスパッチ禁止状態の定数、
 * カーネルグローバル変数（knl_ctxtsk / knl_schedtsk 等）、および
 * 各カーネルオブジェクト初期化関数やシステム依存部ルーチンの
 * プロトタイプ宣言を提供します。
 */

#ifndef _KERNEL_
#define _KERNEL_

/* p-kernel 変更: <config.h> を最初に読む。
 * 元の並びでは <tk/typedef.h>（25行目）が処理される時点で
 * USE_STDINC_STDINT が未定義（=0 扱い）となり、W/UW が
 * signed/unsigned long にフォールバックする。32bit MCU では
 * long == int で顕在化しないが、LP64 ホストでは <tk/tkernel.h>
 * 経由（config が先に入る）の TU と型幅が食い違い、TCB 等の
 * レイアウト不一致を起こす。config を先頭で読めば全 TU で
 * 一貫して int32_t ベースになる。 */
#include <config.h>

#include <sys/machine.h>
#include <sys/queue.h>

#include <tk/typedef.h>
#include <tk/errno.h>
#include <tk/syscall.h>
#include <tk/dbgspt.h>

#include "tstdlib.h"

typedef struct task_control_block	TCB;

#include "../tkernel/timer.h"
#include "../tkernel/winfo.h"
#include "../tkernel/mutex.h"

#include "../sysdepend/sys_msg.h"
#include "../sysdepend/cpu_status.h"
#include "../sysdepend/sysdepend.h"

#define SYSCALL		EXPORT		/* システムコール関数の定義用 */

/* ユーザ定義ハンドラの呼び出し（サブシステムコール、タイムイベントハンドラ） */
# define CallUserHandlerP1(   p1,         hdr, cb)	(*(hdr))(p1)
# define CallUserHandlerP2(   p1, p2,     hdr, cb)	(*(hdr))(p1, p2)
# define CallUserHandlerP3(   p1, p2, p3, hdr, cb)	(*(hdr))(p1, p2, p3)

/**
 * @brief タスク制御ブロック（TCB）
 *
 * タスクごとにカーネルが保持する管理情報です。生成時の属性、
 * 現在の優先度・状態、待ち情報、スタック情報などを格納します。
 */
struct task_control_block {
	QUEUE	tskque;		/* タスクキュー */
	ID	tskid;		/* タスクID */
	void	*exinf;		/* 拡張情報 */
	ATR	tskatr;		/* タスク属性 */
	FP	task;		/* タスク起動アドレス */
	CTXB	tskctxb;	/* タスクコンテキストブロック */
	W	sstksz;		/* スタックサイズ */

	B	isysmode;	/* タスク動作モードの初期値 */
	H	sysmode;	/* タスク動作モード・準タスク部呼び出しレベル */

	UB	ipriority;	/* タスク起動時の優先度 */
	UB	bpriority;	/* ベース優先度 */
	UB	priority;	/* 現在の優先度 */

	UB /*TSTAT*/	state;	/* タスク状態（内部表現） */

	BOOL	klockwait:1;	/* カーネルロック待ち中は TRUE */
	BOOL	klocked:1;	/* カーネルロック保持中は TRUE */

	CONST WSPEC *wspec;	/* 待ち仕様 */
	ID	wid;		/* 待ち対象オブジェクトID */
	INT	wupcnt;		/* キューイングされた起床要求数 */
	INT	suscnt;		/* SUSPEND 要求のネスト数 */
	ER	*wercd;		/* 待ちエラーコード設定領域 */
	WINFO	winfo;		/* 待ち情報 */
	TMEB	wtmeb;		/* 待ちタイマイベントブロック */

	void	*isstack;	/* スタックポインタ初期値 */

#if USE_LEGACY_API && USE_RENDEZVOUS
	RNO	wrdvno;		/* ランデブ番号生成用 */
#endif
#if USE_MUTEX == 1
	MTXCB	*mtxlist;	/* 保持しているミューテックスのリスト */
#endif

#if USE_DBGSPT && defined(USE_FUNC_TD_INF_TSK)
	UW	stime;		/* システムレベル実行時間（ms） */
	UW	utime;		/* ユーザレベル実行時間（ms） */
#endif

#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* オブジェクト名 */
#endif

	/* p-kernel 拡張: 同一優先度内ラウンドロビン（SCHED_RR）
	 *	micro T-Kernel 2.0 ポートから移植。フィールドは TCB 末尾に
	 *	追加しているため、アセンブラが参照する先頭側のオフセット
	 *	（task / tskctxb）には影響しない。 */
	UB	sched_policy;		/* SCHED_FIFO または SCHED_RR */
	UH	time_slice;		/* タイムスライス [tick] */
	UH	remaining_slice;	/* 現スライスの残り [tick] */
};

/*
 * スケジューリングポリシー（p-kernel 拡張）
 */
#define SCHED_FIFO		0	/* 優先度プリエンプティブのみ（既定） */
#define SCHED_RR		1	/* 同一優先度内ラウンドロビン */
#define DEFAULT_TIME_SLICE	10	/* 10 tick × 10ms = スライス 100ms */


/*
 * タスクディスパッチ禁止状態
 *	0 = DDS_ENABLE		 : ディスパッチ許可
 *	1 = DDS_DISABLE_IMPLICIT : 暗黙の処理による禁止
 *	2 = DDS_DISABLE		 : tk_dis_dsp() による禁止
 *	|	|
 *	|	*.c で使用
 *	*.S で使用
 *	  --> アセンブラコードから参照されるため、これらのリテラル値を
 *	      変更してはならない
 *
 *	'knl_dispatch_disabled' は tk_dis_dsp() で設定されたディスパッチ
 *	禁止状態を記録します。遅延割込みを受け付ける CPU では、
 *	'knl_dispatch_disabled' だけではディスパッチ禁止状態を正しく
 *	参照できません。タスクディスパッチ状態の参照には 'in_ddsp()' を
 *	使用してください。'in_ddsp()' は CPU 依存部の定義ファイルで
 *	マクロとして定義されています。
 */
#define DDS_ENABLE		(0)
#define DDS_DISABLE_IMPLICIT	(1)	/* 暗黙の処理により設定 */
#define DDS_DISABLE		(2)	/* tk_dis_dsp() により設定 */
IMPORT INT	knl_dispatch_disabled;

/*
 * 実行中のタスク
 *	'knl_ctxtsk' は実行中のタスク（= CPU がコンテキストを保持している
 *	タスク）の TCB を指す変数です。システムコール処理中に、その
 *	システムコールを発行したタスクの情報を調べる場合は 'ctxtsk' を
 *	使用します。'ctxtsk' を変更するのはタスクディスパッチャのみです。
 */
IMPORT TCB	*knl_ctxtsk;

/*
 * 実行すべきタスク
 *	'knl_schedtsk' は実行すべきタスクの TCB を指す変数です。
 *	遅延ディスパッチやディスパッチ禁止によってディスパッチが
 *	保留されている間は 'ctxtsk' と一致しません。
 */
IMPORT TCB	*knl_schedtsk;

/*
 * カーネルオブジェクトの初期化（オブジェクトごと）
 */
IMPORT ER knl_task_initialize( void );
IMPORT ER knl_semaphore_initialize( void );
IMPORT ER knl_eventflag_initialize( void );
IMPORT ER knl_mailbox_initialize( void );
IMPORT ER knl_messagebuffer_initialize( void );
IMPORT ER knl_rendezvous_initialize( void );
IMPORT ER knl_mutex_initialize( void );
IMPORT ER knl_memorypool_initialize( void );
IMPORT ER knl_fix_memorypool_initialize( void );
IMPORT ER knl_cyclichandler_initialize( void );
IMPORT ER knl_alarmhandler_initialize( void );
IMPORT ER knl_subsystem_initialize( void );

/*
 * カーネルオブジェクトの一括初期化 (tkinit.c)
 */
IMPORT ER knl_init_object(void);

/*
 * デバイス管理の初期化 (device.c)
 */
IMPORT ER knl_initialize_devmgr( void );

/*
 * システムタイマ制御 (timer.c)
 */
IMPORT ER   knl_timer_startup( void );
IMPORT void knl_timer_shutdown( void );
IMPORT void knl_timer_handler( void );

/*
 * ミューテックス制御 (mutex.c)
 */
IMPORT void knl_signal_all_mutex( TCB *tcb );
IMPORT INT knl_chg_pri_mutex( TCB *tcb, INT priority );

/*
 * カーネル内部メモリ割り当て（Imalloc）(memory.c)
 */
IMPORT ER knl_init_Imalloc( void );
IMPORT void* knl_Imalloc( SZ size );
IMPORT void* knl_Icalloc( SZ nmemb, SZ size );
IMPORT void* knl_Irealloc( void *ptr, SZ size );
IMPORT void  knl_Ifree( void *ptr );

/*
 * 初期タスクの生成パラメータ (inittask.c)
 */
IMPORT const T_CTSK knl_init_ctsk;

/*
 * ユーザメインプログラム (usermain.c)
 */
IMPORT INT usermain( void );

/*
 * 省電力機能 (power.c)
 */
IMPORT UINT	knl_lowpow_discnt;


/* ----------------------------------------------------------------------- */
/*
 * ターゲットシステム依存ルーチン (/sysdepend)
 */

/* 低レベルメモリ管理情報 (reset_hdl.c) */
IMPORT	void	*knl_lowmem_top, *knl_lowmem_limit;

/*
 * ハードウェアの起動・再起動・終了 (hw_setting.c)
 */
IMPORT void knl_startup_hw(void);
IMPORT void knl_shutdown_hw( void );
IMPORT ER knl_restart_hw( W mode );

/*
 * CPU 制御 (cpu_cntl.c)
 */
#if TK_SUPPORT_REGOPS
IMPORT void knl_set_reg( TCB *tcb, CONST T_REGS *regs, CONST T_EIT *eit, CONST T_CREGS *cregs );
IMPORT void knl_get_reg( TCB *tcb, T_REGS *regs, T_EIT *eit, T_CREGS *cregs );
#endif /* TK_SUPPORT_REGOPS */

#if NUM_COPROCESSOR > 0
IMPORT ER knl_get_cpr( TCB *tcb, INT copno, T_COPREGS *copregs);
IMPORT ER knl_set_cpr( TCB *tcb, INT copno, CONST T_COPREGS *copregs);
#endif

/*
 *	タスクディスパッチャ (cpu_cntl.c)
 */
IMPORT void knl_force_dispatch( void );
IMPORT void knl_dispatch( void );

/*
 * 割込み制御 (interrupt.c)
 */
IMPORT ER knl_init_interrupt( void );
IMPORT ER knl_define_inthdr( INT intno, ATR intatr, FP inthdr );
IMPORT void knl_return_inthdr(void);

/*
 * デバイスドライバの起動・終了 (devinit.c)
 */
IMPORT ER knl_init_device( void );
IMPORT ER knl_start_device( void );
IMPORT ER knl_finish_device( void );

/*
 * micro T-Kernel の起動・終了 (sysinit.c)
 */
#ifndef ADD_PREFIX_KNL_TO_GLOBAL_NAME
IMPORT INT main(void);
#else
IMPORT INT knl_main(void);
#endif	/* ADD_PREFIX_KNL_TO_GLOBAL_NAME */

IMPORT void knl_tkernel_exit( void );

/*
 * システムコールエントリ
 */
IMPORT void knl_call_entry( void );

/*
 *	省電力機能 (power_save.c)
 */
IMPORT void low_pow( void );		/* 省電力モードへの移行 */
IMPORT void off_pow( void );		/* サスペンドモードへの移行 */

#endif /* _KERNEL_ */
