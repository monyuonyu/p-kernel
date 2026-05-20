/*
 *  cpu_init.c (aarch64)
 *  CPU-dependent initialization for AArch64 T-Kernel
 */

#include "kernel.h"
#include "cpu_insn.h"
#include <subsystem.h>
#include "memory.h"

IMPORT ER knl_init_Imalloc(void);

/* Software interrupt vector table */
FP knl_intvec[N_INTVEC];

/* Task-independent part counter */
W knl_taskindp = 0;

/* Kernel memory area — set at startup */
EXPORT void *knl_lowmem_top;
EXPORT void *knl_lowmem_limit;

/* Linker-defined end of kernel image */
extern char _kernel_end[];

EXPORT ER knl_cpu_initialize(void)
{
    for (INT i = 0; i < N_INTVEC; i++) {
        knl_intvec[i] = NULL;
    }

    knl_lowmem_top   = (void *)(((unsigned long)_kernel_end + 7) & ~7UL);
    knl_lowmem_limit = (void *)SYSTEMAREA_END;

    knl_init_Imalloc();

    return E_OK;
}

#if USE_CLEANUP
EXPORT void knl_cpu_shutdown(void)
{
    __asm__ volatile ("msr daifset, #0xF");   /* mask all exceptions */
    for (;;) {
        __asm__ volatile ("wfe");
    }
}
#endif

EXPORT INT knl_no_support(void *pk_para, FN fncd)
{
    (void)pk_para;
    (void)fncd;
    return E_NOSPT;
}
