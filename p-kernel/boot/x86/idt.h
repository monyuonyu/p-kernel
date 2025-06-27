#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* 64ビットIDTエントリ構造体 */
struct idt_entry {
    uint16_t offset_low;    // オフセット下位16ビット
    uint16_t selector;      // セグメントセレクタ
    uint8_t  ist;          // Interrupt Stack Table（0使用）
    uint8_t  type_attr;    // タイプとアトリビュート
    uint16_t offset_mid;    // オフセット中位16ビット
    uint32_t offset_high;   // オフセット上位32ビット
    uint32_t zero;         // 予約（0）
} __attribute__((packed));

/* IDTポインタ構造体 */
struct idt_ptr {
    uint16_t limit;        // IDTサイズ - 1
    uint64_t base;         // IDTベースアドレス
} __attribute__((packed));

/* IDT設定値 */
#define IDT_ENTRIES         256
#define IDT_INTERRUPT_GATE  0x8E  // 64ビット割り込みゲート
#define IDT_TRAP_GATE       0x8F  // 64ビットトラップゲート
#define KERNEL_CS           0x08  // カーネルコードセグメント

/* 例外番号定義 */
#define EXCEPTION_DE        0   // Division Error
#define EXCEPTION_DB        1   // Debug
#define EXCEPTION_NMI       2   // Non-Maskable Interrupt
#define EXCEPTION_BP        3   // Breakpoint
#define EXCEPTION_OF        4   // Overflow
#define EXCEPTION_BR        5   // BOUND Range Exceeded
#define EXCEPTION_UD        6   // Invalid Opcode
#define EXCEPTION_NM        7   // Device Not Available
#define EXCEPTION_DF        8   // Double Fault
#define EXCEPTION_TS        10  // Invalid TSS
#define EXCEPTION_NP        11  // Segment Not Present
#define EXCEPTION_SS        12  // Stack-Segment Fault
#define EXCEPTION_GP        13  // General Protection
#define EXCEPTION_PF        14  // Page Fault
#define EXCEPTION_MF        16  // x87 FPU Floating-Point Error
#define EXCEPTION_AC        17  // Alignment Check
#define EXCEPTION_MC        18  // Machine Check
#define EXCEPTION_XM        19  // SIMD Floating-Point Exception

/* 関数プロトタイプ */
void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t sel, uint8_t flags);
void idt_install(void);

/* 例外ハンドラプロトタイプ */
extern void isr0(void);   // Division Error
extern void isr1(void);   // Debug
extern void isr2(void);   // NMI
extern void isr3(void);   // Breakpoint
extern void isr4(void);   // Overflow
extern void isr5(void);   // Bound Range Exceeded
extern void isr6(void);   // Invalid Opcode
extern void isr7(void);   // Device Not Available
extern void isr8(void);   // Double Fault
extern void isr10(void);  // Invalid TSS
extern void isr11(void);  // Segment Not Present
extern void isr12(void);  // Stack Fault
extern void isr13(void);  // General Protection Fault
extern void isr14(void);  // Page Fault
extern void isr16(void);  // FPU Error
extern void isr17(void);  // Alignment Check
extern void isr18(void);  // Machine Check
extern void isr19(void);  // SIMD Exception

#endif /* IDT_H */