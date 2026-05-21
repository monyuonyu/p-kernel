/*
 *  arch/linux/aarch64/rtl8139.c
 *  Stub: no NIC driver inside a Linux process. The distributed layer
 *  is not linked into Session 3b's minimal boot, so these symbols
 *  exist purely to satisfy any stray IMPORTs.
 */

#include "kernel.h"

EXPORT INT  rtl8139_init(void)               { return -1; }
EXPORT void rtl8139_tx(const void *p, INT n) { (void)p; (void)n; }
EXPORT INT  rtl8139_rx(void *buf, INT max)   { (void)buf; (void)max; return 0; }

/* arch/common/netstack.c also calls these. Linux build has no NIC,
 * so they're no-ops returning failure / zero. */
EXPORT INT  rtl8139_send(const void *p, INT n) { (void)p; (void)n; return -1; }

EXPORT void rtl8139_get_mac(UB mac[6])
{
    /* Fabricate the QEMU-virt default so netstack sees the "single
     * node, no cluster" MAC and skips the distributed bringup. */
    mac[0] = 0x52; mac[1] = 0x54; mac[2] = 0x00;
    mac[3] = 0x12; mac[4] = 0x34; mac[5] = 0x56;
}

/* Weak hook called by net stack on packet arrival. Linux build does
 * not include a net stack, so this gets called from nowhere. */
__attribute__((weak)) void eth_input(const void *frame, INT size)
{
    (void)frame; (void)size;
}
