/*
 *  arch/linux/aarch64/inittask_main.c
 *  Initial task body — same as arch/aarch64.
 */

#include "kernel.h"
#include "inittask_def.h"
#include <tmonitor.h>

IMPORT INT usermain(void);

EXPORT void knl_init_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

#if USE_KERNEL_MESSAGE
    tm_putstring((UB *)"[T-Kernel] Initial task started\r\n");
#endif

    usermain();

#if USE_KERNEL_MESSAGE
    tm_putstring((UB *)"[T-Kernel] usermain() returned\r\n");
#endif

    tk_ext_tsk();
}
