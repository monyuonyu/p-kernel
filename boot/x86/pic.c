#include "pic.h"
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* I/O待機用 (古いハードウェアへの配慮) */
static inline void io_wait(void) {
    outb(0x80, 0);
}

void pic_init(void) {
    /* ICW1: 初期化開始、ICW4が必要 */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();

    /* ICW2: ベクタオフセット */
    outb(PIC1_DATA, IRQ_BASE);       /* IRQ0-7  → INT 32-39 */
    io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8);   /* IRQ8-15 → INT 40-47 */
    io_wait();

    /* ICW3: カスケード設定 */
    outb(PIC1_DATA, 0x04); io_wait(); /* マスター: IRQ2にスレーブ */
    outb(PIC2_DATA, 0x02); io_wait(); /* スレーブ: カスケードID=2 */

    /* ICW4: 8086モード */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* 全IRQをマスク（個別に有効化する） */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* regparm(1): 引数をeaxで受け取る (64ビットモードから呼ばれるため) */
void __attribute__((regparm(1))) pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

void __attribute__((regparm(1))) pic_mask_irq(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit   = irq & 7;
    outb(port, inb(port) | (1 << bit));
}

void __attribute__((regparm(1))) pic_unmask_irq(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit   = irq & 7;
    outb(port, inb(port) & ~(1 << bit));
}
