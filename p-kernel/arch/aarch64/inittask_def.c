/*
 *  inittask_def.c (aarch64)
 *  Initial task creation parameters
 */

#include "inittask_def.h"

IMPORT void knl_init_task(INT stacd, void *exinf);

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
