.code32

# Multiboot ヘッダー
.set MULTIBOOT_MAGIC, 0x1BADB002
.set MULTIBOOT_FLAGS, 0x00000000
.set MULTIBOOT_CHECKSUM, -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

.section .multiboot
.align 4
.long MULTIBOOT_MAGIC
.long MULTIBOOT_FLAGS
.long MULTIBOOT_CHECKSUM

.section .text
.global _start

_start:
    cli                 # 割り込み無効化
    
    # スタック設定
    mov $0x200000, %esp
    
    # 32ビットでシリアルポート初期化
    call serial_init_32
    
    # 32ビット開始メッセージ出力
    mov $boot_32_msg, %esi
    call print_string_32
    
    # 64ビットロングモードへの移行を試行
    call check_long_mode_support
    test %eax, %eax
    jz no_long_mode
    
    # 64ビット移行成功メッセージ（実際の移行は後で実装）
    mov $entering_64bit_msg, %esi
    call print_string_32
    mov $cpu_64bit_support_msg, %esi
    call print_string_32
    # TODO: 実際の64ビット移行処理
    # call enter_long_mode
    
no_long_mode:
    # 32ビットモードのまま実行
    mov $no_64bit_msg, %esi
    call print_string_32
    call main
    
hang_32:
    jmp hang_32

# 64ビットロングモード対応チェック
check_long_mode_support:
    # CPUID機能の存在確認
    pushfl
    pop %eax
    mov %eax, %ecx
    xor $0x200000, %eax  # ID ビットを反転
    push %eax
    popfl
    pushfl
    pop %eax
    xor %ecx, %eax
    jz no_cpuid
    
    # 拡張CPUID機能確認
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb no_long_mode_support
    
    # ロングモード対応確認
    mov $0x80000001, %eax
    cpuid
    test $0x20000000, %edx  # LM ビット
    jz no_long_mode_support
    
    mov $1, %eax
    ret

no_cpuid:
no_long_mode_support:
    mov $0, %eax
    ret

# TODO: 64ビットロングモード移行（将来実装）
# 現在は64ビット対応チェックのみ実装

.code32

# 32ビットシリアルポート初期化
serial_init_32:
    push %eax
    push %edx
    
    mov $0x3f8, %dx     # COM1ポート
    inc %dx
    mov $0x00, %al
    out %al, %dx        # 割り込み無効
    
    mov $0x3f8, %dx
    add $3, %dx
    mov $0x80, %al
    out %al, %dx        # DLAB有効
    
    mov $0x3f8, %dx
    mov $0x03, %al
    out %al, %dx        # 分周器下位
    
    inc %dx
    mov $0x00, %al
    out %al, %dx        # 分周器上位
    
    add $2, %dx
    mov $0x03, %al
    out %al, %dx        # 8ビット、パリティなし
    
    inc %dx
    mov $0xc7, %al
    out %al, %dx        # FIFO有効
    
    pop %edx
    pop %eax
    ret

# 32ビットモードでの文字列出力関数
print_string_32:
    push %eax
    push %edx
print_loop_32:
    lodsb
    test %al, %al
    jz print_done_32
    call serial_putc_32
    jmp print_loop_32
print_done_32:
    pop %edx
    pop %eax
    ret

# 32ビットモードでのシリアル出力
serial_putc_32:
    push %edx
    mov $0x3f8, %dx
    add $5, %dx
wait_tx_32:
    in %dx, %al
    test $0x20, %al
    jz wait_tx_32
    mov $0x3f8, %dx
    out %al, %dx
    pop %edx
    ret

boot_32_msg:
    .asciz "32-bit kernel started!\r\n"

no_64bit_msg:
    .asciz "64-bit mode not supported, running in 32-bit mode\r\n"

entering_64bit_msg:
    .asciz "Entering 64-bit long mode...\r\n"

success_64bit_msg:
    .asciz "64-bit long mode activated!\r\n"

cpu_64bit_support_msg:
    .asciz "CPU supports 64-bit long mode!\r\n"

# TODO: 64ビット用GDTとページテーブル（将来実装）
