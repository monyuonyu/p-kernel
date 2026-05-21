/*
 *  arch/linux/x86_64/cpu_init.c
 *  Sibling of arch/linux/aarch64/cpu_init.c. knl_cpu_shutdown just
 *  exits the hosted Linux process — no privileged-instruction worry
 *  on either ABI.
 */

#include "kernel.h"
#include "cpu_insn.h"
#include <subsystem.h>
#include "memory.h"

/* Avoid pulling in <stdlib.h> in the same TU as T-Kernel headers —
 * system stdlib.h drags wchar_t/va_list/int64_t into the compilation
 * unit and they collide with the T-Kernel placeholders. */
extern void exit(int) __attribute__((noreturn));

IMPORT ER knl_init_Imalloc(void);

FP knl_intvec[N_INTVEC];

W  knl_taskindp = 0;

EXPORT void *knl_lowmem_top;
EXPORT void *knl_lowmem_limit;

/* The kernel "low memory" area. On Linux we just allocate one big block
 * up front; tasks/heap/etc. carve from this. 16 MB is plenty for the
 * boot banner + a handful of tasks. */
static unsigned char linux_heap[16 * 1024 * 1024] __attribute__((aligned(16)));

/* _kernel_end is normally a linker-defined symbol marking the top of
 * the loaded kernel image (used by kloader_task as a "do not overwrite
 * below here" sentinel). Linux processes have no equivalent. Expose a
 * zero-size symbol that resolves to the bottom of our heap so the
 * kloader bounds check is a stable address. */
__asm__ (".global _kernel_end\n_kernel_end:\n");

EXPORT ER knl_cpu_initialize(void)
{
    for (INT i = 0; i < N_INTVEC; i++) {
        knl_intvec[i] = NULL;
    }

    knl_lowmem_top   = (void *)linux_heap;
    knl_lowmem_limit = (void *)(linux_heap + sizeof(linux_heap));

    knl_init_Imalloc();
    return E_OK;
}

#if USE_CLEANUP
EXPORT void knl_cpu_shutdown(void)
{
    exit(0);
}
#endif

EXPORT INT knl_no_support(void *pk_para, FN fncd)
{
    (void)pk_para;
    (void)fncd;
    return E_NOSPT;
}
