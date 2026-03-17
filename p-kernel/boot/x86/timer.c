#include "timer.h"
#include "pic.h"
#include <stdint.h>

static volatile uint64_t tick_count = 0;

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void timer_init(uint32_t hz) {
    uint32_t divisor = PIT_BASE_FREQ / hz;

    /* PIT チャンネル0: モード3 (矩形波), バイナリカウント */
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

uint64_t timer_get_ticks(void) {
    return tick_count;
}

void timer_irq_handler(void) {
    tick_count++;
}
