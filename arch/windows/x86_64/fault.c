/*
 *  arch/windows/x86_64/fault.c
 *
 *  Minimal fault capture for the native Windows port (P1).
 *
 *  The Linux port (arch/linux/x86_64/fault.c) does task-level fault
 *  isolation: a SIGSEGV/SIGBUS/SIGFPE in a guarded task is redirected to
 *  guard_task_killer, reaping just that task. Doing the equivalent on
 *  Windows needs a Vectored Exception Handler that rewrites the faulting
 *  fiber's CONTEXT (Rip/Rsp/Rbp) — that is P2 work.
 *
 *  For P1 (console boot) this is an honest stub: install an unhandled
 *  exception filter that prints the fault and exits. No per-task
 *  isolation — a fault takes the whole node down (loudly), it is not
 *  silently swallowed.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>

static LONG WINAPI win_fault_filter(EXCEPTION_POINTERS *ep)
{
    unsigned long code =
        (unsigned long)ep->ExceptionRecord->ExceptionCode;
    void *addr = (void *)ep->ExceptionRecord->ExceptionAddress;

    fprintf(stderr,
            "\n[win] FATAL exception 0x%08lx at %p — node down "
            "(no task-level isolation in v1)\n",
            code, addr);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;   /* let the process terminate */
}

/* Called from kernel/mtkernel3/.../windows_x86_64/interrupt.c
 * (knl_init_interrupt). */
void arch_fault_init(void)
{
    SetUnhandledExceptionFilter(win_fault_filter);
}
