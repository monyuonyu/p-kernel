/*
 *  tkdev_timer.h (x86)
 *  Hardware-dependent timer processing using PIT 8253 on x86/QEMU
 */

#ifndef _TKDEV_TIMER_
#define _TKDEV_TIMER_

#include <syslib.h>
#include <sysinfo.h>
#include "tkdev_conf.h"
#include "sysdef_depend.h"

/* Interrupt enable/disable wrappers for timer code */
#define ENAINT  __asm__ volatile("sti" ::: "memory")
#define DISINT  __asm__ volatile("cli" ::: "memory")

/* Settable interval range (millisecond) */
#define MIN_TIMER_PERIOD    1
#define MAX_TIMER_PERIOD    100

/* -----------------------------------------------------------------------
 *  PIT 8253 initialisation
 *  Channel 0, mode 3 (square wave), IRQ0 → INT 32
 * --------------------------------------------------------------------- */

Inline void knl_init_hw_timer(void)
{
    UINT div = PIT_BASE_HZ / TIMER_HZ;

    /* Channel 0, lobyte/hibyte, mode 3 (rate generator), binary */
    outb(PIT_CMD,  0x36);
    outb(PIT_CH0, (UB)(div & 0xFF));
    outb(PIT_CH0, (UB)((div >> 8) & 0xFF));
}

/* -----------------------------------------------------------------------
 *  T-Kernel timer interface
 * --------------------------------------------------------------------- */

Inline void knl_start_hw_timer(void)
{
    knl_init_hw_timer();
}

/* Called at the start of the timer interrupt handler */
Inline void knl_clear_hw_timer_interrupt(void)
{
    /* Send EOI to master PIC - done in irq_handler (boot/x86/pic.c) */
}

/* Called at the end of the timer interrupt handler */
Inline void knl_end_of_hw_timer_interrupt(void)
{
    /* nothing */
}

/* Stop the timer */
Inline void knl_terminate_hw_timer(void)
{
    /* Mask PIT IRQ0 */
    outb(0x21, inb(0x21) | 0x01);
}

/*
 * knl_get_hw_timer_nsec - nanoseconds since last timer tick (approx)
 * Read PIT channel 0 current count.
 */
Inline UW knl_get_hw_timer_nsec(void)
{
    UINT count;
    UINT imask = disint();

    /* Latch channel 0 */
    outb(PIT_CMD, 0x00);
    count  = (UINT)inb(PIT_CH0);
    count |= (UINT)inb(PIT_CH0) << 8;

    enaint(imask);

    /* count counts down from (PIT_BASE_HZ / TIMER_HZ) to 0 */
    UINT period_counts = PIT_BASE_HZ / TIMER_HZ;
    UINT elapsed = period_counts - count;
    /* Convert to nanoseconds: elapsed / PIT_BASE_HZ * 1e9 */
    return (UW)((elapsed * 1000UL) / (PIT_BASE_HZ / 1000000UL));
}

#endif /* _TKDEV_TIMER_ */
