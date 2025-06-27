#include <stdint.h>

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

/* 64ビット対応チェック機能付き32ビットカーネル */
void main() {
    serial_init();
    print("Hybrid 32/64-bit capable kernel started!\r\n");
    print("64-bit CPU support detection completed in assembly\r\n");
    
    // 基本的な32ビット機能テスト
    uint32_t test_val = 0x12345678;
    uint32_t result = test_val * 2;
    
    if (result == 0x2468ACF0) {
        print("32-bit arithmetic test: PASSED!\r\n");
    } else {
        print("32-bit arithmetic test: FAILED!\r\n");
    }
    
    // 64ビット整数演算テスト（32ビットコンパイラでも可能）
    uint64_t test64 = 0x123456789ABCDEF0ULL;
    uint64_t result64 = test64 + 1;
    
    if (result64 == 0x123456789ABCDEF1ULL) {
        print("64-bit integer arithmetic (emulated): PASSED!\r\n");
    } else {
        print("64-bit integer arithmetic (emulated): FAILED!\r\n");
    }
    
    print("Kernel with 64-bit awareness initialization complete!\r\n");
    print("Note: Full 64-bit long mode transition will be implemented in future\r\n");

    // 無限ループ
    for(;;);
}