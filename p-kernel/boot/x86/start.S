/* QEMU x86_64スタートアップアセンブリコード */
.code64
.section .text

.global _start
_start:
    /* スタックポインタの初期化 */
    mov $stack_top, %rsp

    /* プロセッサの基本設定 */
    cli                     /* 割り込み無効化 */
    cld                     /* 方向フラグクリア */

    /* カーネルメイン関数へジャンプ */
    call main

    /* 万一mainから戻ってきた場合 */
    hlt

.section .bss
.align 16
stack_bottom:
    .skip 4096              /* 4KBのスタック領域 */
stack_top:
