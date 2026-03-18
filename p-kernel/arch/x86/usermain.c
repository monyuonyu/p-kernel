/*
 *  usermain.c (x86)
 *  User application - runs inside the T-Kernel initial task
 */

#include "kernel.h"
#include <tmonitor.h>

/*
 * usermain - user application entry point
 *   Called from knl_init_task().
 *   Demonstrates T-Kernel task creation and timer wait.
 */
EXPORT INT usermain(void)
{
#if USE_KERNEL_MESSAGE
    tm_putstring((UB *)"\r\n=== p-kernel x86: T-Kernel running! ===\r\n");
    tm_putstring((UB *)"usermain() started.\r\n");
#endif

    /* Simple heartbeat loop using T-Kernel timer wait */
    for (INT i = 0; i < 5; i++) {
        tk_slp_tsk(1000);   /* sleep 1 second */
#if USE_KERNEL_MESSAGE
        tm_putstring((UB *)"[heartbeat]\r\n");
#endif
    }

#if USE_KERNEL_MESSAGE
    tm_putstring((UB *)"usermain() done.\r\n");
#endif

    return 0;
}
