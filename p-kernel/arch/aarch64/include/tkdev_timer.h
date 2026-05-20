/*
 *  tkdev_timer.h (aarch64)
 *  ARM Generic Timer interface for T-Kernel timer subsystem
 *
 *  The actual timer reload and GIC EOI are handled in tkdev_init.c.
 *  This header provides the inline stubs that timer.c / tkstart.c call.
 */

#ifndef _TKDEV_TIMER_
#define _TKDEV_TIMER_

#include <syslib.h>
#include <sysinfo.h>
#include "tkdev_conf.h"
#include "sysdef_depend.h"
#include "cpu_insn.h"

/* Settable interval range (millisecond) */
#define MIN_TIMER_PERIOD    1
#define MAX_TIMER_PERIOD    100

/* -----------------------------------------------------------------------
 *  ARM Generic Timer initialisation — also called from knl_tkdev_initialize
 * --------------------------------------------------------------------- */

Inline void knl_init_hw_timer(void)
{
    unsigned long freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long interval = freq / TIMER_HZ;
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(interval));
    __asm__ volatile ("msr cntp_ctl_el0,  %0" :: "r"((unsigned long)1));
    DSB();
    ISB();
}

Inline void knl_start_hw_timer(void)
{
    knl_init_hw_timer();
}

/* The GIC EOI is written in the IRQ vector handler (cpu_support.S).
 * Reload the countdown; the vector handler sends GICC_EOIR afterwards. */
Inline void knl_clear_hw_timer_interrupt(void)
{
    unsigned long freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long interval = freq / TIMER_HZ;
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(interval));
    DSB();
}

Inline void knl_end_of_hw_timer_interrupt(void)
{
    /* nothing — EOI handled in assembly IRQ vector */
}

Inline void knl_terminate_hw_timer(void)
{
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(0UL));
}

Inline UW knl_get_hw_timer_nsec(void)
{
    unsigned long tval;
    __asm__ volatile ("mrs %0, cntp_tval_el0" : "=r"(tval));
    unsigned long freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long period = freq / TIMER_HZ;
    unsigned long elapsed = (tval < period) ? (period - tval) : 0;
    return (UW)((elapsed * 1000000000UL) / freq);
}

#endif /* _TKDEV_TIMER_ */
