/*
 * kl_net.c — kloader 専用 最小ネットワークスタック
 *
 * OS 依存なし。静的バッファのみ使用。
 *
 * 実装範囲:
 *   - PCI バス スキャン (RTL8139 vendor=0x10EC device=0x8139)
 *   - RTL8139 最小ドライバ (TX/RX リングバッファ)
 *   - Ethernet II フレーム処理
 *   - ARP (request 送信 + reply 受信・応答)
 *   - IPv4 / UDP (送受信)
 *   - KLOAD_BEACON ブロードキャスト
 *   - KLOAD_START / KLOAD_CHUNK 受信 → メモリ書き込み
 */

#include "kl_net.h"

/* ------------------------------------------------------------------ */
/* I/O ポートアクセス                                                 */
/* ------------------------------------------------------------------ */

static inline u8  inb(u16 p) { u8  v; __asm__ volatile("inb  %1,%0":"=a"(v):"dN"(p)); return v; }
static inline u16 inw(u16 p) { u16 v; __asm__ volatile("inw  %1,%0":"=a"(v):"dN"(p)); return v; }
static inline u32 inl(u16 p) { u32 v; __asm__ volatile("inl  %1,%0":"=a"(v):"dN"(p)); return v; }
static inline void outb(u16 p, u8  v) { __asm__ volatile("outb %0,%1"::"a"(v),"dN"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1"::"a"(v),"dN"(p)); }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0,%1"::"a"(v),"dN"(p)); }

/* ------------------------------------------------------------------ */
/* シリアルデバッグ出力                                               */
/* ------------------------------------------------------------------ */

#define SERIAL_PORT 0x3F8

static void kn_putc(char c)
{
    int t = 10000;
    while (!(inb(SERIAL_PORT + 5) & 0x20) && t--);
    outb(SERIAL_PORT, (u8)c);
}

static void kn_puts(const char *s)
{
    while (*s) { if (*s == '\n') kn_putc('\r'); kn_putc(*s++); }
}

static void kn_putdec(u32 v)
{
    char buf[12]; int i = 11; buf[i] = '\0';
    if (!v) { kn_putc('0'); return; }
    while (v && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    kn_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* PCI アクセス                                                        */
/* ------------------------------------------------------------------ */

#define PCI_ADDR  0xCF8
#define PCI_DATA  0xCFC

static u32 pci_read(u8 bus, u8 dev, u8 fn, u8 reg)
{
    u32 addr = 0x80000000UL | ((u32)bus << 16) | ((u32)dev << 11)
             | ((u32)fn << 8) | (reg & 0xFC);
    outl(PCI_ADDR, addr);
    return inl(PCI_DATA);
}

static u16 pci_read16(u8 bus, u8 dev, u8 fn, u8 reg)
{
    return (u16)(pci_read(bus, dev, fn, reg) >> ((reg & 2) * 8));
}

static void pci_write16(u8 bus, u8 dev, u8 fn, u8 reg, u16 val)
{
    u32 addr = 0x80000000UL | ((u32)bus << 16) | ((u32)dev << 11)
             | ((u32)fn << 8) | (reg & 0xFC);
    outl(PCI_ADDR, addr);
    outw((u16)(PCI_DATA + (reg & 2)), val);
}

/* RTL8139 スキャン → I/O ベースアドレスを返す (0=not found) */
static u16 pci_find_rtl8139(void)
{
    for (u16 bus = 0; bus < 256; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u32 id = pci_read((u8)bus, dev, 0, 0);
            if ((id & 0xFFFF) != 0x10EC) continue;
            if ((id >> 16) != 0x8139)    continue;
            /* BUS MASTER 有効化 */
            u16 cmd = pci_read16((u8)bus, dev, 0, 4);
            pci_write16((u8)bus, dev, 0, 4, (u16)(cmd | 0x07));
            /* BAR0 = I/O ベースアドレス */
            u32 bar0 = pci_read((u8)bus, dev, 0, 0x10);
            return (u16)(bar0 & ~0x3U);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* RTL8139 レジスタ                                                   */
/* ------------------------------------------------------------------ */

#define RTL_IDR0        0x00    /* MAC アドレス (6B) */
#define RTL_MAR0        0x08    /* マルチキャストフィルタ */
#define RTL_CMD         0x37
#define RTL_CAPR        0x38    /* 現在 RX バッファアドレス */
#define RTL_CBR         0x3A    /* 現在 RX バッファ読み出しアドレス */
#define RTL_IMR         0x3C
#define RTL_ISR         0x3E
#define RTL_TCR         0x40    /* TX 設定 */
#define RTL_RCR         0x44    /* RX 設定 */
#define RTL_CONFIG1     0x52
#define RTL_TSAD        0x20    /* TX ステータス (4x DWORD) */
#define RTL_TPPOLL      0xD9
#define RTL_BMCR        0x62

#define RTL_CMD_RST     0x10
#define RTL_CMD_REN     0x08
#define RTL_CMD_TEN     0x04
#define RTL_ISR_ROK     0x0001
#define RTL_ISR_TOK     0x0004
#define RTL_ISR_TER     0x0008
#define RTL_RCR_AAP     0x00000001  /* 自分宛て */
#define RTL_RCR_APM     0x00000002  /* 物理マッチ */
#define RTL_RCR_AM      0x00000004  /* マルチキャスト */
#define RTL_RCR_AB      0x00000008  /* ブロードキャスト */
#define RTL_RCR_WRAP    0x00000080  /* リングバッファ折り返し */
#define RTL_RCR_MXDMA   0x00000700  /* バースト長: 無制限 */
#define RTL_RCR_RBLEN   0x00000000  /* 8K+16 バイト */

/* RX リングバッファ (8KB + 余裕) を固定アドレスに配置 */
#define RTL_RX_BUF_ADDR 0x60000U
#define RTL_RX_BUF_SIZE (8192 + 16 + 1500)

/* TX バッファ (4 スロット × 1536 バイト, TX は循環して使う) */
static u8  rtl_tx_buf[4][1536];
static int rtl_tx_slot = 0;
static u16 rtl_iobase  = 0;
static u16 rtl_rx_off  = 0;   /* RX リング読み出しオフセット */

/* ------------------------------------------------------------------ */
/* ネットワーク状態                                                   */
/* ------------------------------------------------------------------ */

static u8  my_mac[6];
static u32 my_ip;      /* 10.1.0.X ホストバイトオーダー */
static u8  node_id;    /* MAC 末尾 - 1 */

/* ------------------------------------------------------------------ */
/* バイトオーダー変換 (ネットワーク = ビッグエンディアン)             */
/* ------------------------------------------------------------------ */

static u16 htons(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static u16 ntohs(u16 v) { return htons(v); }
static u32 htonl(u32 v)
{
    return ((v >> 24) & 0xFF) | (((v >> 16) & 0xFF) << 8)
         | (((v >> 8) & 0xFF) << 16) | ((v & 0xFF) << 24);
}
static u32 ntohl(u32 v) { return htonl(v); }

/* ------------------------------------------------------------------ */
/* メモリユーティリティ                                               */
/* ------------------------------------------------------------------ */

static void kn_memcpy(void *d, const void *s, u32 n)
{
    u8 *dd = (u8 *)d; const u8 *ss = (const u8 *)s;
    for (u32 i = 0; i < n; i++) dd[i] = ss[i];
}

static void kn_memset(void *d, u8 v, u32 n)
{
    u8 *dd = (u8 *)d;
    for (u32 i = 0; i < n; i++) dd[i] = v;
}

static int kn_memcmp(const void *a, const void *b, u32 n)
{
    const u8 *aa = (const u8 *)a, *bb = (const u8 *)b;
    for (u32 i = 0; i < n; i++)
        if (aa[i] != bb[i]) return (int)aa[i] - (int)bb[i];
    return 0;
}

/* ------------------------------------------------------------------ */
/* IP チェックサム                                                    */
/* ------------------------------------------------------------------ */

static u16 ip_checksum(const void *data, u32 len)
{
    const u16 *p = (const u16 *)data;
    u32 sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const u8 *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum);
}

/* ------------------------------------------------------------------ */
/* RTL8139 初期化                                                     */
/* ------------------------------------------------------------------ */

static int rtl_init(u16 iobase)
{
    rtl_iobase = iobase;

    /* CONFIG1 LWAKE 解除 */
    outb((u16)(iobase + RTL_CONFIG1), 0x00);
    /* ソフトウェアリセット */
    outb((u16)(iobase + RTL_CMD), RTL_CMD_RST);
    int t = 100000;
    while ((inb((u16)(iobase + RTL_CMD)) & RTL_CMD_RST) && t--);
    if (t <= 0) return -1;

    /* MAC 読み出し */
    for (int i = 0; i < 6; i++)
        my_mac[i] = inb((u16)(iobase + RTL_IDR0 + i));

    /* node_id と IP 計算 */
    node_id = (u8)(my_mac[5] - 1);
    my_ip   = (10UL) | (1UL << 8) | (0UL << 16) | ((u32)my_mac[5] << 24);

    /* RX バッファアドレス設定 */
    outl((u16)(iobase + 0x30), RTL_RX_BUF_ADDR);

    /* 割り込みマスク (使わないが念のため) */
    outw((u16)(iobase + RTL_IMR), 0x0005);

    /* RX 設定: AAP+APM+AB+WRAP+MXDMA */
    outl((u16)(iobase + RTL_RCR),
         RTL_RCR_AAP | RTL_RCR_APM | RTL_RCR_AM |
         RTL_RCR_AB | RTL_RCR_WRAP | RTL_RCR_MXDMA);

    /* TX 設定 */
    outl((u16)(iobase + RTL_TCR), 0x00000700);

    /* TX バッファアドレス登録 */
    for (int i = 0; i < 4; i++)
        outl((u16)(iobase + 0x20 + i * 4), (u32)(u32)rtl_tx_buf[i]);

    /* TX/RX 有効化 */
    outb((u16)(iobase + RTL_CMD), RTL_CMD_TEN | RTL_CMD_REN);

    rtl_rx_off = 0;

    kn_puts("[kl_net] RTL8139  MAC=");
    const char *h = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        if (i) kn_putc(':');
        kn_putc(h[my_mac[i] >> 4]); kn_putc(h[my_mac[i] & 0xF]);
    }
    kn_puts("  IP=10.1.0.");
    kn_putdec((u32)my_mac[5]);
    kn_putc('\n');
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ethernet フレーム送信                                              */
/* ------------------------------------------------------------------ */

static void rtl_send(const u8 *frame, u16 len)
{
    if (len < 60) len = 60;   /* 最小フレームサイズ */
    int slot = rtl_tx_slot & 3;
    kn_memcpy(rtl_tx_buf[slot], frame, len);

    /* TX ステータスレジスタへサイズ書き込みで送信開始 */
    outl((u16)(rtl_iobase + 0x10 + slot * 4), (u32)len);

    /* 送信完了待ち */
    int t = 100000;
    while (t--) {
        u32 st = inl((u16)(rtl_iobase + 0x10 + slot * 4));
        if (st & 0x00000100) break;  /* TOK: TX OK */
    }
    rtl_tx_slot++;
}

/* ------------------------------------------------------------------ */
/* Ethernet フレーム受信 (ポーリング)                                 */
/* buf に最大 max_len バイトコピーして実際の長さを返す (0=なし)       */
/* ------------------------------------------------------------------ */

static u16 rtl_recv(u8 *buf, u16 max_len)
{
    /* CMD レジスタで RX バッファ空チェック */
    if (inb((u16)(rtl_iobase + RTL_CMD)) & 0x01) return 0;  /* BUFE=empty */

    u8 *rx = (u8 *)RTL_RX_BUF_ADDR;

    /* RTL8139 RX パケットヘッダ: status(2) + len(2) */
    u16 pkt_status = (u16)(rx[rtl_rx_off] | (rx[rtl_rx_off + 1] << 8));
    u16 pkt_len    = (u16)(rx[rtl_rx_off + 2] | (rx[rtl_rx_off + 3] << 8));

    if (!(pkt_status & 0x0001)) return 0;  /* ROK ビット未セット */
    if (pkt_len < 14 || pkt_len > 1514) {
        /* 壊れたパケット — スキップ */
        rtl_rx_off = (u16)((rtl_rx_off + 4 + ((pkt_len + 3) & ~3)) % (8192 + 16));
        outw((u16)(rtl_iobase + RTL_CAPR), (u16)(rtl_rx_off - 16));
        return 0;
    }

    u16 copy = (pkt_len < max_len) ? pkt_len : max_len;
    u32 data_off = rtl_rx_off + 4;
    /* リングバッファ折り返し考慮コピー */
    for (u16 i = 0; i < copy; i++)
        buf[i] = rx[(data_off + i) % (8192 + 16)];

    /* 次パケット位置 (4 バイトアラインメント) */
    rtl_rx_off = (u16)((rtl_rx_off + 4 + pkt_len + 3) & ~3) % (8192 + 16);
    outw((u16)(rtl_iobase + RTL_CAPR), (u16)(rtl_rx_off - 16));

    return copy;
}

/* ------------------------------------------------------------------ */
/* Ethernet / ARP / UDP フレーム構築                                  */
/* ------------------------------------------------------------------ */

static u8 tx_frame[1536];

/* broadcast MAC */
static const u8 bcast_mac[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

/* Ethernet ヘッダ書き込み */
static void eth_fill(u8 *f, const u8 *dst_mac, u16 etype)
{
    kn_memcpy(f,     dst_mac, 6);
    kn_memcpy(f + 6, my_mac,  6);
    f[12] = (u8)(etype >> 8);
    f[13] = (u8)(etype);
}

/* ARP request: who-has target_ip tell me */
static void send_arp_request(u32 target_ip)
{
    kn_memset(tx_frame, 0, 60);
    eth_fill(tx_frame, bcast_mac, 0x0806);

    u8 *a = tx_frame + 14;
    /* HTYPE=1 PTYPE=0x0800 HLEN=6 PLEN=4 OP=1 */
    a[0]=0; a[1]=1; a[2]=8; a[3]=0; a[4]=6; a[5]=4; a[6]=0; a[7]=1;
    kn_memcpy(a + 8,  my_mac, 6);
    u32 my_ip_n = htonl(my_ip);
    kn_memcpy(a + 14, &my_ip_n, 4);
    kn_memset(a + 18, 0, 6);
    u32 tip_n = htonl(target_ip);
    kn_memcpy(a + 24, &tip_n, 4);
    rtl_send(tx_frame, 60);
}

/* UDP データグラム送信 */
static void send_udp(const u8 *dst_mac, u32 dst_ip,
                     u16 src_port, u16 dst_port,
                     const u8 *data, u16 data_len)
{
    u16 udp_len  = (u16)(8 + data_len);
    u16 ip_len   = (u16)(20 + udp_len);
    u16 frame_len = (u16)(14 + ip_len);

    kn_memset(tx_frame, 0, (u32)frame_len);
    eth_fill(tx_frame, dst_mac, 0x0800);

    /* IP ヘッダ */
    u8 *ip = tx_frame + 14;
    ip[0] = 0x45;           /* version=4, IHL=5 */
    ip[1] = 0;
    ip[2] = (u8)(ip_len >> 8); ip[3] = (u8)ip_len;
    ip[4] = 0; ip[5] = 0;   /* ID */
    ip[6] = 0; ip[7] = 0;   /* flags / frag */
    ip[8] = 64;              /* TTL */
    ip[9] = 17;              /* UDP */
    u32 src_n = htonl(my_ip);
    u32 dst_n = htonl(dst_ip);
    kn_memcpy(ip + 12, &src_n, 4);
    kn_memcpy(ip + 16, &dst_n, 4);
    u16 cksum = ip_checksum(ip, 20);
    ip[10] = (u8)(cksum >> 8); ip[11] = (u8)cksum;

    /* UDP ヘッダ */
    u8 *udp = ip + 20;
    udp[0] = (u8)(src_port >> 8); udp[1] = (u8)src_port;
    udp[2] = (u8)(dst_port >> 8); udp[3] = (u8)dst_port;
    udp[4] = (u8)(udp_len >> 8);  udp[5] = (u8)udp_len;
    udp[6] = 0; udp[7] = 0;    /* チェックサム省略 */

    kn_memcpy(udp + 8, data, data_len);
    rtl_send(tx_frame, frame_len);
}

/* ------------------------------------------------------------------ */
/* KLOAD_BEACON ブロードキャスト                                      */
/* ------------------------------------------------------------------ */

static void send_beacon(void)
{
    KL_BEACON_PKT pkt;
    kn_memset(&pkt, 0, sizeof(pkt));
    pkt.magic   = KLOAD_MAGIC;
    pkt.version = KLOAD_VERSION;
    pkt.type    = KLOAD_BEACON;
    pkt.node_id = node_id;
    pkt.src_ip  = my_ip;   /* ホストバイトオーダー */

    send_udp(bcast_mac, 0xFFFFFFFFUL,
             KLOAD_PORT_RX, KLOAD_PORT_BCN,
             (const u8 *)&pkt, (u16)sizeof(pkt));

    kn_puts("[kl_net] KLOAD_BEACON sent (node ");
    kn_putdec(node_id);
    kn_puts(")\n");
}

/* ------------------------------------------------------------------ */
/* 受信フレーム処理                                                   */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    u8  dst_mac[6], src_mac[6];
    u16 etype;
} ETH_HDR;

typedef struct __attribute__((packed)) {
    u8  ver_ihl, dscp, len_hi, len_lo;
    u16 id, flags_frag;
    u8  ttl, proto;
    u16 cksum;
    u32 src, dst;
} IP_HDR;

typedef struct __attribute__((packed)) {
    u16 src_port, dst_port, len, cksum;
} UDP_HDR;

typedef struct __attribute__((packed)) {
    u32 magic;
    u8  version;
    u8  type;
    u8  src_node;
    u8  _pad;
    u32 total_size;
    u32 chunk_idx;
    u16 chunk_len;
    u8  data[KLOAD_CHUNK_SIZE];
} KLOAD_PKT;

/* 受信状態 */
static u32  kload_total   = 0;
static u32  kload_written = 0;
static int  kload_active  = 0;

static u8 rx_frame[1536];

/* 受信フレームを処理して KLOAD データを dst へ書き込む。
   完了したら kload_written を返す (それ以外 0)。 */
static u32 process_frame(u8 *dst, u32 max_size)
{
    u16 rlen = rtl_recv(rx_frame, sizeof(rx_frame));
    if (rlen < 14) return 0;

    ETH_HDR *eth = (ETH_HDR *)rx_frame;

    /* ARP 処理: 自分宛ての who-has に reply する */
    if (ntohs(eth->etype) == 0x0806) {
        u8 *a = rx_frame + 14;
        u16 op = (u16)((a[6] << 8) | a[7]);
        if (op == 1) {
            /* ARP request */
            u32 tip; kn_memcpy(&tip, a + 24, 4);
            if (ntohl(tip) == my_ip) {
                /* reply */
                u8 r[60]; kn_memset(r, 0, 60);
                kn_memcpy(r,     a + 8, 6);   /* dst = requester MAC */
                kn_memcpy(r + 6, my_mac, 6);
                r[12]=8; r[13]=6;    /* ARP */
                u8 *ra = r + 14;
                ra[0]=0; ra[1]=1; ra[2]=8; ra[3]=0;
                ra[4]=6; ra[5]=4; ra[6]=0; ra[7]=2;
                kn_memcpy(ra + 8,  my_mac, 6);
                u32 my_n = htonl(my_ip);
                kn_memcpy(ra + 14, &my_n, 4);
                kn_memcpy(ra + 18, a + 8, 6);
                kn_memcpy(ra + 24, a + 14, 4);
                rtl_send(r, 60);
            }
        }
        return 0;
    }

    /* IPv4 UDP のみ処理 */
    if (ntohs(eth->etype) != 0x0800) return 0;
    if (rlen < 14 + 20 + 8) return 0;

    IP_HDR  *ip  = (IP_HDR  *)(rx_frame + 14);
    if (ip->proto != 17) return 0;
    u32 ihl = (u32)(ip->ver_ihl & 0x0F) * 4;

    UDP_HDR *udp = (UDP_HDR *)(rx_frame + 14 + ihl);
    if (ntohs(udp->dst_port) != KLOAD_PORT_RX) return 0;

    u16 udp_data_len = (u16)(ntohs(udp->len) - 8);
    if (udp_data_len < 12) return 0;

    KLOAD_PKT *pkt = (KLOAD_PKT *)((u8 *)udp + 8);
    if (pkt->magic   != KLOAD_MAGIC)   return 0;
    if (pkt->version != KLOAD_VERSION) return 0;

    if (pkt->type == KLOAD_START) {
        kload_total   = pkt->total_size;
        kload_written = 0;
        kload_active  = 1;
        /* 受信バッファをゼロクリア */
        kn_memset(dst, 0, (kload_total < max_size) ? kload_total : max_size);
        kn_puts("[kl_net] KLOAD_START  total=");
        kn_putdec(kload_total);
        kn_puts(" bytes\n");
        return 0;
    }

    if (pkt->type == KLOAD_CHUNK && kload_active) {
        u32 offset = pkt->chunk_idx * KLOAD_CHUNK_SIZE;
        u16 clen   = pkt->chunk_len;
        if (offset + clen > max_size) return 0;

        kn_memcpy((u8 *)dst + offset, pkt->data, clen);

        if (offset + clen > kload_written)
            kload_written = offset + clen;

        if (pkt->chunk_idx % 64 == 0) {
            kn_puts("[kl_net] chunk ");
            kn_putdec(pkt->chunk_idx);
            kn_puts("  written=");
            kn_putdec(kload_written);
            kn_puts("\n");
        }

        if (kload_written >= kload_total) {
            kn_puts("[kl_net] kernel received!  size=");
            kn_putdec(kload_written);
            kn_puts("\n");
            kload_active = 0;
            return kload_written;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 簡易タイマー (BIOS タイマーなし → ループカウント)                 */
/* ------------------------------------------------------------------ */

static void kl_delay(u32 count)
{
    volatile u32 v = count;
    while (v--) __asm__ volatile("nop");
}

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

int kl_net_init(void)
{
    u16 iobase = pci_find_rtl8139();
    if (!iobase) {
        kn_puts("[kl_net] RTL8139 not found\n");
        return -1;
    }
    return rtl_init(iobase);
}

int kl_net_receive_kernel(void *dst, u32 max_size)
{
    kn_puts("[kl_net] entering kernel receive loop...\n");

    /* 最初に ARP 自己紹介 (ゲートウェイへ) */
    send_arp_request(my_ip);    /* gratuitous ARP もどき */

    u32 beacon_count   = 0;
    u32 poll_count     = 0;
    /* ビーコンは ~2 秒ごと、タイムアウトは ~60 秒 (ループ数は環境依存) */
    const u32 BEACON_INTERVAL = 2000000U;
    const u32 TIMEOUT_LIMIT   = 60000000U;

    send_beacon();  /* 最初のビーコン */
    beacon_count = BEACON_INTERVAL;

    while (poll_count < TIMEOUT_LIMIT) {
        u32 result = process_frame((u8 *)dst, max_size);
        if (result > 0) return (int)result;

        poll_count++;
        beacon_count--;
        if (beacon_count == 0) {
            send_beacon();
            beacon_count = BEACON_INTERVAL;
        }
    }

    kn_puts("[kl_net] timeout — no kernel received\n");
    return -1;
}
