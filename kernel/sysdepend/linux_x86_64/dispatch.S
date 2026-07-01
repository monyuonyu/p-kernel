/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux x86-64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dispatch.S
 *	タスクディスパッチャ / コンテキスト切替え
 *	（Linux x86-64 ユーザモード, System V AMD64 ABI）
 *
 *	micro T-Kernel 2.0 ポート（arch/linux/x86_64/cpu_support.S）と
 *	同じ方式を μT-Kernel 3.0 の TCB レイアウトに合わせたものです。
 *
 *	  - コンテキストはタスク自身のスタック上の 64 バイトフレームに
 *	    退避し、TCB には SSP（tskctxb.ssp）だけを保存する
 *	  - 割込み禁止は arch_irq_disabled_flag への書き込み
 *	    （SIGALRM がタイマ割込みの代替。preempt.c 参照）
 *	  - 実行可能タスクが無いときは knl_idle_wait（sigsuspend）で
 *	    次の tick まで眠る（ビジーループしない）
 *
 *	休止フレーム（64 バイト, cpu_task.h と一致させること）:
 *	  [rsp+ 0] r12  （新規タスクでは stacd）
 *	  [rsp+ 8] r13  （新規タスクでは exinf）
 *	  [rsp+16] r14
 *	  [rsp+24] r15
 *	  [rsp+32] rbx
 *	  [rsp+40] rbp
 *	  [rsp+48] （予約）
 *	  [rsp+56] rip  （最後の ret が pop。新規タスクではトランポリン）
 */

#define _in_asm_source_
#include "offset.h"

    .text
    .balign 16

/* -----------------------------------------------------------------------
 *  割込みフラグ操作マクロ
 *	arch_irq_disabled_flag（preempt.c の 32 ビット int）へ 0/1 を
 *	書き込む。IRQ_FLAG_CLEAR は保留 tick の再生を行わない素のクリア。
 *	ディスパッチ出口・トランポリン入口で drain を行うと、シグナル
 *	ハンドラから knl_dispatch へ再入する危険があるため。
 * --------------------------------------------------------------------- */

    .macro IRQ_DISABLE
    movl    $1, arch_irq_disabled_flag(%rip)
    .endm

    .macro IRQ_FLAG_CLEAR
    movl    $0, arch_irq_disabled_flag(%rip)
    .endm

/* -----------------------------------------------------------------------
 *  knl_dispatch_to_schedtsk
 *	現在のコンテキストを破棄して schedtsk へ切り替える（強制
 *	ディスパッチ）。タスクの終了・自己削除時に使われ、レジスタは
 *	保存しない。cpu_status.h の knl_force_dispatch() から呼ばれる。
 * --------------------------------------------------------------------- */
    .global knl_dispatch_to_schedtsk
    .type   knl_dispatch_to_schedtsk, @function
knl_dispatch_to_schedtsk:
    IRQ_DISABLE
    movl    $1, knl_dispatch_disabled(%rip)   /* ディスパッチ中 */
    movq    $0, knl_ctxtsk(%rip)              /* ctxtsk = NULL */
    jmp     .Ldispatch_loop

/* -----------------------------------------------------------------------
 *  knl_dispatch_entry
 *	現タスクのコンテキストを自身のスタックへ退避し、SSP を TCB に
 *	書いてから schedtsk を選んで切り替える（通常ディスパッチ）。
 *
 *	関数として呼ばれるため、進入時 [rsp] には呼び出し元の戻り
 *	アドレスが載っている。その下に 56 バイトを確保すると合計
 *	64 バイトの休止フレームが完成する。
 * --------------------------------------------------------------------- */
    .global knl_dispatch_entry
    .type   knl_dispatch_entry, @function
knl_dispatch_entry:
    IRQ_DISABLE
    movl    $1, knl_dispatch_disabled(%rip)

    /* 呼び出し元の rip の下に 56 バイト確保してフレームを構築 */
    subq    $56, %rsp
    movq    %r12,  0(%rsp)
    movq    %r13,  8(%rsp)
    movq    %r14, 16(%rsp)
    movq    %r15, 24(%rsp)
    movq    %rbx, 32(%rsp)
    movq    %rbp, 40(%rsp)
    movq    $0,   48(%rsp)                    /* 予約スロット */
    /* 呼び出し元の rip は [rsp+56] にそのまま残す */

    /* ctxtsk->tskctxb.ssp へ現在の rsp を保存。ctxtsk が NULL の
     * 場合（タスク外コンテキストからの初回起動）は保存しない。 */
    movq    knl_ctxtsk(%rip), %rax
    testq   %rax, %rax
    jz      .Lno_save
    movq    %rsp, TCB_SSP(%rax)
.Lno_save:
    movq    $0, knl_ctxtsk(%rip)

/* -----------------------------------------------------------------------
 *  ディスパッチャループ
 *	schedtsk を選び、その休止フレームへ rsp を切り替えて復帰する。
 *	schedtsk が NULL の間は knl_idle_wait で SIGALRM を待つ。
 * --------------------------------------------------------------------- */
.Ldispatch_loop:
    movq    knl_schedtsk(%rip), %rax
    testq   %rax, %rax
    jz      .Lidle

    /* ctxtsk = schedtsk */
    movq    %rax, knl_ctxtsk(%rip)

    /* rsp を schedtsk の休止フレームへ切替え */
    movq    TCB_SSP(%rax), %rsp

    /* ディスパッチ完了。この時点ではまだ割込み禁止のまま —
     * フラグ解除は復帰直前に行う。 */
    movl    $0, knl_dispatch_disabled(%rip)

    /* フレームの復元 */
    movq     0(%rsp), %r12
    movq     8(%rsp), %r13
    movq    16(%rsp), %r14
    movq    24(%rsp), %r15
    movq    32(%rsp), %rbx
    movq    40(%rsp), %rbp
    addq    $56, %rsp            /* rsp は保存済み rip のスロットを指す */

    /* 割込み再許可（保留分の drain はしない）。ここから ret までの
     * 間に C 関数を呼んではならない — 戻りアドレスがスタック上に
     * あり、push が入るとずれる。 */
    IRQ_FLAG_CLEAR
    ret                          /* rip を pop して復帰 */

.Lidle:
    /* 実行可能タスクが無い。次の SIGALRM tick まで眠る。
     * 待つ前にフラグを解除して tick ハンドラが直接走れるようにし
     * （時刻が進み schedtsk が設定され得る）、起きたら禁止状態に
     * 戻してから schedtsk を再確認する。knl_idle_wait は SIGALRM を
     * ブロックした状態で schedtsk を再チェックするため、この隙間で
     * 実行可能になったタスクを取りこぼすことはない。 */
    IRQ_FLAG_CLEAR
    call    knl_idle_wait
    IRQ_DISABLE
    jmp     .Ldispatch_loop

    .size knl_dispatch_entry, . - knl_dispatch_entry

/* -----------------------------------------------------------------------
 *  knl_task_entry_trampoline
 *	新規タスクの初回起動点。knl_setup_context が休止フレームの
 *	rip スロットへ本アドレスを書くため、ディスパッチャの ret が
 *	ここへ着地する。
 *
 *	  r12 = stacd（タスクエントリの第1引数）
 *	  r13 = exinf（タスクエントリの第2引数）
 *
 *	進入時 rsp は 16 バイト整列。tk_ext_tsk を戻り先として push
 *	すると rsp ≡ 8 (mod 16) となり、これは call 直後と同じ、
 *	ABI が関数エントリに要求する整列位置になる。
 * --------------------------------------------------------------------- */
    .global knl_task_entry_trampoline
    .type   knl_task_entry_trampoline, @function
    .balign 16
knl_task_entry_trampoline:
    /* ディスパッチャは割込み禁止のまま出てくる。タスクは割込み
     * 許可状態で開始する。 */
    IRQ_FLAG_CLEAR

    /* タスクエントリの取得: tcb = knl_ctxtsk; entry = tcb->task */
    movq    knl_ctxtsk(%rip), %rax
    movq    TCB_task(%rax), %rcx

    movq    %r12, %rdi                 /* stacd → 第1引数 */
    movq    %r13, %rsi                 /* exinf → 第2引数 */

    /* タスクが return した場合に tk_ext_tsk へ落ちるよう、
     * 戻りアドレスとして push しておく。 */
    leaq    tk_ext_tsk(%rip), %rdx
    pushq   %rdx

    jmp     *%rcx                      /* タスクへジャンプ */

    .size knl_task_entry_trampoline, . - knl_task_entry_trampoline

/* -----------------------------------------------------------------------
 *  knl_timer_handler_startup
 *	SIGALRM ハンドラ（preempt.c）と μT-Kernel の knl_timer_handler の
 *	橋渡し。前後で knl_taskindp を増減させ、ハンドラ内で
 *	in_indp() 系の判定が真になるようにする。
 * --------------------------------------------------------------------- */
    .global knl_timer_handler_startup
    .type   knl_timer_handler_startup, @function
    .balign 16
knl_timer_handler_startup:
    pushq   %rbp
    movq    %rsp, %rbp

    incl    knl_taskindp(%rip)
    call    knl_timer_handler
    decl    knl_taskindp(%rip)

    popq    %rbp
    ret

    .size knl_timer_handler_startup, . - knl_timer_handler_startup

/* Linux ツールチェーン向け実行不可スタック注記 */
    .section .note.GNU-stack, "", @progbits
