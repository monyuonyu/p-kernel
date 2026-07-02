/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux AArch64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dispatch.S
 *	タスクディスパッチャ / コンテキスト切替え
 *	（Linux AArch64 ユーザモード, AAPCS64）
 *
 *	micro T-Kernel 2.0 ポート（arch/linux/aarch64/cpu_support.S）と
 *	同じ方式を μT-Kernel 3.0 の TCB レイアウトに合わせたものです。
 *	linux_x86_64 の dispatch.S と対になる実装で、契約は同一:
 *
 *	  - コンテキストはタスク自身のスタック上の 112 バイトフレームに
 *	    退避し、TCB には SSP（tskctxb.ssp）だけを保存する
 *	  - 割込み禁止は arch_irq_disabled_flag への書き込み
 *	    （SIGALRM がタイマ割込みの代替。preempt.c 参照）
 *	  - 実行可能タスクが無いときは knl_idle_wait（sigsuspend）で
 *	    次の tick まで眠る（ビジーループしない）
 *
 *	休止フレーム（112 バイト, cpu_task.h と一致させること）:
 *	  [sp+  0] x19（新規タスクでは stacd）  [sp+ 56] x26
 *	  [sp+  8] x20（新規タスクでは exinf）  [sp+ 64] x27
 *	  [sp+ 16] x21                          [sp+ 72] x28
 *	  [sp+ 24] x22                          [sp+ 80] x29 (fp)
 *	  [sp+ 32] x23                          [sp+ 88] x30 (lr) ← 復帰先
 *	  [sp+ 40] x24                          [sp+ 96] （予約）
 *	  [sp+ 48] x25                          [sp+104] （パディング）
 */

#define _in_asm_source_
#include "offset.h"

    .text
    .align 4

/* -----------------------------------------------------------------------
 *  割込みフラグ操作マクロ
 *	arch_irq_disabled_flag（preempt.c の 32 ビット int）へ 0/1 を
 *	書き込む。x16/x17 は AAPCS64 の手続き内スクラッチレジスタで、
 *	T-Kernel の C コードから見える状態を乱さない。
 *	IRQ_FLAG_CLEAR は保留 tick の再生を行わない素のクリア
 *	（ディスパッチ出口で drain するとシグナルハンドラから
 *	knl_dispatch へ再入する危険があるため）。
 * --------------------------------------------------------------------- */

    .macro IRQ_DISABLE
    adrp x16, arch_irq_disabled_flag
    add  x16, x16, :lo12:arch_irq_disabled_flag
    mov  w17, #1
    str  w17, [x16]
    .endm

    .macro IRQ_FLAG_CLEAR
    adrp x16, arch_irq_disabled_flag
    add  x16, x16, :lo12:arch_irq_disabled_flag
    str  wzr, [x16]
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
    adrp x0, knl_dispatch_disabled
    add  x0, x0, :lo12:knl_dispatch_disabled
    mov  w1, #1
    str  w1, [x0]               /* ディスパッチ中（INT = 32bit 書き込み） */
    adrp x0, knl_ctxtsk
    add  x0, x0, :lo12:knl_ctxtsk
    str  xzr, [x0]              /* ctxtsk = NULL */
    b    .Ldispatch_loop

/* -----------------------------------------------------------------------
 *  knl_dispatch_entry
 *	現タスクのコンテキストを自身のスタックへ退避し、SSP を TCB に
 *	書いてから schedtsk を選んで切り替える（通常ディスパッチ）。
 * --------------------------------------------------------------------- */
    .global knl_dispatch_entry
    .type   knl_dispatch_entry, @function
knl_dispatch_entry:
    IRQ_DISABLE
    adrp x0, knl_dispatch_disabled
    add  x0, x0, :lo12:knl_dispatch_disabled
    mov  w1, #1
    str  w1, [x0]

    /* 112 バイトの休止フレームを現在の sp に積む */
    sub sp, sp, #112
    stp x19, x20, [sp,  #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    str xzr,      [sp, #96]     /* 予約スロット */

    /* ctxtsk->tskctxb.ssp へ現在の sp を保存。ctxtsk が NULL の
     * 場合（タスク外コンテキストからの初回起動）は保存しない。 */
    adrp x0, knl_ctxtsk
    add  x0, x0, :lo12:knl_ctxtsk
    ldr  x0, [x0]
    cbz  x0, .Lno_save
    mov  x1, sp
    str  x1, [x0, #TCB_SSP]
.Lno_save:
    adrp x0, knl_ctxtsk
    add  x0, x0, :lo12:knl_ctxtsk
    str  xzr, [x0]

/* -----------------------------------------------------------------------
 *  ディスパッチャループ
 *	schedtsk を選び、その休止フレームへ sp を切り替えて復帰する。
 *	schedtsk が NULL の間は knl_idle_wait で SIGALRM を待つ。
 * --------------------------------------------------------------------- */
.Ldispatch_loop:
    adrp x0, knl_schedtsk
    add  x0, x0, :lo12:knl_schedtsk
    ldr  x0, [x0]
    cbz  x0, .Lidle

    /* ctxtsk = schedtsk */
    adrp x1, knl_ctxtsk
    add  x1, x1, :lo12:knl_ctxtsk
    str  x0, [x1]

    /* sp を schedtsk の休止フレームへ切替え */
    ldr  x1, [x0, #TCB_SSP]
    mov  sp, x1

    /* ディスパッチ完了。フラグ解除は復帰直前に行う。 */
    adrp x1, knl_dispatch_disabled
    add  x1, x1, :lo12:knl_dispatch_disabled
    str  wzr, [x1]

    /* フレームの復元 */
    ldp x19, x20, [sp,  #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #112

    /* 割込み再許可（保留分の drain はしない）。x30 に復帰先が
     * 入っているため、ここから ret までの間に C 関数を呼んでは
     * ならない。 */
    IRQ_FLAG_CLEAR
    ret

.Lidle:
    /* 実行可能タスクが無い。次の SIGALRM tick まで眠る。
     * 待つ前にフラグを解除して tick ハンドラが直接走れるようにし、
     * 起きたら禁止状態に戻してから schedtsk を再確認する。
     * knl_idle_wait は SIGALRM をブロックした状態で schedtsk を
     * 再チェックするため、この隙間で実行可能になったタスクを
     * 取りこぼすことはない。 */
    IRQ_FLAG_CLEAR
    bl   knl_idle_wait
    IRQ_DISABLE
    b    .Ldispatch_loop

    .size knl_dispatch_entry, . - knl_dispatch_entry

/* -----------------------------------------------------------------------
 *  knl_task_entry_trampoline
 *	新規タスクの初回起動点。knl_setup_context が休止フレームの
 *	x30 スロットへ本アドレスを書くため、ディスパッチャの ret が
 *	ここへ着地する。
 *
 *	  x19 = stacd（タスクエントリの第1引数）
 *	  x20 = exinf（タスクエントリの第2引数）
 *
 *	タスクが return した場合は x30 = tk_ext_tsk により
 *	自動的にタスク終了へ落ちる。
 * --------------------------------------------------------------------- */
    .global knl_task_entry_trampoline
    .type   knl_task_entry_trampoline, @function
    .align 4
knl_task_entry_trampoline:
    /* ディスパッチャは割込み禁止のまま出てくる。タスクは割込み
     * 許可状態で開始する。 */
    IRQ_FLAG_CLEAR

    /* タスクエントリの取得: tcb = knl_ctxtsk; entry = tcb->task */
    adrp x16, knl_ctxtsk
    add  x16, x16, :lo12:knl_ctxtsk
    ldr  x16, [x16]
    ldr  x17, [x16, #TCB_task]

    mov  x0, x19                /* stacd → 第1引数 */
    mov  x1, x20                /* exinf → 第2引数 */

    /* 戻り先を tk_ext_tsk にしてタスクへジャンプ */
    adrp x30, tk_ext_tsk
    add  x30, x30, :lo12:tk_ext_tsk

    br   x17

    .size knl_task_entry_trampoline, . - knl_task_entry_trampoline

/* -----------------------------------------------------------------------
 *  knl_timer_handler_startup
 *	SIGALRM ハンドラ（preempt.c）と μT-Kernel の knl_timer_handler の
 *	橋渡し。前後で knl_taskindp を増減させ、ハンドラ内で
 *	in_indp() 系の判定が真になるようにする。
 * --------------------------------------------------------------------- */
    .global knl_timer_handler_startup
    .type   knl_timer_handler_startup, @function
    .align 4
knl_timer_handler_startup:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    adrp x0, knl_taskindp
    add  x0, x0, :lo12:knl_taskindp
    ldr  w1, [x0]
    add  w1, w1, #1
    str  w1, [x0]

    bl   knl_timer_handler

    adrp x0, knl_taskindp
    add  x0, x0, :lo12:knl_taskindp
    ldr  w1, [x0]
    sub  w1, w1, #1
    str  w1, [x0]

    ldp x29, x30, [sp], #16
    ret

    .size knl_timer_handler_startup, . - knl_timer_handler_startup

/* Linux ツールチェーン向け実行不可スタック注記 */
    .section .note.GNU-stack, "", @progbits
