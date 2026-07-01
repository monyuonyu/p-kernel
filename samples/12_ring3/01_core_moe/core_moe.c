/*
 *  12_ring3/01_core_moe/core_moe.c — ring3-core Wave B (first slice)
 *
 *  The "mind" half of the kernel-survives-core-crash proof
 *  (docs/architecture/50-evolution/ring3-core.md II.1a).
 *
 *  Runs as a ring-3 user task, issues SYS_INFER(0x210) with the FIXED
 *  test vector V0, prints the returned class, and exits with the class
 *  as its SYS_EXIT code.
 *
 *  The exit code IS the gate-1 channel: the kernel records the last
 *  SYS_EXIT code (arch/x86/syscall.c user_last_exit_code()), so the
 *  class makes the full round trip
 *      kernel moe_infer → ring3 EAX → this code → SYS_EXIT(class) →
 *      kernel → shell `ring3 test`
 *  and the shell verb compares it against a live ring-0 moe_infer(V0)
 *  oracle.  It cannot be greened by a hard-coded constant.
 *
 *  V0 MUST match arch/x86/shell.c (R3_V0_T/H/P/L).
 */

#include "plibc.h"

#define V0_T  30
#define V0_H  10
#define V0_P  5
#define V0_L  90

void _start(void)
{
    int cls = sys_infer(SYS_SENSOR_PACK(V0_T, V0_H, V0_P, V0_L));

    plib_puts("[core_moe] ring3 SYS_INFER class=");
    if (cls >= 0 && cls <= 9) {
        char d[2]; d[0] = (char)('0' + cls); d[1] = '\0';
        plib_puts(d);
    } else {
        plib_puts("ERR");
    }
    plib_puts("\r\n");

    if (cls < 0) sys_exit(-1);
    sys_exit(cls);   /* gate-1 channel: exit code = class */
}
