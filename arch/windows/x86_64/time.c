/*
 *  arch/windows/x86_64/time.c
 *
 *  Wall-clock and CPU-clock readers for the native Windows port.
 *  Public API and rationale: arch/linux/include/arch_time.h (shared).
 *
 *  Linux uses clock_gettime(CLOCK_MONOTONIC / CLOCK_PROCESS_CPUTIME_ID).
 *  Windows uses QueryPerformanceCounter (wall) and GetProcessTimes (cpu),
 *  both wrapped in win_prim.c to keep windows.h out of this TU.
 */

#include <stdint.h>

#include "arch_time.h"

/* win_prim.c */
extern unsigned long long win_qpc_ns(void);
extern unsigned long long win_cpu_ns(void);

uint64_t arch_get_time_ns(void)
{
    return (uint64_t)win_qpc_ns();
}

uint64_t arch_get_time_us(void)
{
    return (uint64_t)(win_qpc_ns() / 1000ULL);
}

uint64_t arch_get_time_ms(void)
{
    return (uint64_t)(win_qpc_ns() / 1000000ULL);
}

uint64_t arch_get_cpu_time_ns(void)
{
    return (uint64_t)win_cpu_ns();
}

uint64_t arch_get_cpu_time_us(void)
{
    return (uint64_t)(win_cpu_ns() / 1000ULL);
}
