/*
 *  arch/linux/aarch64/time.c
 *
 *  Wall-clock and CPU-clock readers for the Linux userspace port.
 *  See arch/linux/include/arch_time.h for the public API and the
 *  rationale (signal collapse, drift, debug mode).
 *
 *  All implementations are signal-safe — clock_gettime() with the
 *  vDSO fast path makes no system call, takes no locks, allocates
 *  nothing.
 */

#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdint.h>

#include "arch_time.h"

uint64_t arch_get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t arch_get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

uint64_t arch_get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

uint64_t arch_get_cpu_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t arch_get_cpu_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
