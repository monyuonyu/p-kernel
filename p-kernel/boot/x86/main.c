#include <stdint.h>

/* I/Oポートアクセス (32ビットプロテクトモード対応) */
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

/* 32ビットC言語のエントリポイント */
void main() {
    serial_init();
    print("32-bit C kernel started!\r\n");
    print("Protected mode C function executing successfully!\r\n");
    
    // 32ビット算術テスト
    uint32_t test_val = 0x12345678;
    uint32_t result = test_val * 2;
    
    if (result == 0x2468ACF0) {
        print("32-bit arithmetic test: PASSED!\r\n");
    } else {
        print("32-bit arithmetic test: FAILED!\r\n");
    }
    
    // メモリアクセステスト
    uint32_t *mem_test = (uint32_t*)0x200000;
    *mem_test = 0xDEADBEEF;
    if (*mem_test == 0xDEADBEEF) {
        print("32-bit memory access test: PASSED!\r\n");
    } else {
        print("32-bit memory access test: FAILED!\r\n");
    }
    
    print("32-bit protected mode kernel initialization complete!\r\n");

    // 無限ループ
    for(;;);
}