/*
 *  arch/linux/aarch64/inittask_def.c
 *  Initial task creation parameters — same as arch/aarch64.
 */

#include "inittask_def.h"

/* HOSTED-ONLY init-task stack override (wave-dmn-stack). The init task runs
 * usermain() -> the interactive shell, which dispatches the resident-baby
 * verbs (`baby`/`student`/`dmn distill`/`cradle test`/`ss6live`). Every one of
 * those reaches st_forward/st_backward over the ~30MB student, whose per-layer
 * float[DMAX=256]/float[DFFMAX=512] scratch overflows the shared 8KB
 * INITTASK_STKSZ (a plain `baby` SIGSEGVs mid-distill). 256KB matches the SS-6
 * responder / dmn_task precedent that runs the SAME st_forward. This override
 * is hosted-only: bare metal compiles arch/aarch64/inittask_def.c (untouched)
 * and links student_stub.o (no st_forward on the init task), so its crown
 * init-task stack stays 8KB. */
#undef  INITTASK_STKSZ
#define INITTASK_STKSZ (256*1024)

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
