/*
 *  12_ring3/02_core_crash/core_crash.c — ring3-core Wave B (first slice)
 *
 *  The "crash" half of the kernel-survives-core-crash proof
 *  (docs/architecture/50-evolution/ring3-core.md II.1b).
 *
 *  Does ONE successful SYS_INFER, then deliberately faults in ring-3
 *  with a null dereference.  Page 0 is mapped U/S=0 (kernel-only) in
 *  the per-process page tables (arch/x86/paging.c), so the ring-3
 *  write raises #PF (privilege violation) — the fault path the real
 *  core is most likely to hit.
 *
 *  Expected kernel behaviour (ring3-core.md I.3): exception_handler
 *  sees saved CS == USER_CS, prints
 *      [core] ring3 fault #14 @0x004000xx - task reaped
 *  reaps this task, and returns to the scheduler.  The kernel LIVES.
 *
 *  The trailing sys_exit(99) must NEVER execute — if it does, the
 *  fault did not happen and shell `ring3 test` FAILs honestly
 *  (reaped-counter delta would be 0).
 */

#include "plibc.h"

#define V0_T  30
#define V0_H  10
#define V0_P  5
#define V0_L  90

void _start(void)
{
    int cls = sys_infer(SYS_SENSOR_PACK(V0_T, V0_H, V0_P, V0_L));
    (void)cls;
    plib_puts("[core_crash] infer ok - now writing *(int*)0 from ring3\r\n");

    *(volatile int *)0 = 0;    /* #PF from ring3 */

    plib_puts("[core_crash] STILL ALIVE - fault did not fire\r\n");
    sys_exit(99);              /* NOT reached if the fault fired */
}
