/*
 *  inittask_main.c (x86)
 *  Initial task body and user main bridge
 */

#include "kernel.h"
#include "inittask_def.h"
#include <tmonitor.h>

IMPORT INT usermain(void);

/*
 * knl_init_task - initial task function
 *   Created and started by knl_t_kernel_main().
 *   Calls usermain() then terminates gracefully.
 */
EXPORT void knl_init_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

#if USE_KERNEL_MESSAGE
    tm_putstring((UB *)"[T-Kernel] Initial task started\n");
#endif

    /* Run user application */
    usermain();

    /* Should not reach here in normal operation */
#if USE_KERNEL_MESSAGE
    tm_putstring((UB *)"[T-Kernel] usermain() returned\n");
#endif

    tk_ext_tsk();
}
