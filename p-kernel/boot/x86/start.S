.code16
.global _start

_start:
    cli                 # 割り込み無効化
    
    # スタック設定
    mov $0x7c00, %sp
    
    # シリアルポート初期化（直接アセンブリで）
    mov $0x3f8, %dx     # COM1ポート
    mov $0x00, %al
    inc %dx
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
    
    # "Hello from Boot!\r\n" を出力
    mov $hello_msg, %si
print_loop:
    lodsb
    test %al, %al
    jz done_print
    mov %al, %bl        # 文字をblに保存
    call serial_putc
    jmp print_loop
    
done_print:
    # Cコードを呼び出し前にもう一度メッセージ
    mov $calling_c_msg, %si
calling_c_loop:
    lodsb
    test %al, %al
    jz call_c_now
    mov %al, %bl
    call serial_putc
    jmp calling_c_loop
    
call_c_now:
    # 16ビットCコードを呼び出し
    call main
    
hang:
    jmp hang

serial_putc:
    mov $0x3f8, %dx
    add $5, %dx
wait_tx:
    in %dx, %al
    test $0x20, %al
    jz wait_tx
    mov $0x3f8, %dx
    mov %bl, %al        # 文字をalに
    out %al, %dx
    ret

hello_msg:
    .asciz "Hello from Boot!\r\n"

calling_c_msg:
    .asciz "Calling C main...\r\n"

.org 510
.word 0xaa55
