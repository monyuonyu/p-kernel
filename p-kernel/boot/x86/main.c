#include <stdint.h>

/* I/Oポートアクセス (16ビットモード対応) */
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %%al, %%dx" : : "a"(val), "d"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %%dx, %%al" : "=a"(ret) : "d"(port));
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

/* C言語のエントリポイント */
void main() {
    serial_init();
    print("Hello from C!\r\n");
    print("C main function executing...\r\n");
    print("Serial port initialized successfully!\r\n");

    // 無限ループ
    for(;;);
}