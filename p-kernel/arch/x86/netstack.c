/*
 *  netstack.c (x86)
 *  Minimal Ethernet + ARP + IP + ICMP stack for p-kernel
 *
 *  Handles:
 *    Ethernet  — frame dispatch
 *    ARP       — request/reply, 16-entry table
 *    IP        — header checksum, input routing, output build
 *    ICMP      — echo request → reply, echo reply logging
 *
 *  QEMU user networking: guest=10.0.2.15, gateway=10.0.2.2
 */

#include "netstack.h"
#include "rtl8139.h"
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* Stats                                                               */
/* ------------------------------------------------------------------ */

volatile UW net_rx_arp      = 0;
volatile UW net_rx_icmp_req = 0;
volatile UW net_rx_icmp_rep = 0;
volatile UW net_tx_arp      = 0;
volatile UW net_tx_icmp     = 0;
volatile UW net_rx_udp      = 0;
volatile UW net_tx_udp      = 0;

/* ------------------------------------------------------------------ */
/* Serial output helpers                                               */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void ns_puts(const char *s)
{
    INT n = 0;
    while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void ns_putdec(UW v)
{
    char buf[12];
    INT i = 11;
    buf[i] = '\0';
    if (v == 0) { ns_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    ns_puts(&buf[i]);
}


/* Format IP (IP4 format) as "A.B.C.D" */
const char *ip_str(UW ip)
{
    static char buf[16];
    UB a = (UB)(ip & 0xFF);
    UB b = (UB)((ip >> 8) & 0xFF);
    UB c = (UB)((ip >> 16) & 0xFF);
    UB d = (UB)((ip >> 24) & 0xFF);
    /* build string manually */
    INT i = 0;
    UB parts[4] = {a, b, c, d};
    for (INT p = 0; p < 4; p++) {
        if (p) buf[i++] = '.';
        UB v = parts[p];
        if (v >= 100) { buf[i++] = (char)('0' + v/100); v %= 100; }
        if (v >= 10 || parts[p] >= 100) { buf[i++] = (char)('0' + v/10); v %= 10; }
        buf[i++] = (char)('0' + v);
    }
    buf[i] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/* Our MAC address (read from NIC)                                     */
/* ------------------------------------------------------------------ */

static UB my_mac[6];

/* ------------------------------------------------------------------ */
/* ARP table                                                           */
/* ------------------------------------------------------------------ */

#define ARP_TABLE_SIZE  16

typedef struct {
    UW  ip;
    UB  mac[6];
    UB  valid;
} ARP_ENTRY;

static ARP_ENTRY arp_table[ARP_TABLE_SIZE];

static void arp_add(UW ip, const UB mac[6])
{
    /* Update existing entry */
    for (INT i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            for (INT j = 0; j < 6; j++) arp_table[i].mac[j] = mac[j];
            return;
        }
    }
    /* Add new entry */
    for (INT i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            arp_table[i].ip    = ip;
            arp_table[i].valid = 1;
            for (INT j = 0; j < 6; j++) arp_table[i].mac[j] = mac[j];
            return;
        }
    }
    /* Table full: overwrite first entry */
    arp_table[0].ip    = ip;
    arp_table[0].valid = 1;
    for (INT j = 0; j < 6; j++) arp_table[0].mac[j] = mac[j];
}

INT arp_lookup(UW ip, UB mac_out[6])
{
    for (INT i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            for (INT j = 0; j < 6; j++) mac_out[j] = arp_table[i].mac[j];
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* IP checksum                                                         */
/* ------------------------------------------------------------------ */

static UH ip_cksum(const void *data, INT byte_len)
{
    const UH *p = (const UH *)data;
    UW sum = 0;
    while (byte_len > 1) { sum += *p++; byte_len -= 2; }
    if (byte_len)         { sum += *(const UB *)p; }
    while (sum >> 16)     { sum = (sum & 0xFFFF) + (sum >> 16); }
    return (UH)(~sum);
}

/* ------------------------------------------------------------------ */
/* TX frame buffer                                                     */
/* ------------------------------------------------------------------ */

static UB tx_buf[1514];

static const UB BCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const UB ZERO_MAC[6]  = {0,0,0,0,0,0};

/* ------------------------------------------------------------------ */
/* ARP                                                                 */
/* ------------------------------------------------------------------ */

void arp_request(UW target_ip)
{
    ETH_HDR *eth = (ETH_HDR *)tx_buf;
    ARP_PKT *arp = (ARP_PKT *)(tx_buf + sizeof(ETH_HDR));

    /* Ethernet header */
    for (INT i = 0; i < 6; i++) { eth->dst[i] = BCAST_MAC[i]; eth->src[i] = my_mac[i]; }
    eth->type = ETH_TYPE_ARP;

    /* ARP payload */
    arp->htype = htons(1);
    arp->ptype = htons(0x0800);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(1);          /* request */
    for (INT i = 0; i < 6; i++) { arp->sha[i] = my_mac[i]; arp->tha[i] = ZERO_MAC[i]; }
    arp->spa   = NET_MY_IP;
    arp->tpa   = target_ip;

    rtl8139_send(tx_buf, (UH)(sizeof(ETH_HDR) + sizeof(ARP_PKT)));
    net_tx_arp++;
}

static void arp_reply(const ARP_PKT *req, const UB *req_src_mac)
{
    ETH_HDR *eth = (ETH_HDR *)tx_buf;
    ARP_PKT *arp = (ARP_PKT *)(tx_buf + sizeof(ETH_HDR));

    for (INT i = 0; i < 6; i++) { eth->dst[i] = req_src_mac[i]; eth->src[i] = my_mac[i]; }
    eth->type = ETH_TYPE_ARP;

    arp->htype = htons(1);
    arp->ptype = htons(0x0800);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(2);          /* reply */
    for (INT i = 0; i < 6; i++) { arp->sha[i] = my_mac[i]; arp->tha[i] = req->sha[i]; }
    arp->spa   = NET_MY_IP;
    arp->tpa   = req->spa;

    rtl8139_send(tx_buf, (UH)(sizeof(ETH_HDR) + sizeof(ARP_PKT)));
    net_tx_arp++;
}

static void arp_input(const UB *frame, INT len)
{
    if (len < (INT)(sizeof(ETH_HDR) + sizeof(ARP_PKT))) return;

    const ETH_HDR *eth = (const ETH_HDR *)frame;
    const ARP_PKT *arp = (const ARP_PKT *)(frame + sizeof(ETH_HDR));

    if (arp->htype != htons(1))      return;    /* Ethernet only */
    if (arp->ptype != htons(0x0800)) return;    /* IPv4 only     */
    if (arp->hlen != 6 || arp->plen != 4) return;

    /* Learn sender's MAC */
    arp_add(arp->spa, arp->sha);

    UH oper = ntohs(arp->oper);

    if (oper == 1 && arp->tpa == NET_MY_IP) {
        /* ARP request for us → reply */
        ns_puts("[arp] Request from "); ns_puts(ip_str(arp->spa));
        ns_puts(" — sending reply\r\n");
        arp_reply(arp, eth->src);
        net_rx_arp++;
    } else if (oper == 2) {
        /* ARP reply — MAC already learned above */
        ns_puts("[arp] Reply: "); ns_puts(ip_str(arp->spa));
        ns_puts(" is at ");
        for (INT i = 0; i < 6; i++) {
            if (i) ns_puts(":");
            const char *h = "0123456789ABCDEF";
            char buf[3] = { h[arp->sha[i]>>4], h[arp->sha[i]&0xF], '\0' };
            ns_puts(buf);
        }
        ns_puts("\r\n");
        net_rx_arp++;
    }
}

/* ------------------------------------------------------------------ */
/* IP output                                                           */
/* ------------------------------------------------------------------ */

static UH ip_id_ctr = 0x1234;

/* Determine next-hop IP for a given destination */
static UW next_hop(UW dst_ip)
{
    if ((dst_ip & NET_NETMASK) == (NET_MY_IP & NET_NETMASK))
        return dst_ip;          /* local */
    return NET_GW_IP;           /* default gateway */
}

/*
 * ip_send - build and send an IP packet
 *   dst_ip   : destination (IP4 format)
 *   proto    : IP_PROTO_ICMP / TCP / UDP
 *   payload  : data after IP header
 *   plen     : payload length in bytes
 * Returns 0 on success, -1 if ARP not resolved.
 */
static INT ip_send(UW dst_ip, UB proto, const UB *payload, UH plen)
{
    UW  hop = next_hop(dst_ip);
    UB  dst_mac[6];

    if (!arp_lookup(hop, dst_mac)) {
        arp_request(hop);
        return -1;              /* ARP pending — caller should retry */
    }

    ETH_HDR *eth = (ETH_HDR *)tx_buf;
    IP_HDR  *ip  = (IP_HDR  *)(tx_buf + sizeof(ETH_HDR));

    for (INT i = 0; i < 6; i++) { eth->dst[i] = dst_mac[i]; eth->src[i] = my_mac[i]; }
    eth->type = ETH_TYPE_IP;

    ip->vhl   = 0x45;
    ip->tos   = 0;
    ip->len   = htons((UH)(20 + plen));
    ip->id    = htons(ip_id_ctr++);
    ip->frag  = 0;
    ip->ttl   = 64;
    ip->proto = proto;
    ip->csum  = 0;
    ip->src   = NET_MY_IP;
    ip->dst   = dst_ip;
    ip->csum  = ip_cksum(ip, 20);

    UB *data = tx_buf + sizeof(ETH_HDR) + 20;
    for (UH i = 0; i < plen; i++) data[i] = payload[i];

    rtl8139_send(tx_buf, (UH)(sizeof(ETH_HDR) + 20 + plen));
    return 0;
}

/* ------------------------------------------------------------------ */
/* UDP                                                                 */
/* ------------------------------------------------------------------ */

#define UDP_MAX_SOCKS  8

static struct {
    UH          port;
    udp_recv_fn fn;
    UB          active;
} udp_socks[UDP_MAX_SOCKS];

INT udp_bind(UH port, udp_recv_fn fn)
{
    for (INT i = 0; i < UDP_MAX_SOCKS; i++) {
        if (!udp_socks[i].active) {
            udp_socks[i].port   = port;
            udp_socks[i].fn     = fn;
            udp_socks[i].active = 1;
            return 0;
        }
    }
    return -1;
}

static void udp_input(const IP_HDR *ip, const UB *seg, INT seg_len)
{
    if (seg_len < (INT)sizeof(UDP_HDR)) return;

    const UDP_HDR *udp   = (const UDP_HDR *)seg;
    UH udp_len  = ntohs(udp->length);
    if (udp_len < 8 || (INT)udp_len > seg_len) return;

    UH dst_port = ntohs(udp->dst_port);
    UH src_port = ntohs(udp->src_port);
    const UB *data   = seg + sizeof(UDP_HDR);
    UH data_len      = (UH)(udp_len - 8);

    net_rx_udp++;

    for (INT i = 0; i < UDP_MAX_SOCKS; i++) {
        if (udp_socks[i].active && udp_socks[i].port == dst_port) {
            udp_socks[i].fn(ip->src, src_port, data, data_len);
            return;
        }
    }
}

INT udp_send(UW dst_ip, UH src_port, UH dst_port,
             const UB *data, UH data_len)
{
    if (data_len > 508) return -1;

    static UB udp_buf[516];
    UDP_HDR *udp = (UDP_HDR *)udp_buf;
    UH udp_len   = (UH)(8 + data_len);

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons(udp_len);
    udp->checksum = 0;              /* optional for IPv4 — 0 = disabled */

    for (UH i = 0; i < data_len; i++) udp_buf[8 + i] = data[i];

    INT r = ip_send(dst_ip, IP_PROTO_UDP, udp_buf, udp_len);
    if (r == 0) net_tx_udp++;
    return r;
}

/* ------------------------------------------------------------------ */
/* DNS client (A-record lookup via NET_DNS_IP:53)                     */
/* ------------------------------------------------------------------ */

#define DNS_PORT     53
#define DNS_SRC_PORT 5300

static ID  dns_sem    = 0;
static UW  dns_result = 0;
static UB  dns_ok     = 0;

/*
 * Encode "google.com" → \x06google\x03com\x00
 * Returns bytes written, or -1 on error.
 */
static INT dns_encode_name(const char *name, UB *out, INT maxlen)
{
    INT pos = 0;
    while (*name) {
        const char *dot = name;
        while (*dot && *dot != '.') dot++;
        INT n = (INT)(dot - name);
        if (n == 0 || n > 63 || pos + 1 + n + 1 >= maxlen) return -1;
        out[pos++] = (UB)n;
        for (INT i = 0; i < n; i++) out[pos++] = (UB)name[i];
        name = dot;
        if (*name == '.') name++;
    }
    out[pos++] = 0;     /* root label */
    return pos;
}

/*
 * Skip a DNS name field (labels or compressed pointer).
 * Returns new position, or -1 on error.
 */
static INT dns_skip_name(const UB *d, INT pos, INT len)
{
    while (pos < len) {
        UB b = d[pos];
        if (b == 0)              return pos + 1;
        if ((b & 0xC0) == 0xC0) return pos + 2;   /* compressed pointer */
        pos += 1 + b;
    }
    return -1;
}

/* UDP callback: receives DNS response on port DNS_SRC_PORT */
static void dns_recv_cb(UW src_ip, UH src_port,
                        const UB *data, UH dlen)
{
    (void)src_ip; (void)src_port;

    if (dlen >= 12) {
        UH flags   = (UH)((data[2] << 8) | data[3]);
        UH ancount = (UH)((data[6] << 8) | data[7]);

        /* QR=1 (response) and RCODE=0 (no error) */
        if ((flags & 0x8000) && !(flags & 0x000F) && ancount > 0) {
            INT pos = dns_skip_name(data, 12, dlen); /* skip QNAME */
            if (pos >= 0 && pos + 4 <= dlen) {
                pos += 4;   /* skip QTYPE + QCLASS */

                for (UH i = 0; i < ancount && pos < dlen; i++) {
                    pos = dns_skip_name(data, pos, dlen);
                    if (pos < 0 || pos + 10 > dlen) break;
                    UH rtype = (UH)((data[pos]  << 8) | data[pos+1]);
                    UH rdlen = (UH)((data[pos+8] << 8) | data[pos+9]);
                    pos += 10;
                    if (rtype == 1 && rdlen == 4 && pos + 4 <= dlen) {
                        /* A record: wire bytes [a][b][c][d] */
                        dns_result = IP4(data[pos], data[pos+1],
                                         data[pos+2], data[pos+3]);
                        dns_ok = 1;
                        break;
                    }
                    pos += rdlen;
                }
            }
        }
    }
    tk_sig_sem(dns_sem, 1);
}

INT dns_query(const char *hostname, UW *out_ip)
{
    if (!dns_sem) return -1;

    static UB qbuf[256];
    /* DNS header */
    qbuf[0]=0x12; qbuf[1]=0x34;    /* ID */
    qbuf[2]=0x01; qbuf[3]=0x00;    /* Flags: QR=0, RD=1 */
    qbuf[4]=0x00; qbuf[5]=0x01;    /* QDCOUNT=1 */
    qbuf[6]=0x00; qbuf[7]=0x00;    /* ANCOUNT=0 */
    qbuf[8]=0x00; qbuf[9]=0x00;    /* NSCOUNT=0 */
    qbuf[10]=0x00; qbuf[11]=0x00;  /* ARCOUNT=0 */

    INT name_len = dns_encode_name(hostname, qbuf + 12, (INT)sizeof(qbuf) - 16);
    if (name_len < 0) return -1;
    INT pos = 12 + name_len;
    qbuf[pos++] = 0x00; qbuf[pos++] = 0x01;  /* QTYPE  = A  */
    qbuf[pos++] = 0x00; qbuf[pos++] = 0x01;  /* QCLASS = IN */

    dns_ok     = 0;
    dns_result = 0;

    /* Retry up to 5× if ARP for DNS server not yet resolved */
    INT r = -1;
    for (INT retry = 0; retry < 5; retry++) {
        r = udp_send(NET_DNS_IP, DNS_SRC_PORT, DNS_PORT, qbuf, (UH)pos);
        if (r == 0) break;
        tk_dly_tsk(200);    /* yield so net_task can process ARP reply */
    }
    if (r < 0) return r;

    /* Block up to 3 s for response */
    ER er = tk_wai_sem(dns_sem, 1, 3000);
    if (er != E_OK) return -1;

    if (dns_ok) { *out_ip = dns_result; return 0; }
    return -1;
}

/* ------------------------------------------------------------------ */
/* ICMP                                                                */
/* ------------------------------------------------------------------ */

#define PING_DATA_LEN  48

static UH ping_seq_ctr = 1;

INT icmp_ping(UW dst_ip, UH id, UH seq)
{
    /* Build ICMP echo request in a local buffer */
    static UB icmp_buf[sizeof(ICMP_HDR) + PING_DATA_LEN];
    ICMP_HDR *icmp = (ICMP_HDR *)icmp_buf;

    icmp->type = ICMP_ECHO_REQ;
    icmp->code = 0;
    icmp->csum = 0;
    icmp->id   = htons(id);
    icmp->seq  = htons(seq);

    /* Fill data */
    UB *data = icmp_buf + sizeof(ICMP_HDR);
    for (INT i = 0; i < PING_DATA_LEN; i++) data[i] = (UB)i;

    icmp->csum = ip_cksum(icmp_buf, (INT)sizeof(icmp_buf));

    INT r = ip_send(dst_ip, IP_PROTO_ICMP,
                    icmp_buf, (UH)sizeof(icmp_buf));
    if (r == 0) net_tx_icmp++;
    return r;
}

static void icmp_send_reply(const IP_HDR *req_ip, const ICMP_HDR *req_icmp,
                            const UB *req_data, UH data_len,
                            const UB *eth_src_mac)
{
    static UB icmp_buf[sizeof(ICMP_HDR) + 128];
    ICMP_HDR *rep = (ICMP_HDR *)icmp_buf;

    rep->type = ICMP_ECHO_REP;
    rep->code = 0;
    rep->csum = 0;
    rep->id   = req_icmp->id;
    rep->seq  = req_icmp->seq;

    UH plen = data_len;
    if (plen > 128) plen = 128;
    UB *repdata = icmp_buf + sizeof(ICMP_HDR);
    for (UH i = 0; i < plen; i++) repdata[i] = req_data[i];

    UH icmp_len = (UH)(sizeof(ICMP_HDR) + plen);
    rep->csum = ip_cksum(icmp_buf, icmp_len);

    /* Build the reply using the original src MAC directly for speed */
    ETH_HDR *eth = (ETH_HDR *)tx_buf;
    IP_HDR  *ip  = (IP_HDR  *)(tx_buf + sizeof(ETH_HDR));

    for (INT i = 0; i < 6; i++) { eth->dst[i] = eth_src_mac[i]; eth->src[i] = my_mac[i]; }
    eth->type = ETH_TYPE_IP;

    ip->vhl   = 0x45;
    ip->tos   = 0;
    ip->len   = htons((UH)(20 + icmp_len));
    ip->id    = htons(ip_id_ctr++);
    ip->frag  = 0;
    ip->ttl   = 64;
    ip->proto = IP_PROTO_ICMP;
    ip->csum  = 0;
    ip->src   = NET_MY_IP;
    ip->dst   = req_ip->src;
    ip->csum  = ip_cksum(ip, 20);

    UB *payload = tx_buf + sizeof(ETH_HDR) + 20;
    for (UH i = 0; i < icmp_len; i++) payload[i] = icmp_buf[i];

    rtl8139_send(tx_buf, (UH)(sizeof(ETH_HDR) + 20 + icmp_len));
}

static void icmp_input(const UB *frame, const IP_HDR *ip,
                       const UB *icmp_raw, INT icmp_len)
{
    if (icmp_len < (INT)sizeof(ICMP_HDR)) return;

    const ICMP_HDR *icmp = (const ICMP_HDR *)icmp_raw;
    const UB *data       = icmp_raw + sizeof(ICMP_HDR);
    UH  dlen             = (UH)(icmp_len - (INT)sizeof(ICMP_HDR));

    /* Verify checksum */
    if (ip_cksum(icmp_raw, icmp_len) != 0) {
        ns_puts("[icmp] bad checksum\r\n");
        return;
    }

    if (icmp->type == ICMP_ECHO_REQ) {
        /* Send echo reply */
        const ETH_HDR *eth = (const ETH_HDR *)frame;
        icmp_send_reply(ip, icmp, data, dlen, eth->src);
        net_rx_icmp_req++;
        ns_puts("[icmp] echo request from "); ns_puts(ip_str(ip->src));
        ns_puts(" → replied\r\n");

    } else if (icmp->type == ICMP_ECHO_REP) {
        /* Echo reply: this is what ping receives */
        net_rx_icmp_rep++;
        ns_puts("[icmp] echo REPLY from "); ns_puts(ip_str(ip->src));
        ns_puts("  id="); ns_putdec(ntohs(icmp->id));
        ns_puts("  seq="); ns_putdec(ntohs(icmp->seq));
        ns_puts("\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* IP input                                                            */
/* ------------------------------------------------------------------ */

static void ip_input(const UB *frame, INT len)
{
    if (len < (INT)(sizeof(ETH_HDR) + sizeof(IP_HDR))) return;

    const IP_HDR *ip = (const IP_HDR *)(frame + sizeof(ETH_HDR));

    /* Only handle IPv4, 20-byte header */
    if ((ip->vhl >> 4) != 4)  return;
    if ((ip->vhl & 0xF) != 5) return;  /* no options */

    /* Verify header checksum */
    if (ip_cksum(ip, 20) != 0) {
        ns_puts("[ip] bad checksum\r\n");
        return;
    }

    /* Only accept packets addressed to us or broadcast */
    if (ip->dst != NET_MY_IP && ip->dst != NET_BCAST) return;

    UH  ip_total = ntohs(ip->len);
    INT payload_off = (INT)(sizeof(ETH_HDR) + 20);
    INT payload_len = (INT)ip_total - 20;
    if (payload_len <= 0 || payload_off + payload_len > len) return;

    if      (ip->proto == IP_PROTO_ICMP) {
        icmp_input(frame, ip, frame + payload_off, payload_len);
    } else if (ip->proto == IP_PROTO_UDP) {
        udp_input(ip, frame + payload_off, payload_len);
    }
}

/* ------------------------------------------------------------------ */
/* Ethernet input dispatcher                                           */
/* ------------------------------------------------------------------ */

void eth_input(const UB *frame, INT len)
{
    if (len < (INT)sizeof(ETH_HDR)) return;

    const ETH_HDR *eth = (const ETH_HDR *)frame;
    UH type = eth->type;

    if      (type == ETH_TYPE_ARP) arp_input(frame, len);
    else if (type == ETH_TYPE_IP)  ip_input(frame, len);
}

/* ------------------------------------------------------------------ */
/* Startup                                                             */
/* ------------------------------------------------------------------ */

void netstack_start(void)
{
    /* Get our MAC from the NIC */
    rtl8139_get_mac(my_mac);

    /* Create DNS semaphore and register receive port */
    {
        T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                      .isemcnt = 0, .maxsem = 1 };
        dns_sem = tk_cre_sem(&cs);
    }
    udp_bind(DNS_SRC_PORT, dns_recv_cb);

    /* Send ARP request to discover gateway (10.0.2.2).
     * QEMU's virtual switch will respond with the gateway's MAC,
     * populating the ARP table before the user sends any pings. */
    ns_puts("[net] Sending ARP request for gateway ");
    ns_puts(ip_str(NET_GW_IP));
    ns_puts(" and DNS ");
    ns_puts(ip_str(NET_DNS_IP));
    ns_puts("\r\n");
    arp_request(NET_GW_IP);
    arp_request(NET_DNS_IP);
}

/* ------------------------------------------------------------------ */
/* Shell-accessible ping helper                                        */
/* ------------------------------------------------------------------ */

/*
 * Called from shell task.  Sends one ICMP echo request to dst_ip.
 * Returns  0: sent OK
 *         -1: ARP not resolved (ARP request sent, retry in ~1s)
 */
INT icmp_ping_shell(UW dst_ip)
{
    return icmp_ping(dst_ip, 0x5000, ping_seq_ctr++);
}
