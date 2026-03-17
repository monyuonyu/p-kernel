#ifndef PIC_H
#define PIC_H

#include <stdint.h>

/* PIC I/Oポート */
#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1

/* End of Interrupt */
#define PIC_EOI     0x20

/* IRQベクタ番号 (INT 32〜47にリマップ) */
#define IRQ_BASE        32
#define IRQ_TIMER       0
#define IRQ_KEYBOARD    1
#define IRQ_SLAVE       2
#define IRQ_COM2        3
#define IRQ_COM1        4
#define IRQ_LPT2        5
#define IRQ_FLOPPY      6
#define IRQ_LPT1        7
#define IRQ_RTC         8
#define IRQ_MOUSE       12
#define IRQ_FPU         13
#define IRQ_PRIMARY_ATA 14
#define IRQ_SECONDARY_ATA 15

void pic_init(void);
/* regparm(1): 64ビットモードから呼ばれるため、引数はeaxで渡す */
void __attribute__((regparm(1))) pic_send_eoi(uint8_t irq);
void __attribute__((regparm(1))) pic_mask_irq(uint8_t irq);
void __attribute__((regparm(1))) pic_unmask_irq(uint8_t irq);

#endif /* PIC_H */
