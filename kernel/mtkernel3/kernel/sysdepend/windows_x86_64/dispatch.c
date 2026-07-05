/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Windows x86-64 ネイティブポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dispatch.c
 *	タスクディスパッチャ / コンテキスト切替え（Windows Fiber ベース）
 *
 *	Linux x86-64 ポートの dispatch.S（System V ABI の生アセンブラで
 *	callee-saved レジスタとスタックを手退避）を、Windows Fiber API に
 *	置き換えたものです。Fiber がスタックと全レジスタ（XMM6-15 を含む
 *	Win64 の非揮発レジスタ一式）を正しく退避/復元するため、ABI 依存の
 *	アセンブラをこのポートでは一切書きません。
 *
 *	対応関係:
 *	  - 各タスク   ⇔ 1 本の Windows Fiber（CreateFiber）
 *	  - Fiber ハンドル ⇔ tcb->tskctxb.ssp（Linux 版の SSP スロットを流用）
 *	  - コンテキスト切替え ⇔ SwitchToFiber
 *	  - トランポリン（新規タスク初回起動） ⇔ win_task_fiber_entry
 *	  - タスク終了時の再生成 ⇔ 旧 Fiber を「墓場」へ退避し遅延削除
 *
 *	v1 は協調スケジューラ（プリエンプト無し）。タイマ tick は SIGALRM
 *	ではなく、アイドル/ディスパッチという安全点で QueryPerformanceCounter
 *	に追従してポンプします（win_pump_ticks）。
 */

#include <sys/machine.h>
#ifdef WINDOWS_X86_64

#include "kernel.h"

/* arch/windows/x86_64/win_prim.c — Win32 プリミティブの薄いラッパ群。
 * windows.h と kernel.h の型宇宙を分離するため、windows.h はそちら側に
 * 隔離し、ここでは素の C シグネチャだけを参照する。 */
IMPORT void *win_fiber_convert(void);
IMPORT void *win_fiber_create(unsigned long stksz, void (*entry)(void *), void *arg);
IMPORT void  win_fiber_switch(void *fiber);
IMPORT void  win_fiber_delete(void *fiber);
IMPORT void *win_fiber_current(void);
IMPORT void  win_sleep_ms(unsigned int ms);
IMPORT unsigned long long win_qpc_ns(void);

/* 割込み禁止ソフトウェアフラグ（cpu_status.h のマクロ・syslib が参照）。
 * Linux 版は preempt.c が持つが、Windows 版はここで実体を持つ。 */
EXPORT volatile int arch_irq_disabled_flag = 0;

/* preempt.c 相当: 保留 tick の再生（arch_irq_enable_with_drain）。
 * v1 では tick はポンプ方式で安全点でしか進めないため、禁止区間で
 * 取りこぼす tick は無い。契約シンボルとして空に近い実体を置く。 */
EXPORT void arch_irq_enable_with_drain(void)
{
	arch_irq_disabled_flag = 0;
}

/* -----------------------------------------------------------------------
 *  タスクごとの起動引数（stacd）退避テーブル
 *	Linux 版は休止フレームの r12 スロットへ stacd を書いたが、Fiber
 *	ポートではスタックフレームが無いため側テーブルへ退避する。
 *	exinf は tcb->exinf から直接読めるので退避不要。
 * --------------------------------------------------------------------- */
LOCAL INT win_task_stacd[CNF_MAX_TSKID + 1];

/* -----------------------------------------------------------------------
 *  Fiber 墓場（遅延削除リスト）
 *	Fiber は自身の resume 位置を書き換えられないため、タスク終了・
 *	再生成のたびに旧 Fiber は破棄して作り直す。ただし DeleteFiber は
 *	「実行中の Fiber」に対して呼べないため、いったん墓場へ積み、別の
 *	Fiber 上で走るディスパッチの機会に安全に削除する。
 * --------------------------------------------------------------------- */
LOCAL void *win_grave[CNF_MAX_TSKID + 2];
LOCAL int   win_grave_n = 0;

LOCAL void win_grave_sweep(void *keep_next)
{
	void *cur = win_fiber_current();
	int i, j = 0;
	for ( i = 0; i < win_grave_n; i++ ) {
		void *g = win_grave[i];
		if ( g != NULL && g != cur && g != keep_next ) {
			win_fiber_delete(g);		/* 安全に削除 */
		} else {
			win_grave[j++] = g;		/* まだ削除できない — 残す */
		}
	}
	win_grave_n = j;
}

LOCAL void win_grave_push(void *fiber)
{
	if ( fiber == NULL ) return;
	if ( win_grave_n >= (int)(sizeof(win_grave)/sizeof(win_grave[0])) ) {
		win_grave_sweep(NULL);		/* 満杯 — まず掃除を試みる */
	}
	if ( win_grave_n < (int)(sizeof(win_grave)/sizeof(win_grave[0])) ) {
		win_grave[win_grave_n++] = fiber;
	}
	/* それでも入らない場合は取りこぼす（次回 sweep で回収されないが、
	 * 現実的な同時退避数は CNF_MAX_TSKID を超えないため到達しない）。 */
}

/* -----------------------------------------------------------------------
 *  タイマ tick ポンプ（協調スケジューラ用）
 *	QueryPerformanceCounter に追従して、経過した TIMER_PERIOD ごとに
 *	knl_timer_handler を呼ぶ。安全点（アイドル/ディスパッチ入口）から
 *	のみ呼ばれ、win_in_timer で再入を防ぐ。
 * --------------------------------------------------------------------- */
LOCAL unsigned long long win_tick_next_ns = 0;
LOCAL int                win_tick_started = 0;
LOCAL int                win_in_timer     = 0;

/* SIGALRM ハンドラ相当。knl_taskindp を増減して in_indp() を真にし、
 * knl_timer_handler 内の END_CRITICAL_SECTION がディスパッチを起こさない
 * ようにする（Linux 版 dispatch.S の knl_timer_handler_startup と同義）。 */
EXPORT void knl_timer_handler_startup(void)
{
	knl_taskindp++;
	knl_timer_handler();
	knl_taskindp--;
}

LOCAL void win_pump_ticks(void)
{
	unsigned long long now;
	int guard;

	if ( !win_tick_started || win_in_timer ) return;
	win_in_timer = 1;

	now = win_qpc_ns();

	/* 長時間ストール後の暴走キャッチアップを防ぐ: 1 秒以上遅れていたら
	 * 取りこぼしを捨てて基準を再同期する（tick 数を wall-clock に無理に
	 * 合わせない）。 */
	if ( now > win_tick_next_ns + 1000000000ull ) {
		win_tick_next_ns = now;
	}

	guard = 0;
	while ( win_tick_next_ns <= now && guard < 128 ) {
		knl_timer_handler_startup();
		win_tick_next_ns += (unsigned long long)TIMER_PERIOD * 1000000ull;
		guard++;
	}

	win_in_timer = 0;
}

/* sys_timer.h の knl_start_hw_timer から呼ばれる。tick の基準時刻を設定。 */
EXPORT void knl_win_timer_start(void)
{
	win_tick_next_ns = win_qpc_ns() + (unsigned long long)TIMER_PERIOD * 1000000ull;
	win_tick_started = 1;
}

/* hw_setting.c の knl_startup_hw から呼ばれる。主スレッドを Fiber 化する
 * （最初の SwitchToFiber の前に必須）。 */
LOCAL int win_fiber_ready = 0;
EXPORT void knl_win_fiber_boot(void)
{
	if ( !win_fiber_ready ) {
		win_fiber_convert();
		win_fiber_ready = 1;
	}
}

/* -----------------------------------------------------------------------
 *  アイドル待ち
 *	実行可能タスクが無いとき、短く Sleep して経過分の tick をポンプし、
 *	schedtsk を再確認できるよう戻る。ビジーループしない。
 * --------------------------------------------------------------------- */
EXPORT void knl_idle_wait(void)
{
	win_sleep_ms(1);
	win_pump_ticks();
}

/* -----------------------------------------------------------------------
 *  新規タスクの初回起動点（Fiber エントリ）
 *	CreateFiber に渡す C 関数。Linux 版 dispatch.S の
 *	knl_task_entry_trampoline に対応する。
 * --------------------------------------------------------------------- */
LOCAL void win_task_fiber_entry(void *arg)
{
	TCB *tcb = (TCB *)arg;
	INT  stacd;
	void (*entry)(INT, void *);

	/* Linux 版トランポリンの IRQ_FLAG_CLEAR 相当: タスクは割込み許可で
	 * 開始する。 */
	arch_irq_disabled_flag = 0;

	stacd = ( tcb->tskid >= 1 && tcb->tskid <= CNF_MAX_TSKID )
		? win_task_stacd[tcb->tskid] : 0;
	entry = (void (*)(INT, void *))tcb->task;

	entry(stacd, tcb->exinf);

	/* タスク関数が return したら tk_ext_tsk へ（Linux 版はトランポリンが
	 * 戻りアドレスとして push している）。tk_ext_tsk は復帰しない。 */
	tk_ext_tsk();

	/* 到達しない。万一戻ったら安全側でアイドル。 */
	for ( ;; ) knl_idle_wait();
}

/* -----------------------------------------------------------------------
 *  次タスクへの切替え（共通）
 *	Fiber 未生成なら遅延生成し、墓場を掃除してから SwitchToFiber。
 *	呼び出し元 Fiber は、自身が再ディスパッチされたとき本関数から復帰
 *	する。
 * --------------------------------------------------------------------- */
LOCAL void win_switch_to(TCB *next)
{
	if ( next->tskctxb.ssp == NULL ) {
		unsigned long sz = ( next->sstksz > 0 )
				? (unsigned long)next->sstksz : 0;
		next->tskctxb.ssp = win_fiber_create(sz, win_task_fiber_entry, next);
	}
	win_grave_sweep(next->tskctxb.ssp);
	win_fiber_switch(next->tskctxb.ssp);
}

/* schedtsk が選ばれるまでアイドルで待つ共通ループ。 */
LOCAL TCB *win_wait_for_schedtsk(void)
{
	TCB *next;
	for ( ;; ) {
		next = knl_schedtsk;
		if ( next != NULL ) return next;
		/* アイドル: 割込み許可にして tick を進め、再確認する。 */
		knl_dispatch_disabled = 0;
		arch_irq_disabled_flag = 0;
		knl_idle_wait();
		arch_irq_disabled_flag = 1;
		knl_dispatch_disabled = 1;
	}
}

/* -----------------------------------------------------------------------
 *  knl_dispatch_entry
 *	通常ディスパッチ。現 Fiber のコンテキストは SwitchToFiber が自動で
 *	退避するため、Linux 版のようなレジスタ手退避は不要。schedtsk を
 *	選んで切り替え、後で再ディスパッチされたら本関数から復帰する。
 * --------------------------------------------------------------------- */
EXPORT void knl_dispatch_entry(void)
{
	TCB *next;

	arch_irq_disabled_flag = 1;
	knl_dispatch_disabled  = 1;

	/* 安全点でのタイマ追従: yield のたびに wall-clock に追いつく。
	 * win_in_timer と in_indp()（knl_taskindp）で再入・多重ディスパッチを
	 * 防いでいる。 */
	win_pump_ticks();

	next = win_wait_for_schedtsk();

	knl_ctxtsk = next;
	knl_dispatch_disabled = 0;
	win_switch_to(next);

	/* ここへは、この Fiber（タスク）が再びディスパッチされたときに戻る。 */
	arch_irq_disabled_flag = 0;
}

/* -----------------------------------------------------------------------
 *  knl_dispatch_to_schedtsk
 *	強制ディスパッチ。現コンテキストを破棄して schedtsk へ切替え。
 *	タスク終了・自己削除時、および起動時の初回ディスパッチ（主 Fiber、
 *	ctxtsk == NULL）で使われる。通常は復帰しない。
 * --------------------------------------------------------------------- */
EXPORT void knl_dispatch_to_schedtsk(void)
{
	TCB *next;

	arch_irq_disabled_flag = 1;
	knl_dispatch_disabled  = 1;
	knl_ctxtsk = NULL;			/* 現コンテキストを破棄 */

	win_pump_ticks();

	next = win_wait_for_schedtsk();

	knl_ctxtsk = next;
	knl_dispatch_disabled = 0;
	win_switch_to(next);

	/* 破棄済みコンテキストへは戻らない。到達した場合は防御的にアイドル。 */
	arch_irq_disabled_flag = 0;
	for ( ;; ) knl_idle_wait();
}

/* -----------------------------------------------------------------------
 *  cpu_task.h（Inline）から呼ばれるコンテキスト管理の実体
 * --------------------------------------------------------------------- */
EXPORT void knl_win_setup_context(TCB *tcb)
{
	/* 再起動時（既存 Fiber あり）は墓場へ退避し、次回ディスパッチで
	 * 新 Fiber を生成させる（ssp = NULL）。 */
	if ( tcb->tskctxb.ssp != NULL ) {
		win_grave_push(tcb->tskctxb.ssp);
	}
	tcb->tskctxb.ssp = NULL;
}

EXPORT void knl_win_setup_stacd(TCB *tcb, INT stacd)
{
	if ( tcb->tskid >= 1 && tcb->tskid <= CNF_MAX_TSKID ) {
		win_task_stacd[tcb->tskid] = stacd;
	}
}

EXPORT void knl_win_cleanup_context(TCB *tcb)
{
	if ( tcb->tskctxb.ssp != NULL ) {
		win_grave_push(tcb->tskctxb.ssp);
		tcb->tskctxb.ssp = NULL;
	}
}

/* sysdepend.h が IMPORT するトランポリンシンボルの契約実体。Fiber ポート
 * では win_task_fiber_entry が役割を果たすため、これ自体は呼ばれない。 */
EXPORT void knl_task_entry_trampoline(void)
{
}

#endif /* WINDOWS_X86_64 */
