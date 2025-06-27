# 64ビット例外ハンドラ (簡素版)
# 32ビットアセンブラから64ビット命令を生成

.extern exception_handler

# Division Error (0) - エラーコードなし
.global isr0
isr0:
    # 64ビットpush命令をバイトコードで記述
    .byte 0x48, 0x6a, 0x00      # pushq $0 (ダミーエラーコード)
    .byte 0x48, 0x6a, 0x00      # pushq $0 (例外番号)
    jmp isr_common_stub

# Debug Exception (1) - エラーコードなし  
.global isr1
isr1:
    .byte 0x48, 0x6a, 0x00      # pushq $0 (ダミーエラーコード)
    .byte 0x48, 0x6a, 0x01      # pushq $1 (例外番号)
    jmp isr_common_stub

# NMI (2) - エラーコードなし
.global isr2
isr2:
    .byte 0x48, 0x6a, 0x00      # pushq $0 (ダミーエラーコード)
    .byte 0x48, 0x6a, 0x02      # pushq $2 (例外番号)
    jmp isr_common_stub

# Breakpoint (3) - エラーコードなし
.global isr3
isr3:
    .byte 0x48, 0x6a, 0x00      # pushq $0 (ダミーエラーコード)
    .byte 0x48, 0x6a, 0x03      # pushq $3 (例外番号)
    jmp isr_common_stub

# Overflow (4) - エラーコードなし
.global isr4
isr4:
    .byte 0x48, 0x6a, 0x00      # pushq $0 (ダミーエラーコード)
    .byte 0x48, 0x6a, 0x04      # pushq $4 (例外番号)
    jmp isr_common_stub

# BOUND Range Exceeded (5) - エラーコードなし
.global isr5
isr5:
    .byte 0x48, 0x6a, 0x00      # pushq $0 (ダミーエラーコード)
    .byte 0x48, 0x6a, 0x05      # pushq $5 (例外番号)
    jmp isr_common_stub

# Invalid Opcode (6) - エラーコードなし
.global isr6
isr6:
    .byte 0x48, 0x6a, 0x00      # pushq $0 (ダミーエラーコード)
    .byte 0x48, 0x6a, 0x06      # pushq $6 (例外番号)
    jmp isr_common_stub

# Device Not Available (7) - エラーコードなし
.global isr7
isr7:
    .byte 0x48, 0x6a, 0x00      # pushq $0 (ダミーエラーコード)
    .byte 0x48, 0x6a, 0x07      # pushq $7 (例外番号)
    jmp isr_common_stub

# Double Fault (8) - エラーコードあり
.global isr8
isr8:
    .byte 0x48, 0x6a, 0x08      # pushq $8 (例外番号)
    jmp isr_common_stub

# Invalid TSS (10) - エラーコードあり
.global isr10
isr10:
    .byte 0x48, 0x6a, 0x0a      # pushq $10 (例外番号)
    jmp isr_common_stub

# Segment Not Present (11) - エラーコードあり
.global isr11
isr11:
    .byte 0x48, 0x6a, 0x0b      # pushq $11 (例外番号)
    jmp isr_common_stub

# Stack Fault (12) - エラーコードあり
.global isr12
isr12:
    .byte 0x48, 0x6a, 0x0c      # pushq $12 (例外番号)
    jmp isr_common_stub

# General Protection Fault (13) - エラーコードあり
.global isr13
isr13:
    .byte 0x48, 0x6a, 0x0d      # pushq $13 (例外番号)
    jmp isr_common_stub

# Page Fault (14) - エラーコードあり
.global isr14
isr14:
    .byte 0x48, 0x6a, 0x0e      # pushq $14 (例外番号)
    jmp isr_common_stub

# FPU Error (16) - エラーコードなし
.global isr16
isr16:
    .byte 0x48, 0x6a, 0x00      # pushq $0 (ダミーエラーコード)
    .byte 0x48, 0x6a, 0x10      # pushq $16 (例外番号)
    jmp isr_common_stub

# Alignment Check (17) - エラーコードあり
.global isr17
isr17:
    .byte 0x48, 0x6a, 0x11      # pushq $17 (例外番号)
    jmp isr_common_stub

# Machine Check (18) - エラーコードなし
.global isr18
isr18:
    .byte 0x48, 0x6a, 0x00      # pushq $0 (ダミーエラーコード)
    .byte 0x48, 0x6a, 0x12      # pushq $18 (例外番号)
    jmp isr_common_stub

# SIMD Exception (19) - エラーコードなし
.global isr19
isr19:
    .byte 0x48, 0x6a, 0x00      # pushq $0 (ダミーエラーコード)
    .byte 0x48, 0x6a, 0x13      # pushq $19 (例外番号)
    jmp isr_common_stub

# 共通例外ハンドラスタブ（簡素版）
isr_common_stub:
    # C例外ハンドラ呼び出し（引数はスタック上）
    # 簡易版: レジスタ保存は最小限
    .byte 0x48, 0x50            # pushq %rax
    .byte 0x48, 0x51            # pushq %rcx
    .byte 0x48, 0x52            # pushq %rdx
    .byte 0x48, 0x56            # pushq %rsi
    .byte 0x48, 0x57            # pushq %rdi
    
    # 引数設定: exception_num, error_code
    .byte 0x48, 0x8b, 0x7c, 0x24, 0x30  # movq 48(%rsp), %rdi (例外番号)
    .byte 0x48, 0x8b, 0x74, 0x24, 0x38  # movq 56(%rsp), %rsi (エラーコード)
    
    call exception_handler
    
    # レジスタ復帰
    .byte 0x48, 0x5f            # popq %rdi
    .byte 0x48, 0x5e            # popq %rsi
    .byte 0x48, 0x5a            # popq %rdx
    .byte 0x48, 0x59            # popq %rcx
    .byte 0x48, 0x58            # popq %rax
    
    # スタッククリーンアップ (例外番号 + エラーコード)
    .byte 0x48, 0x83, 0xc4, 0x10  # addq $16, %rsp
    
    # 例外から復帰
    .byte 0x48, 0xcf            # iretq