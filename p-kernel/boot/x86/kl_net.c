/*
 * kl_net.c — Network kernel loader for p-kernel kloader (Phase 16b)
 *
 * When no FAT32 disk is present (or KERNEL.ELF is not found),
 * kloader broadcasts a KLRQ ("Kernel Load Request") on UDP port 7370.
 * A running p-kernel node responds with the kernel ELF binary via
 * a simple chunked protocol (KLRS / KLRD / KLRE).
 *
 * Protocol (all UDP, port 7370, little-endian integers):
 *
 *   kloader → broadcast:
 *     "KLRQ" + my_ip[4] + my_mac[6]        (14 bytes)
 *
 *   server → unicast to kloader:
 *     "KLRS" + session_id[4] + total_size[4]  (12 bytes)
 *     "KLRD" + session_id[4] + offset[4] + len[2] + data[len]
 *     ...
 *     "KLRE" + session_id[4] + total_size[4]  (12 bytes)
 *
 * kloader assembles the ELF at the staging area (0x300000), then
 * calls kl_elf_load() to parse and load segments to their target
 * virtual addresses.
 *
 * NIC: RTL8139, found via PCI scan.
 * Memory layout:
 *   0x60000  RX ring buffer  (8 KB + 1516 guard = ~9700 bytes)
 *   0x62800  TX buffers      (1536 × 4 = 6 KB)
 *
 * OS-independent: no kernel headers, plain C99 with inline asm I/O.
 */

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Port I/O                                                            */
/* ------------------------------------------------------------------ */

static inline uint8_t  _inb(uint16_t p)
{ uint8_t  v; __asm__ volatile("inb  %1,%0":"=a"(v):"dN"(p)); return v; }
static inline uint16_t _inw(uint16_t p)
{ uint16_t v; __asm__ volatile("inw  %1,%0":"=a"(v):"dN"(p)); return v; }
static inline uint32_t _inl(uint16_t p)
{ uint32_t v; __asm__ volatile("inl  %1,%0":"=a"(v):"dN"(p)); return v; }
static inline void _outb(uint16_t p, uint8_t  v)
{ __asm__ volatile("outb %0,%1"::"a"(v),"dN"(p)); }
static inline void _outw(uint16_t p, uint16_t v)
{ __asm__ volatile("outw %0,%1"::"a"(v),"dN"(p)); }
static inline void _outl(uint16_t p, uint32_t v)
{ __asm__ volatile("outl %0,%1"::"a"(v),"dN"(p)); }

/* ------------------------------------------------------------------ */
/* Serial (COM1) for status messages                                  */
/* ------------------------------------------------------------------ */

#define COM1 0x3F8
static void net_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            while (!(_inb(COM1+5) & 0x20));
            _outb(COM1, '\r');
        }
        while (!(_inb(COM1+5) & 0x20));
        _outb(COM1, (uint8_t)*s++);
    }
}

/* ------------------------------------------------------------------ */
/* Memory utilities (no libc in kloader)                              */
/* ------------------------------------------------------------------ */

static void kl_memcpy(void *d, const void *s, uint32_t n)
{ uint8_t *dd=(uint8_t*)d; const uint8_t *ss=(const uint8_t*)s; while(n--)*dd++=*ss++; }

static void kl_memset(void *d, uint8_t v, uint32_t n)
{ uint8_t *dd=(uint8_t*)d; while(n--)*dd++=v; }

static int kl_memcmp(const void *a, const void *b, uint32_t n)
{
    const uint8_t *p=(const uint8_t*)a, *q=(const uint8_t*)b;
    while(n--){ if(*p!=*q) return (int)*p-(int)*q; p++;q++; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* PCI configuration space (port 0xCF8/0xCFC)                        */
/* ------------------------------------------------------------------ */

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t reg)
{
    _outl(0xCF8, 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) | (reg&0xFC));
    return _inl(0xCFC);
}
static void pci_write(uint8_t bus, uint8_t dev, uint8_t reg, uint32_t v)
{
    _outl(0xCF8, 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) | (reg&0xFC));
    _outl(0xCFC, v);
}

/* ------------------------------------------------------------------ */
/* RTL8139 minimal driver                                             */
/* ------------------------------------------------------------------ */

#define RTL_VENDOR  0x10EC
#define RTL_DEVICE  0x8139

/* Register offsets */
#define R_MAC       0x00
#define R_TSAD0     0x20    /* TX Start Address 0-3 (4×4 bytes) */
#define R_RBSTART   0x30
#define R_CMD       0x37
#define R_CAPR      0x38
#define R_CBR       0x3A
#define R_TCR       0x40
#define R_RCR       0x44
#define R_CONFIG1   0x52

/* RX ring buffer: 8 KB ring + 1516 bytes guard for wrap-around reads */
#define KL_RX_ADDR   0x60000U
#define KL_RX_RING   8192U
#define KL_RX_SIZE   (KL_RX_RING + 16 + 1516)

/* TX: 4 descriptors × 1536 bytes */
#define KL_TX_ADDR   0x62800U
#define KL_TX_BSIZ   1536U

static uint16_t g_io    = 0;            /* RTL8139 I/O base          */
static uint8_t  g_mac[6];              /* our MAC                   */
static uint8_t  g_ip[4];              /* our IP (10.1.0.mac[5])    */
static uint8_t  g_tx_cur = 0;          /* current TX descriptor     */
static uint32_t g_rx_pos = 0;          /* read position in RX ring  */

static inline uint8_t  r8 (uint8_t r) { return _inb ((uint16_t)(g_io+r)); }
static inline uint16_t r16(uint8_t r) { return _inw ((uint16_t)(g_io+r)); }
static inline void     w8 (uint8_t r, uint8_t  v) { _outb((uint16_t)(g_io+r), v); }
static inline void     w16(uint8_t r, uint16_t v) { _outw((uint16_t)(g_io+r), v); }
static inline void     w32(uint8_t r, uint32_t v) { _outl((uint16_t)(g_io+r), v); }

/* Find RTL8139 on PCI bus, initialise, set g_io / g_mac / g_ip.
 * Returns 1 on success, 0 if not found. */
static int rtl_init(void)
{
    /* Scan bus 0-7, slot 0-31 */
    for (int bus=0; bus<8; bus++) {
        for (int dev=0; dev<32; dev++) {
            uint32_t id = pci_read(bus, dev, 0x00);
            if (id == 0xFFFFFFFF) continue;
            if ((id & 0xFFFF) != RTL_VENDOR || (id>>16) != RTL_DEVICE) continue;

            /* Found — get BAR0 (I/O space) */
            uint32_t bar0 = pci_read(bus, dev, 0x10);
            g_io = (uint16_t)(bar0 & ~3u);

            /* Enable I/O space + Bus Master */
            uint32_t cmd = pci_read(bus, dev, 0x04);
            pci_write(bus, dev, 0x04, cmd | 0x05);

            /* Power on */
            w8(R_CONFIG1, 0x00);

            /* Software reset */
            w8(R_CMD, 0x10);
            { int t=100000; while((r8(R_CMD)&0x10) && t--); }

            /* RX buffer */
            w32(R_RBSTART, KL_RX_ADDR);

            /* Disable interrupts, clear ISR */
            w16(0x3C, 0x0000);
            w16(0x3E, 0xFFFF);

            /* RCR: APM | AB | MXDMA_unlimited | RBLEN_8K | RXFTH_none | WRAP */
            w32(R_RCR, (1u<<1)|(1u<<3)|(7u<<8)|(0u<<11)|(7u<<13)|(1u<<7));

            /* TCR: standard IFG, unlimited DMA */
            w32(R_TCR, (3u<<24)|(7u<<8));

            /* Enable TX + RX */
            w8(R_CMD, 0x0C);

            /* Read MAC from NIC registers */
            for (int i=0; i<6; i++) g_mac[i] = r8(R_MAC+i);

            /* Derive IP: 10.1.0.{mac[5]} (matches QEMU multicast node convention) */
            g_ip[0]=10; g_ip[1]=1; g_ip[2]=0; g_ip[3]=g_mac[5];

            return 1;
        }
    }
    return 0;
}

/* Send one Ethernet frame (polling, no IRQ) */
static void rtl_tx(const uint8_t *frame, uint16_t len)
{
    /* Copy to TX buffer */
    kl_memcpy((void*)(KL_TX_ADDR + g_tx_cur*KL_TX_BSIZ), frame, len);

    /* Set start address */
    w32((uint8_t)(R_TSAD0 + g_tx_cur*4), KL_TX_ADDR + g_tx_cur*KL_TX_BSIZ);

    /* Write length to TSD — this starts the TX */
    w32((uint8_t)(0x10 + g_tx_cur*4), len & 0x1FFF);

    /* Poll TOK (bit 15) */
    int t = 200000;
    while (!(_inl((uint16_t)(g_io+0x10+g_tx_cur*4)) & 0x8000) && t--);

    g_tx_cur = (g_tx_cur+1) & 3;
}

/*
 * Poll RX ring for the next packet.
 * Copies data to buf (up to maxlen bytes).
 * Returns actual length, or 0 if ring empty.
 */
static uint16_t rtl_rx(uint8_t *buf, uint16_t maxlen)
{
    /* Empty if rx_pos == CBR */
    if ((uint16_t)(g_rx_pos & 0xFFFF) == r16(R_CBR)) return 0;

    uint8_t  *hdr    = (uint8_t*)(KL_RX_ADDR + g_rx_pos);
    uint16_t  status = *(uint16_t*)hdr;
    uint16_t  pktsz  = *(uint16_t*)(hdr+2);         /* includes 4-byte CRC */
    uint16_t  datasz = (uint16_t)(pktsz - 4);

    if (!(status & 0x0001) || datasz == 0 || datasz > 1514) {
        /* bad packet — skip */
        g_rx_pos = (uint32_t)((g_rx_pos + 4 + 3) & ~3u);
        if (g_rx_pos >= KL_RX_RING) g_rx_pos -= KL_RX_RING;
        w16(R_CAPR, (uint16_t)((g_rx_pos - 16) & 0xFFFF));
        return 0;
    }

    uint16_t copy = (datasz < maxlen) ? datasz : maxlen;
    kl_memcpy(buf, hdr + 4, copy);

    /* Advance */
    g_rx_pos = (uint32_t)((g_rx_pos + 4 + pktsz + 3) & ~3u);
    if (g_rx_pos >= KL_RX_RING) g_rx_pos -= KL_RX_RING;
    w16(R_CAPR, (uint16_t)((g_rx_pos - 16) & 0xFFFF));

    return copy;
}

/* ------------------------------------------------------------------ */
/* Ethernet / IP / UDP packet builder                                 */
/* ------------------------------------------------------------------ */

static uint16_t ip_csum(const void *data, uint16_t len)
{
    const uint16_t *p = (const uint16_t*)data;
    uint32_t sum = 0;
    while (len >= 2) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* Multicast group used for KLRQ discovery: 230.0.0.1
 * Ethernet MAC for 230.0.0.1 = 01:00:5E:00:00:01
 * RCR_AM (Accept Multicast) is set in both kloader and p-kernel RTL8139. */
static const uint8_t MCAST_MAC[6] = {0x01,0x00,0x5E,0x00,0x00,0x01};
static const uint8_t MCAST_IP[4]  = {230,0,0,1};

/* Send a UDP multicast datagram */
static void send_udp_bcast(uint16_t src_port, uint16_t dst_port,
                            const uint8_t *payload, uint16_t plen)
{
    static uint8_t frame[1600];
    uint8_t *f = frame;

    /* Ethernet header (14 bytes) */
    kl_memcpy(f, MCAST_MAC, 6); f += 6;
    kl_memcpy(f, g_mac,     6); f += 6;
    f[0]=0x08; f[1]=0x00;       f += 2;

    /* IP header (20 bytes) */
    uint8_t *ip = frame + 14;
    uint16_t ip_total = 20 + 8 + plen;
    kl_memset(ip, 0, 20);
    ip[0]=0x45;
    ip[2]=(uint8_t)(ip_total>>8); ip[3]=(uint8_t)(ip_total);
    ip[8]=1;     /* TTL=1 (multicast) */
    ip[9]=17;    /* UDP */
    kl_memcpy(ip+12, g_ip,    4);
    kl_memcpy(ip+16, MCAST_IP,4);
    uint16_t cs = ip_csum(ip, 20);
    /* Store checksum in native (little-endian) byte order so that
     * p-kernel's netstack (which also reads uint16 as little-endian) can
     * verify it correctly with ip_cksum(). */
    ip[10]=(uint8_t)cs; ip[11]=(uint8_t)(cs>>8);

    /* UDP header (8 bytes) */
    uint8_t *udp = ip + 20;
    uint16_t udp_len = (uint16_t)(8 + plen);
    udp[0]=(uint8_t)(src_port>>8); udp[1]=(uint8_t)src_port;
    udp[2]=(uint8_t)(dst_port>>8); udp[3]=(uint8_t)dst_port;
    udp[4]=(uint8_t)(udp_len>>8);  udp[5]=(uint8_t)udp_len;
    udp[6]=0; udp[7]=0;  /* no checksum */

    kl_memcpy(udp+8, payload, plen);

    rtl_tx(frame, (uint16_t)(14 + ip_total));
}

/* ------------------------------------------------------------------ */
/* kloader network protocol                                           */
/* ------------------------------------------------------------------ */

#define KLOAD_PORT   7370
#define STAGING_ADDR 0x300000U
#define CHUNK_TIMEOUT_US  2000000U  /* 2 s per chunk */
#define KLRQ_RETRIES      10
#define KLRQ_RETRY_DELAY  500000U   /* 0.5 s between retries */

/* Forward declaration: ELF loader implemented in kl_fat32.c */
extern uint32_t kl_elf_load(uint32_t staging_addr);

/* Spin ~1 µs (crude: ~1000 nop iterations at ~1 GHz) */
static void delay_us(uint32_t us)
{
    while (us--) for (volatile int i=0; i<100; i++);
}

/*
 * Try to receive a UDP packet addressed to port KLOAD_PORT on our IP.
 * Polls for up to `timeout_us` microseconds.
 * On success: fills `udp_data` (max 1514 bytes) and sets *len_out.
 * Returns 1 on success, 0 on timeout.
 */
static uint8_t g_rx_frame[1514];

static int recv_kload_pkt(uint8_t *udp_data, uint16_t *len_out,
                           uint32_t timeout_us)
{
    while (timeout_us--) {
        uint16_t flen = rtl_rx(g_rx_frame, sizeof(g_rx_frame));
        if (!flen) {
            /* idle ~1 µs */
            for (volatile int i=0; i<100; i++);
            continue;
        }

        /* Parse Ethernet frame */
        if (flen < 14+20+8) continue;

        uint8_t *eth = g_rx_frame;
        uint16_t eth_type = (uint16_t)((eth[12]<<8)|eth[13]);
        if (eth_type != 0x0800) continue;  /* not IPv4 */

        uint8_t *ip = eth + 14;
        if ((ip[0]&0xF0) != 0x40) continue;  /* not IPv4 */
        if (ip[9] != 17) continue;            /* not UDP  */

        /* Accept: unicast to us, multicast 230.0.0.1, or broadcast */
        int to_us    = (kl_memcmp(ip+16, g_ip,     4) == 0);
        int to_mcast = (kl_memcmp(ip+16, MCAST_IP, 4) == 0);
        uint8_t bcast[4] = {255,255,255,255};
        int to_bcast = (kl_memcmp(ip+16, bcast,    4) == 0);
        if (!to_us && !to_mcast && !to_bcast) continue;

        uint8_t *udp = ip + (ip[0]&0x0F)*4;
        uint16_t dport = (uint16_t)((udp[2]<<8)|udp[3]);
        if (dport != KLOAD_PORT) continue;

        uint16_t udp_len = (uint16_t)(((udp[4]<<8)|udp[5]) - 8);
        if (udp_len < 4) continue;

        kl_memcpy(udp_data, udp+8, udp_len);
        *len_out = udp_len;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

/*
 * kl_net_receive_elf()
 *
 * 1. Scan PCI for RTL8139.
 * 2. Broadcast KLRQ (up to KLRQ_RETRIES times).
 * 3. Wait for KLRS (file size + session ID).
 * 4. Receive KLRD data chunks into staging area.
 * 5. Receive KLRE (end marker).
 * 6. Call kl_elf_load() and return entry point.
 *
 * Returns ELF e_entry, or 0 on failure.
 */
uint32_t kl_net_receive_elf(void)
{
    /* ---- NIC init ------------------------------------------------ */
    if (!rtl_init()) {
        net_puts("[kl_net] no RTL8139 found\n");
        return 0;
    }
    net_puts("[kl_net] RTL8139 ready  IP=10.1.0.");
    /* print last octet */
    {
        uint8_t v = g_ip[3];
        if (v >= 100) { _outb(COM1, '0'+(v/100)); _outb(COM1, '0'+(v/10)%10); }
        else if (v >= 10) _outb(COM1, '0'+(v/10));
        _outb(COM1, '0'+(v%10));
    }
    net_puts("\n");

    /* Build KLRQ payload: "KLRQ" + my_ip[4] + my_mac[6] */
    static uint8_t klrq[14];
    klrq[0]='K'; klrq[1]='L'; klrq[2]='R'; klrq[3]='Q';
    kl_memcpy(klrq+4,  g_ip,  4);
    kl_memcpy(klrq+8,  g_mac, 6);

    static uint8_t pkt_buf[1514];
    uint16_t pkt_len;

    /* ---- Broadcast KLRQ, wait for KLRS -------------------------- */
    uint32_t session_id = 0;
    uint32_t total_size  = 0;

    for (int attempt=0; attempt < KLRQ_RETRIES; attempt++) {
        net_puts("[kl_net] broadcasting KLRQ...\n");
        send_udp_bcast(KLOAD_PORT, KLOAD_PORT, klrq, sizeof(klrq));

        /* Wait up to 1s for KLRS */
        if (recv_kload_pkt(pkt_buf, &pkt_len, 1000000U)) {
            if (pkt_len >= 12 && kl_memcmp(pkt_buf, "KLRS", 4) == 0) {
                session_id = *(uint32_t*)(pkt_buf+4);
                total_size = *(uint32_t*)(pkt_buf+8);
                net_puts("[kl_net] KLRS received, size=");
                /* print size */
                uint32_t s = total_size;
                char tmp[12]; int ti=0;
                if (!s) { tmp[ti++]='0'; }
                else { while(s){ tmp[ti++]='0'+(s%10); s/=10; } }
                for (int i=ti-1; i>=0; i--) _outb(COM1, tmp[i]);
                net_puts(" bytes\n");
                break;
            }
        }

        if (session_id) break;
        delay_us(KLRQ_RETRY_DELAY);
    }

    if (!session_id || !total_size) {
        net_puts("[kl_net] no server responded\n");
        return 0;
    }

    /* ---- Receive KLRD data chunks -------------------------------- */
    uint32_t received = 0;

    while (received < total_size) {
        if (!recv_kload_pkt(pkt_buf, &pkt_len, CHUNK_TIMEOUT_US)) {
            net_puts("[kl_net] chunk timeout\n");
            return 0;
        }

        if (pkt_len < 4) continue;

        /* Check for KLRE (end) */
        if (kl_memcmp(pkt_buf, "KLRE", 4) == 0) {
            if (pkt_len >= 12 && *(uint32_t*)(pkt_buf+4) == session_id) {
                net_puts("[kl_net] KLRE received early\n");
                break;
            }
            continue;
        }

        /* Expect KLRD */
        if (kl_memcmp(pkt_buf, "KLRD", 4) != 0) continue;
        if (pkt_len < 14) continue;
        if (*(uint32_t*)(pkt_buf+4) != session_id) continue;

        uint32_t offset = *(uint32_t*)(pkt_buf+8);
        uint16_t len    = *(uint16_t*)(pkt_buf+12);
        if (pkt_len < (uint16_t)(14 + len)) continue;
        if (offset + len > total_size) continue;

        kl_memcpy((void*)(STAGING_ADDR + offset), pkt_buf+14, len);
        received = (offset + len > received) ? (offset + len) : received;
    }

    /* ---- Wait for KLRE if not received yet ----------------------- */
    if (received < total_size) {
        net_puts("[kl_net] incomplete: received/total mismatch\n");
        return 0;
    }

    /* Drain remaining packets looking for KLRE */
    for (int i=0; i<1000; i++) {
        if (recv_kload_pkt(pkt_buf, &pkt_len, 10000U)) {
            if (pkt_len >= 12 && kl_memcmp(pkt_buf, "KLRE", 4)==0 &&
                *(uint32_t*)(pkt_buf+4) == session_id) break;
        }
    }

    net_puts("[kl_net] kernel received, loading ELF...\n");
    return kl_elf_load(STAGING_ADDR);
}
