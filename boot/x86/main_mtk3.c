#include <stdint.h>
#include <stddef.h>
#include "idt.h"
#include "memory.h"
#include "pic.h"
#include "timer.h"

/* μT-Kernel 3.0 起動 (kernel/mtkernel3): knl_startup_hw で
 * システムメモリ領域を設定し、knl_main が sysinit を実行する */
extern void knl_startup_hw(void);
extern int  knl_main(void);

#ifdef ARK_BAREMETAL_SMOKE
/* Bare-metal ARK smoke test (arch/x86/ark_bdev.c). Local prototype so this
 * TU need not pull in kernel.h; INT == int on i686. Runs before T-Kernel,
 * using ATA-PIO (polled) directly, then halts with a PASS/FAIL verdict. */
extern int ark_baremetal_smoke(void (*emit)(const char *));
#endif


/* I/Oポートアクセス */
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

void serial_init() {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
}

void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0);
    outb(COM1, c);
}

void print(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        serial_putc(str[i]);
    }
}

/* p-kernel x86 メインエントリ */
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
    print("[OK]   Memory\r\n");

#ifdef ARK_BAREMETAL_SMOKE
    /* ARK on REAL hardware: format/write/sync/remount/read round-trip on the
     * physical ATA disk, then halt. No T-Kernel needed — IDE PIO is polled. */
    {
        int rc = ark_baremetal_smoke(print);
        if (rc == 0)
            print("[ARK] SMOKE RESULT: PASS\r\n");
        else
            print("[ARK] SMOKE RESULT: FAIL\r\n");
        print("=== ark-smoke done — halting ===\r\n");
        for (;;) asm volatile ("hlt");
    }
#endif

    /* PIC初期化 (全IRQマスク - T-Kernelが必要なものを開ける) */
    print("[INIT] PIC (8259A)...\r\n");
    pic_init();
    /* 全IRQをマスク: T-Kernel の tkdev_initialize() がアンマスクする */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    print("[OK]   PIC\r\n");

    /* T-Kernel 起動 */
    print("[BOOT] Starting T-Kernel...\r\n");
    knl_startup_hw();
    knl_main();

    /* ここには到達しない */
    print("[ERROR] T-Kernel returned!\r\n");
    for (;;) {
        asm volatile ("hlt");
    }
}
