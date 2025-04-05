#include <stdint.h>

/* I/Oポートアクセス */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* QEMUシリアルポート (COM1) */
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

/* シリアルポートに1文字出力 */
void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0);
    outb(COM1, c);
}

/* 文字列出力 */
void print(const char *str) {
    while (*str) {
        serial_putc(*str++);
    }
}

/* メイン関数 */
void main() {
    serial_init();
    print("p-kernel x64 bootloader started\r\n");

    // TODO: カーネルロード処理を実装

    print("Booting kernel...\r\n");
    
    // カーネルエントリポイントへジャンプ
    // void (*kernel_entry)() = (void (*)())0x100000;
    // kernel_entry();
}
