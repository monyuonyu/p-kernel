# 64ビット例外ハンドラ (Interrupt Service Routines)
# 注意: 32ビットアセンブラでコンパイルするが、64ビットモードで実行される

# 外部関数
.extern exception_handler

# エラーコードなしの例外マクロ
.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    .byte 0x48; push $0        # pushq $0 (ダミーエラーコード)
    .byte 0x48; push $\num     # pushq $\num (例外番号)
    jmp isr_common_stub
.endm

# エラーコードありの例外マクロ  
.macro ISR_ERRCODE num
.global isr\num
isr\num:
    .byte 0x48; push $\num     # pushq $\num (例外番号)
    jmp isr_common_stub
.endm

# 例外ハンドラ定義
ISR_NOERRCODE 0   # Division Error
ISR_NOERRCODE 1   # Debug
ISR_NOERRCODE 2   # NMI
ISR_NOERRCODE 3   # Breakpoint
ISR_NOERRCODE 4   # Overflow
ISR_NOERRCODE 5   # Bound Range Exceeded
ISR_NOERRCODE 6   # Invalid Opcode
ISR_NOERRCODE 7   # Device Not Available
ISR_ERRCODE   8   # Double Fault
ISR_ERRCODE   10  # Invalid TSS
ISR_ERRCODE   11  # Segment Not Present
ISR_ERRCODE   12  # Stack Fault
ISR_ERRCODE   13  # General Protection Fault
ISR_ERRCODE   14  # Page Fault
ISR_NOERRCODE 16  # FPU Error
ISR_ERRCODE   17  # Alignment Check
ISR_NOERRCODE 18  # Machine Check
ISR_NOERRCODE 19  # SIMD Exception

# 共通例外ハンドラスタブ
isr_common_stub:
    # レジスタ保存
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    # セグメントレジスタ保存
    mov %ds, %ax
    pushq %rax
    
    # カーネルデータセグメント設定
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    
    # C例外ハンドラ呼び出し
    # 引数: exception_num (スタック上), error_code (スタック上)
    mov 144(%rsp), %rdi  # 例外番号 (第1引数)
    mov 152(%rsp), %rsi  # エラーコード (第2引数)
    call exception_handler
    
    # セグメントレジスタ復帰
    popq %rax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    
    # レジスタ復帰
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    
    # 例外番号とエラーコードをスタックから除去
    addq $16, %rsp
    
    # 例外から復帰
    iretq