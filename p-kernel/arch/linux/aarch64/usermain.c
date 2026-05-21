/*
 *  arch/linux/aarch64/usermain.c
 *
 *  Minimal-but-real usermain for the UMP build. Brings up the AI
 *  primitives (tensor pool, ai_job queue, pipeline) and the data
 *  layers that don't need a NIC (K-DDS in single-node mode, DTR
 *  Transformer). Skips the distributed bringup — netstack expects an
 *  rtl8139, and arch/linux only stubs that out, so SWIM / Raft /
 *  DRPC stay dormant on this build.
 *
 *  Once the Phase B Android port adds a real network backend, this
 *  file converges with arch/aarch64/usermain.c.
 */

#include "kernel.h"
#include <tmonitor.h>

IMPORT void ai_kernel_init(void);
IMPORT void kdds_init(void);
IMPORT void dtr_init(void);

EXPORT INT usermain(void)
{
    tm_putstring((UB *)"\r\n p-kernel  [linux / aarch64 userspace]\r\n");
    tm_putstring((UB *)"\r\n");

    /* AI Body layer — tensor pool, ai_job queue, pipeline, MLP. */
    ai_kernel_init();

    /* K-DDS — pub/sub. Single-node mode without a NIC. */
    kdds_init();

    /* DTR — distributed Transformer (the AI brain layer). */
    dtr_init();

    tm_putstring((UB *)"\r\n  T-Kernel is alive inside a Linux process.\r\n");
    tm_putstring((UB *)"  (single-node mode; network bringup pending Phase B.)\r\n");
    tm_putstring((UB *)"  Press Ctrl-C in the host terminal to exit.\r\n");

    for (;;) {
        tk_dly_tsk(1000);
    }
    return 0;
}
