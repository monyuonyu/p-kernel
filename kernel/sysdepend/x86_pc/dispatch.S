/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dispatch.S
 *	タスクディスパッチャ / コンテキスト切替え
 *	（x86 32bit 互換モード on ロングモード, cdecl）
 *
 *	micro T-Kernel 2.0 ポート（arch/x86/cpu_support.S）と同じ方式を
 *	μT-Kernel 3.0 の TCB レイアウトに合わせたものです。ベアメタル
 *	固有のフックも維持します:
 *	  - eager FPU 退避/復帰（arch/x86/fpu.c の side-table 方式）
 *	  - 毎ディスパッチの TSS.RSP0 更新（ring3 タスクのカーネル
 *	    スタック切替。gdt_set_kernel_stack）
 *	  - ディスパッチ直前の TCB poison チェック（解放済み TCB への
 *	    切替を確定的な halt に変える）
 *
 *	休止フレーム（cpu_task.h と一致させること）:
 *	  [esp+ 0] ebx   [esp+ 8] edi   [esp+16] （予約）
 *	  [esp+ 4] esi   [esp+12] ebp   [esp+20] ret（トランポリン/呼出元）
 */

#define _in_asm_source_
#include "offset.h"

    .code32
    .text
    .align 4

/* -----------------------------------------------------------------------
 *  knl_dispatch_to_schedtsk
 *	現在のコンテキストを破棄して schedtsk へ切り替える（強制
 *	ディスパッチ）。cpu_status.h の knl_force_dispatch() から呼ばれる。
 * --------------------------------------------------------------------- */
    .globl  knl_dispatch_to_schedtsk
knl_dispatch_to_schedtsk:
    cli
    movl    $1, (knl_dispatch_disabled)   /* ディスパッチ中 */
    movl    $0, (knl_ctxtsk)              /* ctxtsk = NULL（保存なし） */
    jmp     .Ldispatch_loop

/* -----------------------------------------------------------------------
 *  knl_dispatch_entry
 *	現タスクのコンテキストを自身のスタックへ退避し、SSP を TCB に
 *	書いてから schedtsk を選んで切り替える（通常ディスパッチ）。
 *	END_CRITICAL_SECTION から通常の C 関数として呼ばれる。
 * --------------------------------------------------------------------- */
    .globl  knl_dispatch_entry
knl_dispatch_entry:
    cli
    movl    $1, (knl_dispatch_disabled)

    /* callee-saved レジスタ + 予約スロットを退避
     * （呼び出し元の戻りアドレスは既に [esp] にある） */
    pushl   $0                      /* 予約スロット */
    pushl   %ebp
    pushl   %edi
    pushl   %esi
    pushl   %ebx

    /* ctxtsk->tskctxb.ssp へ現在の esp を保存。ctxtsk が NULL の
     * 場合（タスク外コンテキスト）は保存しない。 */
    movl    (knl_ctxtsk), %eax
    testl   %eax, %eax
    jz      .Lno_save
    movl    %esp, TCB_SSP(%eax)

    /* eager FPU 退避: 退場タスクの x87/MMX/SSE 状態を side-table へ
     * fxsave（arch/x86/fpu.c）。fpu_init() が武装するまで no-op。 */
    pushl   %eax
    call    knl_fpu_save_ctx
    addl    $4, %esp
.Lno_save:
    movl    $0, (knl_ctxtsk)

/* -----------------------------------------------------------------------
 *  ディスパッチャループ
 * --------------------------------------------------------------------- */
.Ldispatch_loop:
    movl    (knl_schedtsk), %eax
    testl   %eax, %eax
    jz      .Lidle

    /* 解放済み TCB（state == TS_NONEXIST）への切替を検出して
     * 確定的に halt する（kill/heal churn 起因の garbage-PC #PF を
     * grep 可能な停止に変える）。 */
    pushl   %eax
    call    knl_dispatch_poison_check
    addl    $4, %esp
    movl    (knl_schedtsk), %eax    /* call で %eax が壊れるため再読込 */

    /* ctxtsk = schedtsk、esp を切替え */
    movl    %eax, (knl_ctxtsk)
    movl    TCB_SSP(%eax), %esp

    /* eager FPU 復帰（初回はクリーンテンプレート） */
    pushl   %eax
    call    knl_fpu_restore_ctx
    addl    $4, %esp

    /* TSS.RSP0 を入場タスクのカーネルスタックトップへ毎回更新。
     * ring3 タスク終了後の stale RSP0 によるスタック破壊を防ぐ
     * （純 ring0 タスクには無害）。 */
    movl    (knl_ctxtsk), %eax
    pushl   TCB_isstack(%eax)
    call    gdt_set_kernel_stack
    addl    $4, %esp

    /* ディスパッチ完了 */
    movl    $0, (knl_dispatch_disabled)

    /* フレームの復元 */
    popl    %ebx
    popl    %esi
    popl    %edi
    popl    %ebp
    addl    $4, %esp                /* 予約スロットを捨てる */

    sti
    ret                             /* 保存済み戻り先 / トランポリンへ */

.Lidle:
    /* 実行可能タスクが無い — 次の割込みまで halt */
    sti
    hlt
    cli
    jmp     .Ldispatch_loop

/* -----------------------------------------------------------------------
 *  knl_task_entry_trampoline
 *	新規タスクの初回起動点。knl_setup_context が休止フレームの
 *	ret スロットへ本アドレスを書く。この時点でスタックは
 *	  [esp+0] &tk_ext_tsk  [esp+4] stacd  [esp+8] exinf
 *	という cdecl フレームなので、TCB_task へ jmp するだけでよい。
 * --------------------------------------------------------------------- */
    .globl  knl_task_entry_trampoline
knl_task_entry_trampoline:
    sti                             /* 新規タスクは割込み許可で開始 */

    movl    (knl_ctxtsk), %eax
    movl    TCB_task(%eax), %eax
    jmp     *%eax                   /* task(stacd, exinf) へ末尾ジャンプ */

/* -----------------------------------------------------------------------
 *  knl_timer_handler_startup
 *	IRQ0（PIT）ハンドラと μT-Kernel の knl_timer_handler の橋渡し。
 *	前後で knl_taskindp を増減させる。
 * --------------------------------------------------------------------- */
    .globl  knl_timer_handler_startup
knl_timer_handler_startup:
    incl    (knl_taskindp)
    call    knl_timer_handler
    decl    (knl_taskindp)
    ret

/* end of dispatch.S */
