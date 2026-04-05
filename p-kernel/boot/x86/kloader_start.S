/*
 * kloader_start.S — Stage-1 Kernel Loader エントリポイント
 *
 * Multiboot a.out kludge (flags bit 16) でアドレス 0x50000 にロード。
 * 32ビット保護モードのみ（ロングモード不要）。
 * ディスク読み込み後 0x100000 へジャンプ。
 */

    .set MULTIBOOT_MAGIC,    0x1BADB002
    /* bit 0: align, bit 1: meminfo, bit 16: a.out kludge */
    .set MULTIBOOT_FLAGS,    0x00010003
    .set MULTIBOOT_CHECKSUM, -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

    .set KLOADER_LOAD_ADDR,  0x50000
    .set KLOADER_STACK_TOP,  0x58000   /* kloader 上部 — 安全なスタック */

    .section .multiboot
    .align 4
multiboot_header:
    .long MULTIBOOT_MAGIC
    .long MULTIBOOT_FLAGS
    .long MULTIBOOT_CHECKSUM
    /* a.out kludge フィールド (flags bit 16) */
    .long multiboot_header          /* header_addr  — ヘッダの物理アドレス */
    .long KLOADER_LOAD_ADDR         /* load_addr    — セクション開始アドレス */
    .long 0                         /* load_end_addr — 0=ELFサイズから算出 */
    .long 0                         /* bss_end_addr  — 0=bssなし扱い */
    .long _start                    /* entry_addr   — エントリポイント */

    .section .text
    .code32
    .globl _start
_start:
    /* 割り込み無効化 */
    cli

    /* GDT ロード */
    lgdt (kl_gdtr)

    /* DS/ES/SS をデータセグメントに設定 */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    /* CS をコードセグメントにリロード */
    ljmp $0x08, $1f
1:
    /* スタック設定 */
    movl $KLOADER_STACK_TOP, %esp

    /* kl_main() 呼び出し */
    call kl_main

    /* kl_main は返らないはずだが、返ってきたら停止 */
halt:
    hlt
    jmp halt

/* ------------------------------------------------------------------ */
/* 32ビット GDT                                                        */
/* ------------------------------------------------------------------ */
    .align 8
kl_gdt:
    /* 0x00: null descriptor */
    .quad 0x0000000000000000
    /* 0x08: 32-bit code segment  base=0 limit=4GB G=1 D=1 P=1 DPL=0 */
    .word 0xFFFF, 0x0000
    .byte 0x00, 0x9A, 0xCF, 0x00
    /* 0x10: 32-bit data segment  base=0 limit=4GB G=1 B=1 P=1 DPL=0 */
    .word 0xFFFF, 0x0000
    .byte 0x00, 0x92, 0xCF, 0x00
kl_gdt_end:

kl_gdtr:
    .word kl_gdt_end - kl_gdt - 1
    .long kl_gdt
