/*
 *  arch/linux/aarch64/usermain.c
 *  Minimal usermain for Session 3b — just prove T-Kernel boots.
 *
 *  Prints a banner via tm_putstring, then sleeps in a do-nothing loop
 *  so the user can observe the boot succeeded. Session 4+ wires in
 *  the AI primitives, distributed layer, and real shell.
 */

#include "kernel.h"
#include <tmonitor.h>

IMPORT void sio_init(void);

EXPORT INT usermain(void)
{
    tm_putstring((UB *)" p-kernel  [linux / aarch64 userspace]\r\n");
    tm_putstring((UB *)"  (Session 3b — boot reached usermain. Hi from the great goal.)\r\n");
    tm_putstring((UB *)"  Press Ctrl-C in the host terminal to exit.\r\n");

    for (;;) {
        tk_dly_tsk(1000);   /* 1 second; the timer IRQ keeps this alive */
    }

    return 0;
}
