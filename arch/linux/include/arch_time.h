/* arch/linux/include/arch_time.h
 *
 * Wall-clock and CPU-clock readers for the Linux userspace port.
 *
 * These exist because the SIGALRM-driven tick counter (knl_current_time)
 * can drift behind wall-clock when Linux withholds CPU from us — POSIX
 * standard signals collapse, so N missed expirations of the 10 ms timer
 * arrive as a single SIGALRM when we next run. The preempt.c handler
 * uses timer_getoverrun() to replay missed ticks, which keeps the tick
 * count honest at signal-delivery moments; for queries between ticks,
 * callers wanting sub-tick precision should read arch_get_time_ns()
 * directly from CLOCK_MONOTONIC.
 *
 * CPU-time variants are provided for debugger-friendly modes — they
 * stop advancing while the process is stopped (gdb break, SIGSTOP),
 * which avoids the "100 timer expirations all fire at once on resume"
 * surprise.
 *
 * All readers are signal-safe (use clock_gettime, no malloc, no locks).
 */
#ifndef _PKERNEL_ARCH_LINUX_ARCH_TIME_H
#define _PKERNEL_ARCH_LINUX_ARCH_TIME_H

#include <stdint.h>

/* Monotonic wall-clock readers — track real time elapsed since boot. */
extern uint64_t arch_get_time_ns(void);
extern uint64_t arch_get_time_us(void);
extern uint64_t arch_get_time_ms(void);

/* Process-CPU readers — track only time we actually ran. Useful for
 * deterministic replay and for measuring kernel work without external
 * interference. */
extern uint64_t arch_get_cpu_time_ns(void);
extern uint64_t arch_get_cpu_time_us(void);

#endif /* _PKERNEL_ARCH_LINUX_ARCH_TIME_H */
