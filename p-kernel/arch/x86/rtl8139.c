/*
 *  rtl8139.c (x86)
 *  RealTek RTL8139 NIC driver for p-kernel
 *
 *  Hardware model:
 *    QEMU: -device rtl8139,netdev=net0 -netdev user,id=net0
 *
 *  RX: linear ring buffer (8 KB + guard bytes)
 *  TX: 4 static descriptors, round-robin
 */

#include "rtl8139.h"
#include "pci.h"
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* RTL8139 register offsets                                            */
/* ------------------------------------------------------------------ */

#define R_MAC       0x00    /* MAC address (6 bytes)                   */
#define R_MAR       0x08    /* Multicast filter (8 bytes)              */
#define R_TSD0      0x10    /* TX Status descriptor 0-3 (4 x 4 bytes)  */
#define R_TSAD0     0x20    /* TX Start Address 0-3 (4 x 4 bytes)      */
#define R_RBSTART   0x30    /* RX Buffer Start Address (4 bytes)        */
#define R_CMD       0x37    /* Command (1 byte)                         */
#define R_CAPR      0x38    /* Current Address of Packet Read (2 bytes) */
#define R_CBR       0x3A    /* Current Buffer Address (2 bytes)         */
#define R_IMR       0x3C    /* Interrupt Mask Register (2 bytes)        */
#define R_ISR       0x3E    /* Interrupt Status Register (2 bytes)      */
#define R_TCR       0x40    /* TX Configuration (4 bytes)               */
#define R_RCR       0x44    /* RX Configuration (4 bytes)               */
#define R_CONFIG1   0x52    /* Configuration 1 (1 byte)                 */

/* CMD bits */
#define CMD_RST     0x10
#define CMD_RE      0x08
#define CMD_TE      0x04

/* ISR / IMR bits */
#define ISR_ROK     0x0001  /* RX OK             */
#define ISR_TOK     0x0004  /* TX OK             */
#define ISR_RXOVW   0x0010  /* RX buffer overflow */

/* RX Configuration */
#define RCR_AB          (1u << 3)   /* Accept Broadcast          */
#define RCR_APM         (1u << 1)   /* Accept Physical Match     */
#define RCR_AM          (1u << 2)   /* Accept Multicast          */
#define RCR_WRAP        (1u << 7)   /* Wrap: keep in one buffer  */
#define RCR_MXDMA_UNL  (7u << 8)   /* Unlimited DMA burst       */
#define RCR_RBLEN_8K   (0u << 11)  /* 8 KB RX buffer            */
#define RCR_RXFTH_NONE (7u << 13)  /* No FIFO threshold         */

/* TX Configuration */
#define TCR_IFG_DEF     (3u << 24)  /* Standard interframe gap   */
#define TCR_MXDMA_2048  (7u << 8)   /* Max DMA burst 2048 bytes  */

/* TX Status bits */
#define TSD_TOK         (1u << 15)
#define TSD_TABT        (1u << 30)

/* ------------------------------------------------------------------ */
/* Buffers (static, identity-mapped: physical addr == virtual addr)   */
/* ------------------------------------------------------------------ */

#define RX_BUF_LEN   (8192 + 16 + 1500)    /* 8 KB ring + guard       */
#define TX_BUF_LEN   1536                   /* max Ethernet frame      */
#define TX_NUM       4                      /* descriptor count        */

static UB rx_buf[RX_BUF_LEN]             __attribute__((aligned(4)));
static UB tx_buf[TX_NUM][TX_BUF_LEN]     __attribute__((aligned(4)));

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

static UH  rtl_io   = 0;       /* I/O base from BAR0    */
static UB  rtl_irq  = 0;       /* PCI interrupt line    */
static UB  rtl_mac[6];
static INT tx_cur   = 0;       /* current TX descriptor */
static INT rx_pos   = 0;       /* current RX read head  */
static ID  rx_sem   = 0;       /* T-Kernel semaphore    */

volatile UW  rtl_rx_count   = 0;
volatile UW  rtl_tx_count   = 0;
volatile INT rtl_initialized = 0;

/* ------------------------------------------------------------------ */
/* I/O helpers                                                         */
/* ------------------------------------------------------------------ */

static inline UB rdb(UH off)
{
    UB v;
    UH p = (UH)(rtl_io + off);
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline UH rdw(UH off)
{
    UH v;
    UH p = (UH)(rtl_io + off);
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline UW rdl(UH off)
{
    UW v;
    UH p = (UH)(rtl_io + off);
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void wrb(UH off, UB v)
{
    UH p = (UH)(rtl_io + off);
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p));
}
static inline void wrw(UH off, UH v)
{
    UH p = (UH)(rtl_io + off);
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(p));
}
static inline void wrl(UH off, UW v)
{
    UH p = (UH)(rtl_io + off);
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(p));
}

/* PIC I/O (for unmasking the IRQ line) */
static inline UB pic_inb(UH port)
{
    UB v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void pic_outb(UH port, UB v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* ------------------------------------------------------------------ */
/* Serial output helpers (for IRQ-context safe logging)               */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void net_puts(const char *s)
{
    INT n = 0;
    while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void net_putdec(UW v)
{
    char buf[12];
    INT i = 11;
    buf[i] = '\0';
    if (v == 0) { net_puts("0"); return; }
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
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
/* IRQ dispatch table (defined in idt.c)                              */
/* ------------------------------------------------------------------ */

IMPORT void (*x86_irq_handlers[16])(void);

/* IRQ handler — called in interrupt context */
static void rtl_irq_handler(void)
{
    UH isr = rdw(R_ISR);
    wrw(R_ISR, isr);            /* ACK all bits */

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
    if (len < 60) len = 60;     /* Ethernet minimum frame */

    INT idx = tx_cur;

    /* Copy to aligned TX buffer */
    for (UH i = 0; i < len; i++) {
        tx_buf[idx][i] = data[i];
    }
    /* Zero-pad to minimum */
    for (UH i = len; i < 60; i++) {
        tx_buf[idx][i] = 0;
    }

    /* Load address then kick off DMA by writing size to TSD */
    wrl((UH)(R_TSAD0 + idx * 4), (UW)tx_buf[idx]);
    wrl((UH)(R_TSD0  + idx * 4), (UW)len);

    tx_cur = (tx_cur + 1) % TX_NUM;
    return E_OK;
}

INT rtl8139_recv(UB *buf, INT maxlen)
{
    /* Compare CAPR and CBR — if equal, nothing new */
    UH cbr = rdw(R_CBR);
    if ((UH)(rx_pos & 0xFFFF) == cbr) return 0;

    /* Packet header: 2-byte status + 2-byte length (includes 4-byte CRC) */
    volatile UH *hdr = (volatile UH *)(rx_buf + rx_pos);
    UH status = hdr[0];
    UH pktlen = (UH)(hdr[1] - 4);      /* strip CRC */

    /* Check ROK bit in status */
    if (!(status & 0x0001) || pktlen == 0 || pktlen > 1514) {
        /* Bad packet — skip 4-byte header and advance */
        rx_pos = (rx_pos + 4 + 3) & ~3;
        if (rx_pos >= 8192) rx_pos -= 8192;
        wrw(R_CAPR, (UH)((rx_pos - 16) & 0xFFFF));
        return 0;
    }

    if (pktlen > (UH)maxlen) pktlen = (UH)maxlen;

    /* Copy payload */
    const UB *src = (const UB *)(rx_buf + rx_pos + 4);
    for (UH i = 0; i < pktlen; i++) {
        buf[i] = src[i];
    }

    /* Advance read pointer: header(4) + length + CRC(4), DWORD aligned */
    rx_pos = (rx_pos + 4 + hdr[1] + 3) & ~3;
    if (rx_pos >= 8192) rx_pos -= 8192;
    wrw(R_CAPR, (UH)((rx_pos - 16) & 0xFFFF));

    return (INT)pktlen;
}

/* ------------------------------------------------------------------ */
/* Net RX task                                                         */
/* ------------------------------------------------------------------ */

IMPORT void eth_input(const UB *frame, INT len);
IMPORT void netstack_start(void);

static UB pkt_buf[1514];

void net_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    net_puts("[net] RX task running\r\n");

    for (;;) {
        /* Block until IRQ handler signals packet received */
        tk_wai_sem(rx_sem, 1, TMO_FEVR);

        /* Drain all packets in the ring buffer */
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

ER rtl8139_init(ID sem)
{
    UB bus, dev, func;

    net_puts("[net] Scanning PCI bus for RTL8139...\r\n");

    if (!pci_find_device(PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8139,
                         &bus, &dev, &func)) {
        net_puts("[net] RTL8139 not found\r\n");
        return E_NOEXS;
    }

    rx_sem = sem;

    /* BAR0: I/O base (bit 0 = I/O space indicator, mask it out) */
    UW bar0 = pci_read32(bus, dev, func, PCI_BAR0);
    rtl_io  = (UH)(bar0 & 0xFFFC);

    /* Interrupt line */
    rtl_irq = pci_read8(bus, dev, func, PCI_INT_LINE);

    /* Enable I/O space + bus mastering */
    UH pcicmd = pci_read16(bus, dev, func, PCI_COMMAND);
    pci_write16(bus, dev, func, PCI_COMMAND,
                (UH)(pcicmd | PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER));

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

    /* Read MAC address */
    for (INT i = 0; i < 6; i++) {
        rtl_mac[i] = rdb((UH)(R_MAC + i));
    }

    /* Set RX ring buffer start address */
    rx_pos = 0;
    wrl(R_RBSTART, (UW)rx_buf);

    /* Interrupt mask: RX OK + TX OK */
    wrw(R_IMR, (UH)(ISR_ROK | ISR_TOK));

    /* RX config: broadcast + physical match, 8 KB, no FIFO threshold, WRAP */
    wrl(R_RCR, RCR_AB | RCR_APM | RCR_AM |
               RCR_MXDMA_UNL | RCR_RBLEN_8K | RCR_RXFTH_NONE | RCR_WRAP);

    /* TX config */
    wrl(R_TCR, TCR_IFG_DEF | TCR_MXDMA_2048);

    /* Enable RX + TX */
    wrb(R_CMD, (UB)(CMD_RE | CMD_TE));

    /* Register IRQ handler */
    if (rtl_irq < 16) {
        x86_irq_handlers[rtl_irq] = rtl_irq_handler;

        /* PCI uses level-triggered interrupts.
         * Configure ELCR (Edge/Level Control Register) for this IRQ.
         *   0x4D0 = master PIC ELCR (IRQ 0-7)
         *   0x4D1 = slave  PIC ELCR (IRQ 8-15)
         * Bit N set = level triggered.  IRQs 0,1,2,8,13 must stay edge. */
        if (rtl_irq >= 3) {
            UH elcr_port = (rtl_irq < 8) ? 0x4D0 : 0x4D1;
            UB bit       = (UB)(1 << (rtl_irq & 7));
            UB elcr      = pic_inb(elcr_port);
            pic_outb(elcr_port, (UB)(elcr | bit));
        }

        /* Unmask IRQ on PIC (and IRQ2 cascade line for slave IRQs) */
        if (rtl_irq < 8) {
            UB mask = pic_inb(0x21);
            pic_outb(0x21, (UB)(mask & ~(1 << rtl_irq)));
        } else {
            /* Slave IRQ: unmask target IRQ on slave PIC */
            UB smask = pic_inb(0xA1);
            pic_outb(0xA1, (UB)(smask & ~(1 << (rtl_irq - 8))));
            /* Also unmask IRQ2 (cascade) on master PIC */
            UB mmask = pic_inb(0x21);
            pic_outb(0x21, (UB)(mmask & ~0x04));
        }
    }

    rtl_initialized = 1;

    /* Print result */
    net_puts("[net] RTL8139 ready  I/O=0x");
    net_puthex16(rtl_io);
    net_puts("  IRQ=");
    net_putdec((UW)rtl_irq);
    net_puts("  MAC=");
    for (INT i = 0; i < 6; i++) {
        if (i) net_puts(":");
        net_puthex8(rtl_mac[i]);
    }
    net_puts("\r\n");

    return E_OK;
}
