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
    
    # 32ビットCコードを呼び出し
    call main
    
hang_32:
    jmp hang_32

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
