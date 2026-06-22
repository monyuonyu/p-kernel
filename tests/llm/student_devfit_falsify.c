/*
 *  student_devfit_falsify.c — the load-bearing control for [device-fit].
 *
 *  Driven by run_devfit.sh under a constrained address space (ulimit -v) so the
 *  L arena (~314MB) CANNOT allocate while S/M can. Calls st_init_device on the
 *  injected 512MB profile and prints the outcome.
 *
 *  Two builds, ONE source:
 *    - PRODUCTION (no -D): st_init_device probes -> 512MB profile -> tier S,
 *      arena ~2.5MB allocates -> prints "OK tier=S". (Even if RAM lied high, the
 *      L->M->S step-down would land on a fitting tier.)
 *    - FALSIFIER (-DDEVFIT_IGNORE_MEASURE): st_init_device hardcodes ST_TIER_L
 *      and DISABLES the step-down -> tries the ~314MB L arena under the cap ->
 *      malloc fails -> ST_E_OOM -> prints "OOM".
 *
 *  The script asserts: production prints OK (a small device gets a FITTING
 *  mind), the falsifier prints OOM (ignore the measurement and a small device
 *  fails to bring up its student). That is the proof the measurement is load-
 *  bearing — the same discipline as SS-4's -DSS4_GROW_NAIVE.
 *
 *  Exit code: 0 if st_init_device returned ST_OK, 1 otherwise (so the script
 *  can branch on it directly).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include "../../arch/common/llm/student.h"
#include "../../arch/common/llm/dev_capacity.h"

static const char *TN[ST_NTIER] = { "S", "M", "L" };

int main(void)
{
    st_model m;
    int rc = st_init_device(&m, 0xC0FFEEu);
    if (rc == ST_OK) {
        printf("OK tier=%s (arena fits)\n", TN[m.tier]);
        st_free(&m);
        return 0;
    }
    printf("OOM rc=%d (could not bring up the student)\n", rc);
    return 1;
}
