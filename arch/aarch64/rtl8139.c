/*
 *  rtl8139.c (aarch64)
 *  RealTek RTL8139 NIC driver — MMIO variant for AArch64.
 *
 *  Differences from arch/x86/rtl8139.c:
 *    - Register access via BAR1 (MMIO) instead of BAR0 (I/O ports)
 *    - DMA buffer addresses are 32-bit truncations of 64-bit kernel
 *      pointers; QEMU virt places RAM at 0x40000000 and we use < 256 MB,
 *      so all buffers fit in the 32-bit window the chip can address.
 *    - GIC SPI IRQ via INTNO_RTL8139_GIC (QEMU virt slot-0 INTA = INTID 35).
 *      gic_enable_irq() unmasks; knl_define_inthdr() installs the handler;
 *      _vec_el1_irq dispatches into rtl_irq_handler() on every ROK/TOK.
 */

#include "rtl8139.h"
#include "pci.h"
#include "kernel.h"
#include "mmio.h"
#include "tkdev_conf.h"

IMPORT void gic_enable_irq(UINT intid);

/* ------------------------------------------------------------------ */
/* RTL8139 register offsets (chip register map is arch-independent)    */
/* ------------------------------------------------------------------ */

#define R_MAC       0x00
#define R_MAR       0x08
#define R_TSD0      0x10
#define R_TSAD0     0x20
#define R_RBSTART   0x30
#define R_CMD       0x37
#define R_CAPR      0x38
#define R_CBR       0x3A
#define R_IMR       0x3C
#define R_ISR       0x3E
#define R_TCR       0x40
#define R_RCR       0x44
#define R_CONFIG1   0x52

#define CMD_RST     0x10
#define CMD_RE      0x08
#define CMD_TE      0x04

#define ISR_ROK     0x0001
#define ISR_TOK     0x0004
#define ISR_RXOVW   0x0010

#define RCR_AB          (1u << 3)
#define RCR_APM         (1u << 1)
#define RCR_AM          (1u << 2)
#define RCR_WRAP        (1u << 7)
#define RCR_MXDMA_UNL  (7u << 8)
#define RCR_RBLEN_8K   (0u << 11)
#define RCR_RXFTH_NONE (7u << 13)

#define TCR_IFG_DEF     (3u << 24)
#define TCR_MXDMA_2048  (7u << 8)

/* PCI BAR1 = MMIO base (RTL8139 supports both I/O and memory access).
 * Bit 0 = 0 indicates memory space (vs I/O space). Mask off the low
 * four bits (memory space type + prefetch flag) to get the base. */
#define PCI_BAR1        0x14

/* ------------------------------------------------------------------ */
/* Buffers (static, identity-mapped: physical == virtual under QEMU)   */
/* ------------------------------------------------------------------ */

#define RX_BUF_LEN   (8192 + 16 + 1500)
#define TX_BUF_LEN   1536
#define TX_NUM       4

static UB rx_buf[RX_BUF_LEN]             __attribute__((aligned(4)));
static UB tx_buf[TX_NUM][TX_BUF_LEN]     __attribute__((aligned(4)));

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

static unsigned long rtl_mmio = 0;     /* BAR1 base, MMIO       */
unsigned long rtl_mmio_for_diag = 0;   /* exposed for shell `rx` cmd */
static UB  rtl_irq  = 0;               /* PCI INT_LINE (legacy) */
static UB  rtl_mac[6];
static INT tx_cur   = 0;
static INT rx_pos   = 0;
static ID  rx_sem   = 0;

volatile UW  rtl_rx_count   = 0;
volatile UW  rtl_tx_count   = 0;
volatile INT rtl_initialized = 0;

/* ------------------------------------------------------------------ */
/* MMIO register accessors                                            */
/* ------------------------------------------------------------------ */

static inline UB  rdb(UH off) { return mmio_read8 (rtl_mmio + off); }
static inline UH  rdw(UH off) { return mmio_read16(rtl_mmio + off); }
static inline UW  rdl(UH off) { return mmio_read32(rtl_mmio + off); }
static inline void wrb(UH off, UB v) { mmio_write8 (rtl_mmio + off, v); }
static inline void wrw(UH off, UH v) { mmio_write16(rtl_mmio + off, v); }
static inline void wrl(UH off, UW v) { mmio_write32(rtl_mmio + off, v); }

/* ------------------------------------------------------------------ */
/* Serial output helpers                                              */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void net_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void net_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { net_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    net_puts(&buf[i]);
}

static void net_puthex8(UB v)
{
    const char *h = "0123456789ABCDEF";
    char buf[3] = { h[v >> 4], h[v & 0xF], '\0' };
    net_puts(buf);
}

static void net_puthex16(UH v)
{
    net_puthex8((UB)(v >> 8));
    net_puthex8((UB)(v & 0xFF));
}

/* ------------------------------------------------------------------ */
/* IRQ handler — runs in interrupt context, dispatched from            */
/* _vec_el1_irq via knl_intvec[INTNO_RTL8139_GIC].                     */
/* ------------------------------------------------------------------ */

static void rtl_irq_handler(void)
{
    UH isr = rdw(R_ISR);
    wrw(R_ISR, isr);            /* ACK all bits (write-1-to-clear) */

    if (isr & ISR_ROK) {
        rtl_rx_count++;
        if (rx_sem > 0) {
            tk_sig_sem(rx_sem, 1);
        }
    }
    if (isr & ISR_TOK) {
        rtl_tx_count++;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void rtl8139_get_mac(UB mac[6])
{
    for (INT i = 0; i < 6; i++) mac[i] = rtl_mac[i];
}

ER rtl8139_send(const UB *data, UH len)
{
    if (!rtl_initialized) return E_NOEXS;
    if (len > TX_BUF_LEN)  return E_PAR;
    if (len < 60) len = 60;

    INT idx = tx_cur;

    for (UH i = 0; i < len; i++) tx_buf[idx][i] = data[i];
    for (UH i = len; i < 60; i++) tx_buf[idx][i] = 0;

    /* RTL8139 DMA is 32-bit. Cast 64-bit kernel pointer to 32-bit phys
     * address — safe while kernel RAM stays within the low 4 GB. */
    wrl((UH)(R_TSAD0 + idx * 4), (UW)(unsigned long)tx_buf[idx]);
    wrl((UH)(R_TSD0  + idx * 4), (UW)len);
    MMIO_DSB();

    tx_cur = (tx_cur + 1) % TX_NUM;
    return E_OK;
}

INT rtl8139_recv(UB *buf, INT maxlen)
{
    UH cbr = rdw(R_CBR);
    if ((UH)(rx_pos & 0xFFFF) == cbr) return 0;

    volatile UH *hdr = (volatile UH *)(rx_buf + rx_pos);
    UH status = hdr[0];
    UH pktlen = (UH)(hdr[1] - 4);

    if (!(status & 0x0001) || pktlen == 0 || pktlen > 1514) {
        rx_pos = (rx_pos + 4 + 3) & ~3;
        if (rx_pos >= 8192) rx_pos -= 8192;
        wrw(R_CAPR, (UH)((rx_pos - 16) & 0xFFFF));
        return 0;
    }

    if (pktlen > (UH)maxlen) pktlen = (UH)maxlen;

    const UB *src = (const UB *)(rx_buf + rx_pos + 4);
    for (UH i = 0; i < pktlen; i++) buf[i] = src[i];

    rx_pos = (rx_pos + 4 + hdr[1] + 3) & ~3;
    if (rx_pos >= 8192) rx_pos -= 8192;
    wrw(R_CAPR, (UH)((rx_pos - 16) & 0xFFFF));

    return (INT)pktlen;
}

/* ------------------------------------------------------------------ */
/* Net RX task — IRQ-driven.                                          */
/* The GIC SPI handler (rtl_irq_handler) signals rx_sem on each ROK;  */
/* the task wakes, drains every queued frame, then sleeps again.      */
/* ------------------------------------------------------------------ */

/* Weak stub: when arch/common/netstack.c is linked in (Phase 2c) its
 * strong definition of eth_input() replaces this default. */
__attribute__((weak)) void eth_input(const UB *frame, INT len)
{
    (void)frame; (void)len;
}

static UB pkt_buf[1514];

void net_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    net_puts("[net] RX task running (IRQ mode)\r\n");

    for (;;) {
        tk_wai_sem(rx_sem, 1, TMO_FEVR);

        INT len;
        while ((len = rtl8139_recv(pkt_buf, (INT)sizeof(pkt_buf))) > 0) {
            if (len < 14) continue;
            eth_input(pkt_buf, len);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

/* QEMU virt PCIe MMIO32 region: 0x10000000..0x3eff0000.
 * We pick the first 4 KB slot for the RTL8139 (it only needs 256 bytes,
 * but PCIe BAR alignment quantizes to the device's natural window). */
#define RTL_MMIO_BAR_ASSIGN  0x10000000UL

ER rtl8139_init(ID sem)
{
    UB bus, dev, func;

    net_puts("[net] Scanning PCIe ECAM for RTL8139...\r\n");

    if (!pci_find_device(PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8139,
                         &bus, &dev, &func)) {
        net_puts("[net] RTL8139 not found\r\n");
        return E_NOEXS;
    }

    /* QEMU virt PCIe routes legacy INTx pins to SPIs 3..6 using the
     * standard PCI swizzle. INT_PIN is 1..4 for INTA..INTD; without
     * UEFI/ACPI we just assume INTA (1) — the chip never wires anything
     * else. The hard-coded INTNO_RTL8139_GIC=35 only worked for dev=0;
     * QEMU virt actually lands the RTL8139 at dev=1, which rotates to
     * INTID 36. Compute it from (dev,int_pin) so this also stays right
     * if QEMU shuffles slots in a future release. */
    UB int_pin = pci_read8(bus, dev, func, 0x3D);
    if (int_pin == 0) int_pin = 1;
    UINT spi = 3 + ((UINT)(dev + int_pin - 1) % 4);
    UINT rtl_intid = 32 + spi;

    rx_sem = sem;

    /* BAR1 = memory-mapped registers. Lower 4 bits are flags.
     * On QEMU virt with no UEFI firmware, BARs are unassigned (read 0).
     * Assign our chosen MMIO address manually. */
    UW bar1 = pci_read32(bus, dev, func, PCI_BAR1);
    if ((bar1 & 0xFFFFFFF0u) == 0) {
        pci_write32(bus, dev, func, PCI_BAR1, (UW)RTL_MMIO_BAR_ASSIGN);
        bar1 = pci_read32(bus, dev, func, PCI_BAR1);
        net_puts("[net] assigned BAR1 manually\r\n");
    }
    rtl_mmio = (unsigned long)(bar1 & 0xFFFFFFF0u);
    rtl_mmio_for_diag = rtl_mmio;

    /* INT_LINE is firmware-routed; on QEMU virt without UEFI this is
     * undefined. We don't use it (poll mode). Record for diagnostics. */
    rtl_irq = pci_read8(bus, dev, func, PCI_INT_LINE);

    /* Enable memory space + bus mastering (memory, not I/O, for MMIO). */
    UH pcicmd = pci_read16(bus, dev, func, PCI_COMMAND);
    pci_write16(bus, dev, func, PCI_COMMAND,
                (UH)(pcicmd | 0x0002 /* MEM_SPACE */ | PCI_CMD_BUS_MASTER));

    /* Power on */
    wrb(R_CONFIG1, 0x00);

    /* Software reset */
    wrb(R_CMD, CMD_RST);
    {
        volatile INT t = 0;
        while ((rdb(R_CMD) & CMD_RST) && t < 100000) t++;
        if (t >= 100000) {
            net_puts("[net] RTL8139 reset timeout\r\n");
            return E_TMOUT;
        }
    }

    /* Read MAC */
    for (INT i = 0; i < 6; i++) rtl_mac[i] = rdb((UH)(R_MAC + i));

    /* RX ring */
    rx_pos = 0;
    wrl(R_RBSTART, (UW)(unsigned long)rx_buf);

    /* Interrupt mask — chip will assert INTA when ROK or TOK fires. */
    wrw(R_IMR, (UH)(ISR_ROK | ISR_TOK));

    /* Hook into _vec_el1_irq via knl_intvec[], then unmask in GICD. */
    knl_define_inthdr((INT)rtl_intid, (FP)rtl_irq_handler);
    gic_enable_irq(rtl_intid);

    /* Multicast: accept all */
    wrl(R_MAR,   0xFFFFFFFFu);
    wrl(R_MAR+4, 0xFFFFFFFFu);

    wrl(R_RCR, RCR_AB | RCR_APM | RCR_AM |
               RCR_MXDMA_UNL | RCR_RBLEN_8K | RCR_RXFTH_NONE | RCR_WRAP);
    wrl(R_TCR, TCR_IFG_DEF | TCR_MXDMA_2048);

    /* Enable RX + TX */
    wrb(R_CMD, (UB)(CMD_RE | CMD_TE));
    MMIO_DSB();

    rtl_initialized = 1;

    net_puts("[net] RTL8139 ready  MMIO=0x");
    net_puthex16((UH)(rtl_mmio >> 16));
    net_puthex16((UH)(rtl_mmio & 0xFFFF));
    net_puts("  PCI_INT_LINE=");
    net_putdec((UW)rtl_irq);
    net_puts("  MAC=");
    for (INT i = 0; i < 6; i++) {
        if (i) net_puts(":");
        net_puthex8(rtl_mac[i]);
    }
    net_puts("\r\n");

    return E_OK;
}
