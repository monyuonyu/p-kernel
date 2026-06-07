/*
 *  arch/linux/aarch64/include/tkdev_timer.h
 *  Hardware-timer hooks for the Linux userspace port.
 *
 *  Shadows arch/aarch64/include/tkdev_timer.h. The bare-metal sibling
 *  pokes EL1-controlled timer registers (cntp_tval_el0, cntp_ctl_el0)
 *  via MSR — those instructions raise SIGILL when executed from EL0
 *  in a Linux process. Here they all collapse into no-ops; the actual
 *  periodic tick is delivered by the SIGALRM timer armed in
 *  arch/linux/aarch64/preempt.c (arch_timer_start), which runs from
 *  knl_tkdev_initialize before T-Kernel reaches knl_timer_initialize.
 */

#ifndef _TKDEV_TIMER_
#define _TKDEV_TIMER_

#include <syslib.h>
#include <sysinfo.h>
#include "tkdev_conf.h"
#include "sysdef_depend.h"
#include "cpu_insn.h"

#define MIN_TIMER_PERIOD    1
#define MAX_TIMER_PERIOD    100

Inline void knl_init_hw_timer(void)               { }
Inline void knl_start_hw_timer(void)              { }
Inline void knl_clear_hw_timer_interrupt(void)    { }
Inline void knl_end_of_hw_timer_interrupt(void)   { }
Inline void knl_terminate_hw_timer(void)          { }
Inline UW   knl_get_hw_timer_nsec(void)           { return 0; }

#endif /* _TKDEV_TIMER_ */
