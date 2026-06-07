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

# ============================================================
# IRQハンドラスタブ (IRQ0-15 → INT 32-47)
# PIC初期化でリマップ済み
#
# 重要: IA-32eモード (ロングモード) では、64ビット割り込みゲートの
# ターゲットCSは必ず64ビットコードセグメント (L=1) でなければならない。
# CS=0x08 (L=0) を使うとGPFが発生する。
# 従って、IDTゲートはCS=0x18 (64ビット) を使用し、
# ハンドラは64ビットモードで実行される。
#
# 設計:
# 1. CPUは64ビット割り込みフレームをプッシュし、64ビットモードで
#    irqN スタブへジャンプ (CS=0x18)
# 2. 64ビットモードで: irq_num/dummy をpushq、レジスタ保存
# 3. 32ビット互換モードへ far jump してC関数を呼び出す
# 4. C関数から戻ったら 64ビットモードへ far jump
# 5. レジスタ復元、iretq
#
# 64ビットモードでのスタックレイアウト (irq_common_stub entry時):
#   [rsp+ 0] irq_num  (8バイト: pushqで積んだ値)
#   [rsp+ 8] dummy    (8バイト)
#   [rsp+16] RIP      (CPU 64ビット割り込みフレーム)
#   [rsp+24] CS
#   [rsp+32] RFLAGS
#   [rsp+40] RSP
#   [rsp+48] SS
# ============================================================

.extern irq_handler

# IRQスタブマクロ: 64ビットモードで実行される (CS=0x18)
# .byte 0x48, 0x6a はREX.W + PUSH = pushq (8バイト)
.macro IRQ_STUB num
.global irq\num
irq\num:
    .byte 0x48, 0x6a, 0x00      # pushq $0  (ダミーエラーコード, 8バイト)
    .byte 0x48, 0x6a, \num      # pushq $num (IRQ番号, 8バイト)
    jmp irq_common_stub
.endm

IRQ_STUB 0    # タイマー (PIT)
IRQ_STUB 1    # キーボード
IRQ_STUB 2    # スレーブPICカスケード
IRQ_STUB 3    # COM2
IRQ_STUB 4    # COM1
IRQ_STUB 5    # LPT2
IRQ_STUB 6    # フロッピー
IRQ_STUB 7    # LPT1 / スプリアス
IRQ_STUB 8    # RTC
IRQ_STUB 9    # 空き
IRQ_STUB 10   # 空き
IRQ_STUB 11   # 空き
IRQ_STUB 12   # PS/2マウス
IRQ_STUB 13   # FPU
IRQ_STUB 14   # プライマリATA
IRQ_STUB 15   # セカンダリATA

# 共通IRQスタブ (64ビットモードで実行, CS=0x18)
#
# ステップ1: 64ビットモードでレジスタ保存
# ステップ2: retfq で 32ビット互換モードへ切り替え (CS=0x08)
# ステップ3: C関数 irq_handler を呼び出す
# ステップ4: retf で 64ビットモードへ戻す (CS=0x18)
# ステップ5: レジスタ復元, iretq
#
# 注意: 64ビットモードで 0xEA (direct far jump) は無効命令。
# 代わりに retfq/retf を使う:
#   pushq selector; pushq offset; rex.W retf  → far jump (64ビット→32ビット)
#   push  selector; push  offset; retf        → far jump (32ビット→64ビット)
#
# スタックレイアウト (irq_common_stub 入時, 64ビットモード):
#   [rsp+ 0] irq_num  (8バイト)
#   [rsp+ 8] dummy    (8バイト)
#   [rsp+16] RIP      (CPUが積んだ64ビット割り込みフレーム)
#   [rsp+24] CS       (8バイト)
#   [rsp+32] RFLAGS   (8バイト)
#   [rsp+40] RSP      (8バイト)
#   [rsp+48] SS       (8バイト)
irq_common_stub:
    # 64ビットモード: レジスタを pushq で保存
    .byte 0x48, 0x50            # pushq %rax
    .byte 0x48, 0x51            # pushq %rcx
    .byte 0x48, 0x52            # pushq %rdx

    # 保存後スタック:
    #   [rsp+ 0] rdx
    #   [rsp+ 8] rcx
    #   [rsp+16] rax
    #   [rsp+24] irq_num  ← 0x18
    #   ...

    # irq_num を eax に読み込む (regparm(1): 第1引数は eax)
    .byte 0x8b, 0x44, 0x24, 0x18  # movl 24(%rsp), %eax

    # 32ビット互換モードへ遠ジャンプ: pushq CS; pushq offset; retfq
    # pushq $0x08 (32ビット互換 CS)
    .byte 0x48, 0x6a, 0x08        # pushq $0x08
    # pushq $irq_call32_stub (32ビットオフセット → 64ビットとしてプッシュ)
    .byte 0x68                    # push imm32 (zero-extends to 64-bit)
    .long irq_call32_stub
    # rex.W retf: 64ビットモードで far return → CS:RIP をポップ
    .byte 0x48, 0xcb              # rex.W retf

# 32ビット互換モードで実行される部分 (CS=0x08)
irq_call32_stub:
    # irq_handler 呼び出し (eax に irq_num, regparm(1))
    call irq_handler

    # 64ビットモードへ戻る: push selector; push offset; retf
    # push $0x18 (64ビット CS)
    .byte 0x6a, 0x18              # push $0x18
    # push $irq_back64_stub
    .byte 0x68                    # push imm32
    .long irq_back64_stub
    # retf: 32ビットモードで far return → EIP, CS をポップ → 64ビットへ
    .byte 0xcb                    # retf

# 64ビットモードで実行される部分 (CS=0x18)
irq_back64_stub:
    # レジスタ復帰 (popq)
    .byte 0x48, 0x5a            # popq %rdx
    .byte 0x48, 0x59            # popq %rcx
    .byte 0x48, 0x58            # popq %rax

    # irq_num + dummy を除去 (8+8=16バイト)
    .byte 0x48, 0x83, 0xc4, 0x10  # addq $16, %rsp

    # 64ビット割り込みから復帰
    .byte 0x48, 0xcf            # iretq

# ============================================================
# INT 0x80 システムコールハンドラ
#
# IDTゲート: CS=0x18 (64ビット), DPL=3 (ring3から呼び出し可能)
#
# 動作概要:
#   1. 64ビットモードでエントリ (CS=0x18)
#      ring3→ring0 特権切り替え時にCPUが64ビット割り込みフレームを積む:
#        [rsp+32] SS   (ring3)
#        [rsp+24] RSP  (ring3)
#        [rsp+16] RFLAGS
#        [rsp+ 8] CS   (ring3 = 0x23)
#        [rsp+ 0] RIP  (ring3 EIP)
#   2. EAX=nr, EBX=arg0, ECX=arg1, EDX=arg2 をpushq保存
#   3. 32ビット互換モードへ far jump → syscall_dispatch() 呼び出し
#   4. 64ビットモードへ far jump → iretq でring3へ復帰
#
# スタックレイアウト (pushq保存後, 64ビットモード):
#   [rsp+ 0] rax (syscall nr / return value slot)
#   [rsp+ 8] rcx (arg1)
#   [rsp+16] rdx (arg2)
#   [rsp+24] rbx (arg0)
#   [rsp+32] 64ビット割り込みフレーム (RIP,CS,RFLAGS,RSP,SS)
# ============================================================

.extern syscall_dispatch

.global syscall_isr
syscall_isr:
    # 64ビットモード: システムコール引数レジスタを pushq で保存
    # 保存順: rax(nr), rcx(arg1), rdx(arg2), rbx(arg0), rsi(ring3 ESI)
    .byte 0x48, 0x50            # pushq %rax   (syscall番号 / 戻り値スロット)
    .byte 0x48, 0x51            # pushq %rcx   (arg1)
    .byte 0x48, 0x52            # pushq %rdx   (arg2)
    .byte 0x48, 0x53            # pushq %rbx   (arg0)
    .byte 0x48, 0x56            # pushq %rsi   (ring3 ESI保存: isr.Sは保存しないのでここで保存)

    # 32ビット互換モードへ far jump: pushq CS; pushq offset; rex.W retf
    .byte 0x48, 0x6a, 0x08      # pushq $0x08  (32ビット互換 CS)
    .byte 0x68                  # push imm32
    .long syscall_call32_stub
    .byte 0x48, 0xcb            # rex.W retf   (64→32ビット far jump)

# ---- 32ビット互換モードで実行される部分 (CS=0x08) ----
syscall_call32_stub:
    # DS/SS/ESをカーネルデータセレクタ(0x10)に再ロード
    # ring3→ring0割り込み時: CPUがSSをnull(0x0000)にセット(IA-32eモード)
    # DSはring3のUSER_DS(0x2B)が残る可能性がある
    # カーネルのC関数が正しくカーネルスタック/データにアクセスできるよう修正
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    # スタックレイアウト (64ビットpushq = 8バイト/要素):
    #   [esp+ 0] rsi low32 (ring3 ESI)
    #   [esp+ 8] rbx low32 (arg0)
    #   [esp+16] rdx low32 (arg2)
    #   [esp+24] rcx low32 (arg1)
    #   [esp+32] rax low32 (syscall番号)
    #
    # syscall_dispatch(nr, arg0, arg1, arg2) を cdecl で呼び出す

    mov  8(%esp), %ebx          # arg0
    mov 16(%esp), %edx          # arg2
    mov 24(%esp), %ecx          # arg1
    mov 32(%esp), %eax          # nr

    push %edx                   # push arg2
    push %ecx                   # push arg1
    push %ebx                   # push arg0
    push %eax                   # push nr
    call syscall_dispatch
    add  $16, %esp

    # 戻り値 (EAX) を保存済み rax スロットに書き戻す
    # → 64ビットモードで popq %rax するとring3の EAX に戻る
    mov  %eax, 32(%esp)

    # 64ビットモードへ戻る: push CS; push offset; retf
    .byte 0x6a, 0x18            # push $0x18   (64ビット CS)
    .byte 0x68                  # push imm32
    .long syscall_back64_stub
    .byte 0xcb                  # retf          (32→64ビット far jump)

# ---- 64ビットモードで実行される部分 (CS=0x18) ----
syscall_back64_stub:
    # pushq の逆順で popq (LIFO)
    .byte 0x48, 0x5e            # popq %rsi   (ring3 ESI復元)
    .byte 0x48, 0x5b            # popq %rbx   (arg0, 破棄)
    .byte 0x48, 0x5a            # popq %rdx   (arg2, 破棄)
    .byte 0x48, 0x59            # popq %rcx   (arg1, 破棄)
    .byte 0x48, 0x58            # popq %rax   (戻り値 → ring3 EAX)

    # ring3 へ iretq 復帰
    .byte 0x48, 0xcf            # iretq