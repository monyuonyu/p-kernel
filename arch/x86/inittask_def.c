/*
 *  inittask_def.c (x86)
 *  Initial task creation parameters
 */

#include "inittask_def.h"

/* Forward declaration of initial task function */
IMPORT void knl_init_task(INT stacd, void *exinf);

/*
 * Initial task creation parameters
 * Referenced by the startup code that calls knl_t_kernel_main().
 */
EXPORT const T_CTSK knl_c_init_task = {
    (void *)INITTASK_EXINF,
    INITTASK_TSKATR,
    (FP)knl_init_task,
    INITTASK_ITSKPRI,
    INITTASK_STKSZ,
#if USE_OBJECT_NAME
    INITTASK_DSNAME,
#endif
    INITTASK_STACK,
};
