/*
 *  arch/windows/x86_64/win_prim.c
 *
 *  Thin C wrappers over the Win32 primitives the μT-Kernel 3.0 Windows
 *  port needs: Fibers (cooperative context switch), Sleep, and
 *  QueryPerformanceCounter. This TU is the ONLY place windows.h is
 *  included on the kernel side — it keeps the Win32 type universe
 *  (BOOL/DWORD/LPVOID/…) away from the T-Kernel type universe
 *  (B/W/UW/BOOL/…), which otherwise collide. dispatch.c and the sysdep
 *  layer call these plain-C signatures instead.
 *
 *  Fibers, not hand-written asm: a Fiber owns its stack AND the full
 *  register set (including the Win64 non-volatile XMM6-XMM15), so
 *  SwitchToFiber is a correct, ABI-complete cooperative switch with no
 *  assembly to get wrong. This is the deliberate choice over porting the
 *  Linux dispatch.S to the Microsoft x64 ABI.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

/* ConvertThreadToFiber(NULL) — make the calling (primary) thread a fiber
 * so SwitchToFiber is legal. Returns the primary fiber handle. */
void *win_fiber_convert(void)
{
    return ConvertThreadToFiber(NULL);
}

/* CreateFiber — one fiber per task. dwStackSize is the initial commit;
 * the reserve defaults to the exe's default (≈1 MB) and grows, so even a
 * small task stksz is safe (unlike the Linux port's fixed-size stacks). */
void *win_fiber_create(unsigned long stksz, void (*entry)(void *), void *arg)
{
    return CreateFiber((SIZE_T)stksz, (LPFIBER_START_ROUTINE)entry, arg);
}

void win_fiber_switch(void *fiber)
{
    SwitchToFiber(fiber);
}

void win_fiber_delete(void *fiber)
{
    DeleteFiber(fiber);
}

void *win_fiber_current(void)
{
    return GetCurrentFiber();
}

void win_sleep_ms(unsigned int ms)
{
    Sleep((DWORD)ms);
}

/* Logical CPU count — replaces sysconf(_SC_NPROCESSORS_ONLN) for the LLM
 * tensor-parallel pool sizing (pk_parallel.c). */
int win_num_cpus(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (si.dwNumberOfProcessors > 0) ? (int)si.dwNumberOfProcessors : 1;
}

/* Monotonic wall-clock in nanoseconds from QueryPerformanceCounter.
 * Split the multiply to avoid overflow at large counter values. */
unsigned long long win_qpc_ns(void)
{
    static LARGE_INTEGER freq = { .QuadPart = 0 };
    LARGE_INTEGER now;
    unsigned long long q, r, f;

    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
        if (freq.QuadPart == 0) freq.QuadPart = 1;
    }
    QueryPerformanceCounter(&now);

    f = (unsigned long long)freq.QuadPart;
    q = (unsigned long long)now.QuadPart / f;
    r = (unsigned long long)now.QuadPart % f;
    return q * 1000000000ULL + (r * 1000000000ULL) / f;
}

/* Process CPU time (kernel + user) in nanoseconds from GetProcessTimes.
 * FILETIME is in 100 ns units. */
unsigned long long win_cpu_ns(void)
{
    FILETIME cre, ex, krn, usr;
    ULARGE_INTEGER k, u;

    if (!GetProcessTimes(GetCurrentProcess(), &cre, &ex, &krn, &usr))
        return win_qpc_ns();

    k.LowPart = krn.dwLowDateTime;  k.HighPart = krn.dwHighDateTime;
    u.LowPart = usr.dwLowDateTime;  u.HighPart = usr.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) * 100ULL;   /* 100 ns → ns */
}
