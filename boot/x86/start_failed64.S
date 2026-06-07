# 64ビット移行可能カーネル（Multiboot互換）
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

# 32ビットエントリポイント（Multibootから呼び出される）
_start:
    cli                 # 割り込み無効化
    
    # スタック設定（32ビット）
    mov $0x200000, %esp
    
    # 32ビットシリアルポート初期化
    call serial_init_32
    
    # 32ビット開始メッセージ
    pushl $starting_msg
    call print_string_32
    addl $4, %esp
    
    # 64ビットロングモード対応チェック
    call check_long_mode_support
    test %eax, %eax
    jz no_long_mode_error
    
    # 64ビット移行開始メッセージ
    pushl $entering_64_msg
    call print_string_32
    addl $4, %esp
    
    # 64ビットロングモードに移行
    call enter_long_mode
    
no_long_mode_error:
    pushl $no_64bit_error_msg
    call print_string_32
    addl $4, %esp
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
    jz no_cpuid_support
    
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

no_cpuid_support:
no_long_mode_support:
    mov $0, %eax
    ret

# 64ビットロングモードに移行
enter_long_mode:
    # PAE有効化
    mov %cr4, %eax
    or $0x20, %eax      # PAE ビット
    mov %eax, %cr4
    
    # ページテーブル設定
    call setup_page_tables
    
    # ページテーブルをCR3に設定
    mov $pml4_table, %eax
    mov %eax, %cr3
    
    # IA32_EFER MSRでロングモード有効化
    mov $0xC0000080, %ecx  # IA32_EFER MSR
    rdmsr
    or $0x100, %eax        # LME ビット
    wrmsr
    
    # ページング有効化（ロングモード移行）
    mov %cr0, %eax
    or $0x80000000, %eax   # PG ビット
    mov %eax, %cr0
    
    # 64ビットGDTをロード
    lgdt gdt64_descriptor
    
    # 64ビットモードへジャンプ
    ljmp $0x08, $start_64bit

# ページテーブル設定（アイデンティティマッピング）
setup_page_tables:
    # すべてのページテーブルをクリア
    mov $pml4_table, %edi
    mov $0, %eax
    mov $3072, %ecx  # 3 * 4096 / 4
    rep stosl
    
    # PML4テーブル（最上位）
    mov $pml4_table, %edi
    mov $pdp_table, %eax
    or $0x03, %eax      # Present + Writable
    mov %eax, (%edi)
    
    # PDPテーブル
    mov $pdp_table, %edi
    mov $pd_table, %eax
    or $0x03, %eax
    mov %eax, (%edi)
    
    # PDテーブル（2MBページ使用）
    mov $pd_table, %edi
    mov $0, %eax
pd_loop:
    mov %eax, %ebx
    shl $21, %ebx       # 2MBページ
    or $0x83, %ebx      # Present + Writable + Page Size
    mov %ebx, (%edi)
    add $8, %edi
    inc %eax
    cmp $512, %eax
    jl pd_loop
    
    ret

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

# 32ビット文字列出力
print_string_32:
    push %ebp
    mov %esp, %ebp
    push %esi
    push %eax
    
    mov 8(%ebp), %esi
print_loop_32:
    lodsb
    test %al, %al
    jz print_done_32
    call serial_putc_32
    jmp print_loop_32
print_done_32:
    pop %eax
    pop %esi
    pop %ebp
    ret

# 32ビットシリアル出力
serial_putc_32:
    push %eax
    push %edx
    
    mov %al, %ah      # 文字をAHに保存
    mov $0x3f8, %dx
    add $5, %dx
wait_tx_32:
    in %dx, %al
    test $0x20, %al
    jz wait_tx_32
    mov $0x3f8, %dx
    mov %ah, %al      # 保存した文字をALに復帰
    out %al, %dx
    
    pop %edx
    pop %eax
    ret

hang_32:
    jmp hang_32

# 64ビットコードセクション開始
.code64
start_64bit:
    # セグメントレジスタ設定
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    
    # 64ビットスタック設定
    mov $0x300000, %rsp
    
    # 64ビット開始メッセージ
    mov $success_64_msg, %rdi
    call print_string_64
    
    # 64ビットCコードを呼び出し
    call main
    
hang_64:
    jmp hang_64

# 64ビット文字列出力
print_string_64:
    push %rax
    push %rdx
    push %rsi
    
    mov %rdi, %rsi
print_loop_64:
    lodsb
    test %al, %al
    jz print_done_64
    call serial_putc_64
    jmp print_loop_64
print_done_64:
    pop %rsi
    pop %rdx
    pop %rax
    ret

# 64ビットシリアル出力
serial_putc_64:
    push %rax
    push %rdx
    
    mov %al, %ah      # 文字をAHに保存
    mov $0x3f8, %dx
    add $5, %dx
wait_tx_64:
    in %dx, %al
    test $0x20, %al
    jz wait_tx_64
    mov $0x3f8, %dx
    mov %ah, %al      # 保存した文字をALに復帰
    out %al, %dx
    
    pop %rdx
    pop %rax
    ret

# メッセージ定義
.section .rodata
starting_msg:
    .asciz "Starting 32-bit kernel with 64-bit transition...\r\n"

entering_64_msg:
    .asciz "Entering 64-bit long mode...\r\n"

success_64_msg:
    .asciz "64-bit long mode activated successfully!\r\n"

no_64bit_error_msg:
    .asciz "ERROR: 64-bit long mode not supported!\r\n"

# 64ビット用GDT
.section .data
.align 8
gdt64_start:
    .quad 0                         # NULL セグメント
    .quad 0x00af9a000000ffff        # 64ビットコードセグメント
    .quad 0x00af92000000ffff        # 64ビットデータセグメント
gdt64_end:

gdt64_descriptor:
    .word gdt64_end - gdt64_start - 1
    .long gdt64_start

# ページテーブル（4KB境界でアライン）
.section .bss
.align 4096
pml4_table:
    .space 4096

.align 4096
pdp_table:
    .space 4096

.align 4096
pd_table:
    .space 4096