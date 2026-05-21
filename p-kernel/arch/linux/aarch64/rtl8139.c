/*
 *  arch/linux/aarch64/rtl8139.c
 *  Stub: no NIC driver inside a Linux process. The distributed layer
 *  is not linked into Session 3b's minimal boot, so these symbols
 *  exist purely to satisfy any stray IMPORTs.
 */

#include "kernel.h"

EXPORT INT  rtl8139_init(void)            { return -1; }
EXPORT void rtl8139_tx(const void *p, INT n) { (void)p; (void)n; }
EXPORT INT  rtl8139_rx(void *buf, INT max)   { (void)buf; (void)max; return 0; }

/* Weak hook called by net stack on packet arrival. Linux build does
 * not include a net stack, so this gets called from nowhere. */
__attribute__((weak)) void eth_input(const void *frame, INT size)
{
    (void)frame; (void)size;
}
