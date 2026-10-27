/*
 * relay_unicast_test.c — [relay-unicast] (ROADMAP N1, research/n1-fleet-64.md
 * "宛先つきの送り"). The hosted relay link (arch/linux/x86_64/net_relay.c)
 * must hand a frame whose Ethernet destination is one node's MAC
 * (52:54:00:00:00:N, rtl8139.c) to the relay as REL_DATA dst=N, so the relay
 * forwards it to one node instead of fanning it out to all (audit-90: the relay
 * did 28x / 45x deliveries per packet at 32 / 64 nodes). Everything else stays
 * REL_BROADCAST. Receivers still filter by IP (netstack.c ip_input), so what a
 * node accepts does not change; only who is sent the copy does.
 *
 * Built by #including the TU (the test_net_lan.c pattern) and pointing it at
 * a socket of ours that plays the relay:
 *   U1 unicast node MAC            -> REL_DATA, dst = N
 *   U2 ff:ff:ff:ff:ff:ff           -> REL_BROADCAST, dst 0
 *   U3 multicast 01:00:5e:..       -> REL_BROADCAST
 *   U4 unicast MAC of another form -> REL_BROADCAST
 *   U5 node MAC with N = 0         -> REL_BROADCAST
 *   U6 our own MAC                 -> REL_BROADCAST (as before: never to self)
 *   U7 frame shorter than 6 bytes  -> REL_BROADCAST
 *   U8 PKERNEL_RELAY_UNICAST=0     -> U1's frame goes REL_BROADCAST (off switch)
 *   U9 the payload after the header is the frame, byte for byte (U1)
 *
 *   ./run_relay_unicast.sh      exit 0 = ALL PASS
 */
#define _GNU_SOURCE
#ifdef RU_TCP                       /* the same items over the TCP fallback link */
#include "../../arch/linux/x86_64/net_relay_tcp.c"
#define net_relay_init  net_relay_tcp_init
#define net_relay_send  net_relay_tcp_send
#define RU_SOCK         SOCK_STREAM
#define RU_NAME         "relay-unicast-tcp"
#else
#include "../../arch/linux/x86_64/net_relay.c"
#define RU_SOCK         SOCK_DGRAM
#define RU_NAME         "relay-unicast"
#endif

#include <sys/time.h>

/* the symbols the link takes from the rest of the hosted build */
int pkernel_default_node_id(void) { return 3; }
unsigned int net_my_ip;

static int fails;
static int relay_fd;                /* UDP: the socket; TCP: the accepted stream */

static void check(int ok, const char *name, const char *what)
{
    printf("[" RU_NAME "] %s %s: %s\n", name, ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

/* one packet off the wire: a datagram, or one [u16 BE len][pkt] TCP frame */
static int take1(unsigned char *buf, int cap)
{
#ifdef RU_TCP
    unsigned char h[2];
    if (recv(relay_fd, h, 2, MSG_WAITALL) != 2) return -1;
    int len = (h[0] << 8) | h[1];
    if (len > cap) return -1;
    return recv(relay_fd, buf, (size_t)len, MSG_WAITALL) == len ? len : -1;
#else
    return (int)recv(relay_fd, buf, (size_t)cap, 0);
#endif
}

/* receive one packet the node sent to "the relay"; skip REGISTER/KEEPALIVE */
static int take(unsigned char *buf, int cap)
{
    for (int i = 0; i < 8; i++) {
        int n = take1(buf, cap);
        if (n <= 0) return -1;
        if (buf[5] == REL_DATA || buf[5] == REL_BROADCAST) return n;
    }
    return -1;
}

static void frame_to(unsigned char *f, const unsigned char mac[6])
{
    memset(f, 0, 60);
    memcpy(f, mac, 6);
    f[6] = 0x52; f[7] = 0x54; f[11] = 3;     /* src MAC = node 3 */
    f[12] = 0x08; f[13] = 0x00;              /* IPv4 */
    for (int i = 14; i < 60; i++) f[i] = (unsigned char)i;
}

static void one(const char *name, const char *what, const unsigned char mac[6],
                int flen, int want_type, int want_dst)
{
    unsigned char f[60], pkt[MAX_PKT];
    frame_to(f, mac);
    net_relay_send(f, flen);
    int n = take(pkt, sizeof(pkt));
    int ok = n > 0 && pkt[5] == want_type && pkt[7] == want_dst;
    char line[160];
    snprintf(line, sizeof(line), "%s (got type=%d dst=%d)", what,
             n > 0 ? pkt[5] : -1, n > 0 ? pkt[7] : -1);
    check(ok, name, line);
}

int main(void)
{
    int lfd = socket(AF_INET, RU_SOCK, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0) { perror("bind"); return 2; }
    socklen_t al = sizeof(a);
    getsockname(lfd, (struct sockaddr *)&a, &al);
#ifdef RU_TCP
    listen(lfd, 1);
#endif
    relay_fd = lfd;

    char port[16];
    snprintf(port, sizeof(port), "%d", (int)ntohs(a.sin_port));
    setenv("PKERNEL_NODE_ID", "3", 1);
    setenv("PKERNEL_RELAY_HOST", "127.0.0.1", 1);
    setenv("PKERNEL_RELAY_PORT", port, 1);
    unsetenv("PKERNEL_RELAY");
    unsetenv("PKERNEL_RELAY_UNICAST");
    if (net_relay_init() < 0) { printf("[" RU_NAME "] FAIL init\n"); return 1; }
#ifdef RU_TCP
    relay_fd = accept(lfd, NULL, NULL);
    if (relay_fd < 0) { printf("[" RU_NAME "] FAIL accept\n"); return 1; }
#endif
    struct timeval tv = { 2, 0 };
    setsockopt(relay_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const unsigned char n7[6]   = { 0x52, 0x54, 0x00, 0x00, 0x00, 7 };
    const unsigned char bc[6]   = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    const unsigned char mc[6]   = { 0x01, 0x00, 0x5e, 0x00, 0x00, 7 };
    const unsigned char oth[6]  = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    const unsigned char n0[6]   = { 0x52, 0x54, 0x00, 0x00, 0x00, 0 };
    const unsigned char self[6] = { 0x52, 0x54, 0x00, 0x00, 0x00, 3 };

    one("U1", "node MAC 7 -> REL_DATA dst 7", n7, 60, REL_DATA, 7);
    one("U2", "broadcast MAC -> REL_BROADCAST", bc, 60, REL_BROADCAST, 0);
    one("U3", "multicast MAC -> REL_BROADCAST", mc, 60, REL_BROADCAST, 0);
    one("U4", "other unicast MAC -> REL_BROADCAST", oth, 60, REL_BROADCAST, 0);
    one("U5", "node MAC 0 -> REL_BROADCAST", n0, 60, REL_BROADCAST, 0);
    one("U6", "own MAC -> REL_BROADCAST", self, 60, REL_BROADCAST, 0);
    one("U7", "5-byte frame -> REL_BROADCAST", n7, 5, REL_BROADCAST, 0);

    {   /* U9: the frame rides unchanged after the header */
        unsigned char f[60], pkt[MAX_PKT];
        frame_to(f, n7);
        net_relay_send(f, 60);
        int n = take(pkt, sizeof(pkt));
        int hdr = (wire_version == RELAY_VER_V1) ? HEAD_LEN : HEAD_LEN + AUTH_LEN;
        check(n == hdr + 60 && memcmp(pkt + hdr, f, 60) == 0, "U9",
              "payload is the frame byte for byte");
    }

    setenv("PKERNEL_RELAY_UNICAST", "0", 1);
    one("U8", "PKERNEL_RELAY_UNICAST=0 -> node MAC 7 goes REL_BROADCAST", n7, 60,
        REL_BROADCAST, 0);

    printf("[" RU_NAME "] %s\n", fails ? "RED" : "ALL PASS");
    return fails ? 1 : 0;
}
