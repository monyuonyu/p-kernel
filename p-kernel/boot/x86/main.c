#include <stdint.h>
#include <stddef.h>
#include "idt.h"
#include "memory.h"

/* I/Oポートアクセス (64ビットロングモード対応) */
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* シリアルポート (COM1) */
#define COM1 0x3F8

/* シリアルポート初期化 */
void serial_init() {
    outb(COM1 + 1, 0x00);    // Disable interrupts
    outb(COM1 + 3, 0x80);    // Enable DLAB
    outb(COM1 + 0, 0x03);    // Divisor = 3 (lo byte)
    outb(COM1 + 1, 0x00);    //           (hi byte)
    outb(COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7);    // Enable FIFO
}

void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0); // 送信バッファが空になるまで待つ
    outb(COM1, c);
}

void print(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        serial_putc(str[i]);
    }
}

/* 完全64ビットロングモード移行カーネル */
void main() {
    serial_init();
    print("=== 64-bit Long Mode Kernel with IDT ===\r\n");
    print("Successfully transitioned to 64-bit long mode!\r\n");
    print("Running C code called from 64-bit assembly context\r\n");
    
    /* IDT（割り込み記述子テーブル）初期化 */
    print("Initializing IDT (Interrupt Descriptor Table)...\r\n");
    idt_init();
    idt_install();
    print("IDT initialized successfully!\r\n");
    
    /* 物理メモリマップ初期化 */
    print("Initializing physical memory management...\r\n");
    memory_init();
    memory_dump_regions();
    
    // 64ビット整数演算テスト
    uint64_t test64_a = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t test64_b = 1;
    uint64_t result64 = test64_a + test64_b;
    
    print("64-bit integer arithmetic test: ");
    if (result64 == 0) {
        print("PASSED!\r\n");
    } else {
        print("FAILED!\r\n");
    }
    
    // 64ビット乗算テスト
    uint64_t mult_a = 0x100000000ULL;
    uint64_t mult_b = 0x100000000ULL;
    uint64_t mult_result = mult_a * mult_b;
    
    print("64-bit multiplication test: ");
    if (mult_result == 0) { // 64-bit overflow to 0
        print("PASSED!\r\n");
    } else {
        print("FAILED!\r\n");
    }
    
    // 大きな数値テスト
    uint64_t large_num = 0x123456789ABCDEF0ULL;
    print("64-bit large number test: ");
    if (large_num > 0xFFFFFFFFULL) {
        print("PASSED!\r\n");
    } else {
        print("FAILED!\r\n");
    }
    
    print("=== Long Mode Transition Complete! ===\r\n");
    print("Kernel is now running in 64-bit long mode environment\r\n");
    print("C code execution from 64-bit context confirmed!\r\n");
    
    /* IDT機能テスト */
    print("\r\n=== IDT Exception Handling Test ===\r\n");
    print("Testing breakpoint exception (INT3)...\r\n");
    
    // ブレークポイント例外をテスト（非致命的）
    asm volatile ("int3");  // Breakpoint例外を発生
    
    print("Breakpoint exception handling complete!\r\n");
    print("IDT is working correctly!\r\n");

    // 無限ループ
    for(;;);
}