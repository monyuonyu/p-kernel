/*
 *  arch/linux/aarch64/rtl8139.c
 *
 *  T-Kernel-side shim implementing the rtl8139.h surface in terms of
 *  AF_UNIX sockets (see net_unix.c). From the netstack's perspective
 *  this driver behaves like a real NIC — accepts frames, delivers
 *  received ones via eth_input, exposes a MAC.
 *
 *  No interrupts. The companion net_task polls rtl8139_recv at
 *  ~100 Hz via tk_dly_tsk(1); SIGALRM keeps the poll cadence in step
 *  with wall clock.
 */

#include "kernel.h"
#include "rtl8139.h"

/* Forward declarations of the POSIX-side backend (in net_unix.c).
 * Types are plain ABI-compatible C, no T-Kernel headers needed. */
extern int arch_linux_net_init(void);
extern int arch_linux_net_send(const void *frame, int len);
extern int arch_linux_net_recv(void *buf, int maxlen);
extern int arch_linux_net_node_id(void);

EXPORT volatile UW  rtl_rx_count   = 0;
EXPORT volatile UW  rtl_tx_count   = 0;
EXPORT volatile INT rtl_initialized = 0;

static ID  rtl_rx_sem = -1;        /* unused on Linux; kept for ABI */
static INT my_node_id = 1;

EXPORT ER rtl8139_init(ID rx_sem)
{
    INT n = arch_linux_net_init();
    if (n < 0) return E_NOEXS;
    my_node_id      = n;
    rtl_rx_sem      = rx_sem;
    rtl_initialized = 1;
    return E_OK;
}

EXPORT ER rtl8139_send(const UB *data, UH len)
{
    if (!rtl_initialized) return E_NOEXS;
    if (arch_linux_net_send(data, (int)len) < 0) return E_IO;
    rtl_tx_count++;
    return E_OK;
}

EXPORT INT rtl8139_recv(UB *buf, INT maxlen)
{
    if (!rtl_initialized) return 0;
    INT n = arch_linux_net_recv(buf, maxlen);
    if (n > 0) rtl_rx_count++;
    return n;
}

EXPORT void rtl8139_get_mac(UB mac[6])
{
    /* Cluster convention: 52:54:00:00:00:0N → distributed node N.
     * netstack interprets this and joins the swarm in DRPC/SWIM mode.
     * If init never happened we fall through to a known-good single-
     * node MAC. */
    mac[0] = 0x52; mac[1] = 0x54; mac[2] = 0x00;
    mac[3] = 0x00; mac[4] = 0x00;
    mac[5] = rtl_initialized ? (UB)my_node_id : (UB)0x56;
}

/* Weak default — overridden by arch/common/netstack.c's strong def. */
__attribute__((weak)) void eth_input(const UB *frame, INT len)
{
    (void)frame; (void)len;
}

static UB rtl_rx_pkt[1514];

EXPORT void net_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for (;;) {
        INT len = rtl8139_recv(rtl_rx_pkt, (INT)sizeof(rtl_rx_pkt));
        if (len > 14) {
            eth_input(rtl_rx_pkt, len);
        } else {
            /* No frame waiting; yield until the next SIGALRM-driven
             * tick. 10 ms cadence matches arch_timer_start. */
            tk_dly_tsk(10);
        }
    }
}
