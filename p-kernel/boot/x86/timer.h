#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#define PIT_BASE_FREQ   1193182UL   /* PITの基本クロック周波数 (Hz) */
#define TIMER_HZ        100         /* タイマー割り込み周波数 */

void timer_init(uint32_t hz);
uint64_t timer_get_ticks(void);
void timer_irq_handler(void);

#endif /* TIMER_H */
