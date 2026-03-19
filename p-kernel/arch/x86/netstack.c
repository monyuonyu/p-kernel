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

    if (ip->proto == IP_PROTO_ICMP) {
        icmp_input(frame, ip, frame + payload_off, payload_len);
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

    /* Send ARP request to discover gateway (10.0.2.2).
     * QEMU's virtual switch will respond with the gateway's MAC,
     * populating the ARP table before the user sends any pings. */
    ns_puts("[net] Sending ARP request for gateway ");
    ns_puts(ip_str(NET_GW_IP));
    ns_puts("\r\n");
    arp_request(NET_GW_IP);
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
