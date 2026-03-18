/*
 *  cpu_init.c (x86)
 *  CPU-dependent initialization for x86 T-Kernel
 */

#include "kernel.h"
#include "cpu_insn.h"
#include <subsystem.h>
#include "memory.h"
IMPORT ER knl_init_Imalloc(void);

/* Interrupt vector table - indexed by vector number (0..255) */
FP knl_intvec[256];

/* Task-independent part counter */
W knl_taskindp = 0;

/* Kernel memory area (set by knl_cpu_initialize based on _kernel_end) */
EXPORT void *knl_lowmem_top;
EXPORT void *knl_lowmem_limit;

/* Linker symbol: end of kernel image (defined in linker.ld) */
extern char _kernel_end[];

/*
 * knl_cpu_initialize
 *   Called from knl_t_kernel_main() (tkstart.c) during kernel startup.
 *   On x86 without USE_TRAP, no SVC vectors need registering.
 *   The IDT is already set up by boot/x86/idt.c.
 */
EXPORT ER knl_cpu_initialize(void)
{
    /* Clear vector table */
    for (INT i = 0; i < 256; i++) {
        knl_intvec[i] = NULL;
    }

    /* Set up kernel memory area for T-Kernel imalloc
     * lowmem_top: just after kernel image, aligned to 4-byte boundary
     * lowmem_limit: SYSTEMAREA_END (64MB)
     */
    knl_lowmem_top   = (void *)(((UW)_kernel_end + 3) & ~3UL);
    knl_lowmem_limit = (void *)SYSTEMAREA_END;

    /* Initialize internal memory allocator */
    knl_init_Imalloc();

    return E_OK;
}

#if USE_CLEANUP
/*
 * knl_cpu_shutdown
 *   Called when the kernel shuts down.
 */
EXPORT void knl_cpu_shutdown(void)
{
    /* Mask all IRQs */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}
#endif /* USE_CLEANUP */

/*
 * knl_no_support - stub for unsupported T-Kernel system calls
 */
EXPORT INT knl_no_support(void *pk_para, FN fncd)
{
    (void)pk_para;
    (void)fncd;
    return E_NOSPT;
}
