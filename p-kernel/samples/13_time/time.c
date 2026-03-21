/*
 *  16_time/time.c — T-Kernel time API demo
 *
 *  Tests:
 *    tk_get_tim()   — read system time (ms since boot)
 *    tk_dly_tsk()   — delay for specified milliseconds
 */
#include "plibc.h"

void _start(void)
{
    int ok = 1;

    /* --- Test 1: tk_get_tim() returns a valid time --------------- */
    PK_SYSTIM t0, t1;
    int r = tk_get_tim(&t0);
    if (r != 0) {
        plib_puts("[16] tk_get_tim error: ");
        plib_puti(r);
        plib_puts("\r\n");
        ok = 0;
    } else {
        plib_puts("[16] tk_get_tim OK: lo=");
        plib_putu(t0.lo);
        plib_puts(" ms\r\n");
    }

    /* --- Test 2: tk_dly_tsk() delays for ~200 ms ----------------- */
    tk_dly_tsk(200);
    tk_get_tim(&t1);

    /* elapsed = t1.lo - t0.lo (ms, ignoring hi for short durations) */
    unsigned int elapsed = t1.lo - t0.lo;

    /* Accept 150..350 ms range (QEMU timer jitter) */
    if (elapsed >= 150 && elapsed <= 350) {
        plib_puts("[16] tk_dly_tsk OK: elapsed=");
        plib_putu(elapsed);
        plib_puts(" ms\r\n");
    } else {
        plib_puts("[16] tk_dly_tsk elapsed=");
        plib_putu(elapsed);
        plib_puts(" ms (expected ~200) => FAIL\r\n");
        ok = 0;
    }

    /* --- Test 3: two consecutive reads advance time -------------- */
    PK_SYSTIM ta, tb;
    tk_get_tim(&ta);
    tk_dly_tsk(50);
    tk_get_tim(&tb);
    if (tb.lo > ta.lo) {
        plib_puts("[16] time advances OK\r\n");
    } else {
        plib_puts("[16] time advances FAIL\r\n");
        ok = 0;
    }

    plib_puts(ok ? "[16] time => OK\r\n" : "[16] time => FAIL\r\n");
    sys_exit(0);
}
