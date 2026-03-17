#include <stdint.h>
#include <stddef.h>
#include "idt.h"
#include "memory.h"
#include "pic.h"
#include "timer.h"

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
    print("=== p-kernel x86 boot ===\r\n");

    /* IDT初期化 */
    print("[INIT] IDT...\r\n");
    idt_init();
    idt_install();
    print("[OK]   IDT\r\n");

    /* 物理メモリ初期化 */
    print("[INIT] Memory...\r\n");
    memory_init();
    memory_dump_regions();
    print("[OK]   Memory\r\n");

    /* PIC初期化 (IRQ0-7: INT32-39, IRQ8-15: INT40-47) */
    print("[INIT] PIC (8259A)...\r\n");
    pic_init();
    print("[OK]   PIC\r\n");

    /* PIT初期化 (100Hz) */
    print("[INIT] PIT timer (100Hz)...\r\n");
    timer_init(TIMER_HZ);
    pic_unmask_irq(IRQ_TIMER);   /* IRQ0 (タイマー) のみ有効化 */
    print("[OK]   PIT\r\n");

    /* 割り込み有効化 */
    print("[INIT] Enabling interrupts...\r\n");
    asm volatile ("sti");
    print("[OK]   Interrupts enabled\r\n");

    /* タイマー動作確認 (5秒間、1秒ごとに報告) */
    print("\r\n=== Timer test (5 seconds) ===\r\n");

    uint64_t last_report = 0;
    uint32_t seconds = 0;

    while (seconds < 5) {
        uint64_t ticks = timer_get_ticks();

        if (ticks - last_report >= (uint64_t)TIMER_HZ) {
            seconds++;
            last_report = ticks;

            /* 秒数を表示 */
            char buf[4];
            buf[0] = '0' + seconds;
            buf[1] = 's';
            buf[2] = ' ';
            buf[3] = '\0';
            print("[TICK] ");
            print(buf);

            /* tick数を16進で表示 */
            char hex[19];
            hex[0] = '(';
            hex[1] = 't';
            hex[2] = 'i';
            hex[3] = 'c';
            hex[4] = 'k';
            hex[5] = 's';
            hex[6] = '=';
            for (int i = 7; i < 15; i++) {
                int nibble = (ticks >> ((14 - i) * 4)) & 0xF;
                hex[i] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
            }
            hex[15] = ')';
            hex[16] = '\r';
            hex[17] = '\n';
            hex[18] = '\0';
            print(hex);
        }

        asm volatile ("hlt");  /* 次の割り込みまで待機 */
    }

    print("\r\n=== Timer test PASSED! ===\r\n");
    print("PIC + PIT working correctly.\r\n");
    print("\r\nKernel idle loop.\r\n");

    /* アイドルループ */
    for (;;) {
        asm volatile ("hlt");
    }
}
