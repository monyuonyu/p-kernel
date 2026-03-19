/*
 *  netstack.h (x86)
 *  Ethernet + ARP + IP + ICMP + UDP + DNS stack for p-kernel
 *
 *  Network configuration (QEMU user networking):
 *    My IP  : 10.0.2.15
 *    Gateway: 10.0.2.2
 *    DNS    : 10.0.2.3
 *    Netmask: 255.255.255.0
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* IP address helpers                                                  */
/* ------------------------------------------------------------------ */

/*
 * IP4 - construct an IP address as a UW in the form that appears when
 * reading a network-order field as a little-endian UW on x86.
 * Packet bytes: [a][b][c][d]  →  UW = d<<24 | c<<16 | b<<8 | a
 */
#define IP4(a,b,c,d)  ((UW)(((UB)(d)<<24)|((UB)(c)<<16)|((UB)(b)<<8)|(UB)(a)))

/* QEMU user-mode network defaults */
#define NET_MY_IP    IP4(10,  0, 2, 15)
#define NET_GW_IP    IP4(10,  0, 2,  2)
#define NET_DNS_IP   IP4(10,  0, 2,  3)
#define NET_NETMASK  IP4(255,255,255, 0)
#define NET_BCAST    IP4(10,  0, 2,255)

/* Byte-order helpers (host ↔ network) */
static inline UH htons(UH v) { return (UH)((v >> 8) | (v << 8)); }
static inline UW htonl(UW v)
{
    return ((v & 0x000000FF) << 24) | ((v & 0x0000FF00) << 8)
         | ((v & 0x00FF0000) >> 8)  | ((v & 0xFF000000) >> 24);
}
#define ntohs htons
#define ntohl htonl

/* ------------------------------------------------------------------ */
/* Packet header structs (all packed, network byte order)             */
/* ------------------------------------------------------------------ */

typedef struct {
    UB  dst[6];
    UB  src[6];
    UH  type;           /* htons: 0x0800=IP, 0x0806=ARP */
} __attribute__((packed)) ETH_HDR;

#define ETH_TYPE_IP   htons(0x0800)
#define ETH_TYPE_ARP  htons(0x0806)

typedef struct {
    UH  htype;          /* htons(1) = Ethernet */
    UH  ptype;          /* htons(0x0800) = IPv4 */
    UB  hlen;           /* 6 */
    UB  plen;           /* 4 */
    UH  oper;           /* htons(1)=request, htons(2)=reply */
    UB  sha[6];         /* sender MAC */
    UW  spa;            /* sender IP (IP4 format) */
    UB  tha[6];         /* target MAC */
    UW  tpa;            /* target IP (IP4 format) */
} __attribute__((packed)) ARP_PKT;

typedef struct {
    UB  vhl;            /* 0x45 = version4 + 20-byte header */
    UB  tos;
    UH  len;            /* total length including header (htons) */
    UH  id;             /* htons */
    UH  frag;           /* flags + fragment offset */
    UB  ttl;
    UB  proto;          /* 1=ICMP, 6=TCP, 17=UDP */
    UH  csum;           /* IP header checksum */
    UW  src;            /* source IP (IP4 format) */
    UW  dst;            /* destination IP (IP4 format) */
} __attribute__((packed)) IP_HDR;

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

typedef struct {
    UB  type;           /* 8=echo request, 0=echo reply */
    UB  code;
    UH  csum;
    UH  id;             /* htons */
    UH  seq;            /* htons */
    /* followed by data */
} __attribute__((packed)) ICMP_HDR;

#define ICMP_ECHO_REQ  8
#define ICMP_ECHO_REP  0

typedef struct {
    UH  src_port;       /* htons */
    UH  dst_port;       /* htons */
    UH  length;         /* header + data, htons */
    UH  checksum;       /* 0 = disabled (optional for IPv4) */
    /* followed by data */
} __attribute__((packed)) UDP_HDR;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Format IP address as dotted decimal (static buffer, not reentrant) */
const char *ip_str(UW ip);

/* Process received Ethernet frame (called from net_task) */
void eth_input(const UB *frame, INT len);

/* Send ARP request for target IP and seed ARP table for gateway */
void netstack_start(void);

/* Send ICMP echo request. Returns 0 if sent, -1 if ARP not resolved */
INT icmp_ping(UW dst_ip, UH id, UH seq);

/* Look up ARP table. Returns 1 if found and mac_out filled. */
INT arp_lookup(UW ip, UB mac_out[6]);

/* Send ARP request for given IP */
void arp_request(UW target_ip);

/* UDP receive callback: called from net_task when a datagram arrives */
typedef void (*udp_recv_fn)(UW src_ip, UH src_port, const UB *data, UH len);

/* Bind a local UDP port to a callback. Returns 0 on success. */
INT udp_bind(UH port, udp_recv_fn fn);

/*
 * Send a UDP datagram.
 * Returns 0 on success, -1 if ARP not resolved (ARP request sent, retry).
 */
INT udp_send(UW dst_ip, UH src_port, UH dst_port, const UB *data, UH data_len);

/*
 * DNS A-record lookup.
 * Sends a query to NET_DNS_IP:53 and blocks up to 3 s.
 * Returns 0 and fills *out_ip on success, -1 on timeout/NXDOMAIN.
 */
INT dns_query(const char *hostname, UW *out_ip);

/* Stats */
extern volatile UW net_rx_arp;
extern volatile UW net_rx_icmp_req;
extern volatile UW net_rx_icmp_rep;
extern volatile UW net_tx_arp;
extern volatile UW net_tx_icmp;
extern volatile UW net_rx_udp;
extern volatile UW net_tx_udp;
