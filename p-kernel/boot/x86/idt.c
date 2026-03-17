#include "idt.h"
#include "pic.h"
#include "timer.h"

/* IDTテーブル（256エントリ） */
static struct idt_entry idt_table[IDT_ENTRIES];
static struct idt_ptr idt_pointer;

/* IDTエントリ設定関数 */
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t sel, uint8_t flags) {
    idt_table[num].offset_low  = handler & 0xFFFF;
    idt_table[num].offset_mid  = (handler >> 16) & 0xFFFF;
    idt_table[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt_table[num].selector    = sel;
    idt_table[num].ist         = 0;           // IST使用しない
    idt_table[num].type_attr   = flags;
    idt_table[num].zero        = 0;
}

/* IDT初期化 */
void idt_init(void) {
    /* IDTポインタ設定 */
    idt_pointer.limit = (sizeof(struct idt_entry) * IDT_ENTRIES) - 1;
    idt_pointer.base  = (uint64_t)&idt_table;

    /* IDTテーブルをクリア */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_table[i].offset_low  = 0;
        idt_table[i].offset_mid  = 0;
        idt_table[i].offset_high = 0;
        idt_table[i].selector    = 0;
        idt_table[i].ist         = 0;
        idt_table[i].type_attr   = 0;
        idt_table[i].zero        = 0;
    }

    /* 例外ハンドラを設定 (CS=0x18: IA-32eモードでは64ビットCSが必須) */
    idt_set_gate(EXCEPTION_DE, (uint64_t)isr0,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_DB, (uint64_t)isr1,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_NMI,(uint64_t)isr2,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_BP, (uint64_t)isr3,  KERNEL64_CS, IDT_TRAP_GATE);
    idt_set_gate(EXCEPTION_OF, (uint64_t)isr4,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_BR, (uint64_t)isr5,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_UD, (uint64_t)isr6,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_NM, (uint64_t)isr7,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_DF, (uint64_t)isr8,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_TS, (uint64_t)isr10, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_NP, (uint64_t)isr11, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_SS, (uint64_t)isr12, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_GP, (uint64_t)isr13, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_PF, (uint64_t)isr14, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_MF, (uint64_t)isr16, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_AC, (uint64_t)isr17, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_MC, (uint64_t)isr18, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_XM, (uint64_t)isr19, KERNEL64_CS, IDT_INTERRUPT_GATE);

    /* IRQハンドラ登録 (INT 32-47, CS=0x18: 64ビットゲート必須) */
    idt_set_gate(IRQ_VECTOR_BASE +  0, (uint64_t)irq0,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  1, (uint64_t)irq1,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  2, (uint64_t)irq2,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  3, (uint64_t)irq3,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  4, (uint64_t)irq4,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  5, (uint64_t)irq5,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  6, (uint64_t)irq6,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  7, (uint64_t)irq7,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  8, (uint64_t)irq8,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  9, (uint64_t)irq9,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE + 10, (uint64_t)irq10, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE + 11, (uint64_t)irq11, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE + 12, (uint64_t)irq12, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE + 13, (uint64_t)irq13, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE + 14, (uint64_t)irq14, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE + 15, (uint64_t)irq15, KERNEL64_CS, IDT_INTERRUPT_GATE);
}

/* IDTをCPUに登録 */
void idt_install(void) {
    asm volatile ("lidt %0" : : "m" (idt_pointer));
}

/* 外部シリアル出力関数 */
extern void print(const char *str);

/* 共通例外ハンドラ */
void exception_handler(uint64_t exception_num, uint64_t error_code) {
    const char* exception_messages[] = {
        "Division Error",
        "Debug Exception", 
        "Non-Maskable Interrupt",
        "Breakpoint",
        "Overflow",
        "BOUND Range Exceeded",
        "Invalid Opcode",
        "Device Not Available",
        "Double Fault",
        "Coprocessor Segment Overrun",
        "Invalid TSS",
        "Segment Not Present",
        "Stack-Segment Fault",
        "General Protection Fault",
        "Page Fault",
        "Reserved",
        "x87 FPU Error",
        "Alignment Check",
        "Machine Check",
        "SIMD Floating-Point Exception"
    };

    print("\r\n=== KERNEL EXCEPTION ===\r\n");
    print("Exception: ");
    if (exception_num < 20) {
        print(exception_messages[exception_num]);
    } else {
        print("Unknown Exception");
    }
    print("\r\n");
    
    // エラーコードがある例外の場合
    if (exception_num == 8 || (exception_num >= 10 && exception_num <= 14) || exception_num == 17) {
        print("Error Code: ");
        // 簡易的な16進数表示（後でprintk実装時に改善）
        char hex_str[19] = "0x";
        for (int i = 15; i >= 0; i--) {
            int digit = (error_code >> (i * 4)) & 0xF;
            hex_str[17-i] = digit < 10 ? '0' + digit : 'A' + digit - 10;
        }
        hex_str[18] = '\0';
        print(hex_str);
        print("\r\n");
    }

    // ブレークポイント例外の場合は復帰可能
    if (exception_num == 3) {
        print("Breakpoint exception handled - continuing execution...\r\n");
        return;  // 例外から復帰
    }
    
    print("System halted.\r\n");

    /* 致命的例外の場合はシステム停止 */
    while (1) {
        asm volatile ("hlt");
    }
}

/* 共通IRQディスパッチャ (isr.S の irq_common_stub から呼ばれる) */
/* regparm(1): 第1引数を %eax/rax レジスタで渡す (i686でもレジスタ渡し) */
void __attribute__((regparm(1))) irq_handler(uint32_t irq_num) {
    /* IRQに対応するハンドラを呼び出す */
    if (irq_num == IRQ_TIMER) {
        timer_irq_handler();
    }

    /* PICにEnd of Interruptを送信 */
    pic_send_eoi((uint8_t)irq_num);
}

/* IRQハンドラ登録関数 (将来の拡張用スタブ) */
void irq_register_handler(uint8_t irq, void (*handler)(void)) {
    (void)irq;
    (void)handler;
}