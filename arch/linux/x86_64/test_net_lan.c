/*
 *  arch/linux/x86_64/test_net_lan.c
 *
 *  N-1 LAN-DIRECT unit test (HONEST sandbox scope).
 *
 *  A true 2-node LAN-broadcast mesh needs distinct hosts or network
 *  namespaces; ip-netns/unshare are BLOCKED in this Termux/proot sandbox
 *  (Operation not permitted), and two same-host processes cannot both
 *  bind 0.0.0.0:PORT without SO_REUSEPORT (which we deliberately refuse,
 *  because it would load-balance rather than fan-out). So this test
 *  exercises net_lan.c's logic in isolation by #including the TU and
 *  driving its real public API against a hand-built peer socket:
 *
 *    T1  v2 framing round-trip: a frame built with the shared PSK is
 *        accepted (payload returned byte-identical), and the SOURCE peer
 *        is LEARNED.
 *    T2  learn-table: the peer count goes 0 -> 1 on first contact and
 *        stays 1 on a second datagram from the same source (dedup).
 *    T3  PSK boundary: a frame MAC'd with the WRONG key is dropped
 *        (mac_drop_count increments, payload NOT delivered, peer NOT
 *        learned).
 *    T4  replay window: the exact same v2 datagram replayed is dropped
 *        (replay_drop_count increments).
 *    T5  self-echo filter: a frame whose src == our node id (our own
 *        broadcast looped back) is ignored.
 *    T6  send fans out: net_lan_send() to a known peer delivers the raw
 *        payload to that peer's socket (broadcast + unicast path).
 *
 *  Build:  see the [lan-mesh] cert commands in the PR body.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Pull the unit under test in directly so we can reach its statics and
 * the wire helpers (build_packet/compute_mac) to craft peer frames. */
#include "net_lan.c"

/* A second, independent "peer" socket bound to a different loopback port.
 * It stands in for a second node; we craft its v2 frames by reusing the
 * module's own build_packet() but under a peer-chosen key/nonce. */
static int peer_fd = -1;
static struct sockaddr_in peer_to_us;     /* address of net_lan's socket */
static struct sockaddr_in peer_self;      /* the peer's own bound address */

/* Build a v2 frame exactly like build_packet() but with an ARBITRARY key
 * and src id (so we can mint frames "from" another node, incl. wrong-key). */
static int craft_v2(unsigned char *buf, unsigned src, u64 nonce,
                    const u8 the_key[KEY_LEN], const void *payload, int plen)
{
    buf[0] = (u8)(RELAY_MAGIC      & 0xff);
    buf[1] = (u8)((RELAY_MAGIC>>8) & 0xff);
    buf[2] = (u8)((RELAY_MAGIC>>16)& 0xff);
    buf[3] = (u8)((RELAY_MAGIC>>24)& 0xff);
    buf[4] = RELAY_VER_V2;
    buf[5] = REL_BROADCAST;
    buf[6] = (u8)src;
    buf[7] = 0;
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    store_u64_le(buf + HEAD_LEN, nonce);

    /* compute_mac() uses the module-global `key`; temporarily swap it. */
    u8 saved[KEY_LEN];
    memcpy(saved, key, KEY_LEN);
    memcpy(key, the_key, KEY_LEN);
    compute_mac(RELAY_VER_V2, REL_BROADCAST, src, 0, nonce, payload, plen,
                buf + HEAD_LEN + 8);
    memcpy(key, saved, KEY_LEN);

    if (payload && plen > 0) memcpy(buf + HEAD_LEN + AUTH_LEN, payload, plen);
    return HEAD_LEN + AUTH_LEN + plen;
}

static void send_from_peer(const unsigned char *buf, int n)
{
    ssize_t r = sendto(peer_fd, buf, (size_t)n, 0,
                       (struct sockaddr *)&peer_to_us, sizeof(peer_to_us));
    assert(r == n);
    usleep(20000);   /* let the datagram land in our recv queue */
}

int main(void)
{
    /* The shared PSK (64 hex chars = 32 bytes). */
    const char *good_hex =
        "0011223344556677889900112233445566778899001122334455667788990011";
    setenv("PKERNEL_RELAY_KEY", good_hex, 1);
    setenv("PKERNEL_NODE_ID", "1", 1);
    /* Use a non-default port so we never collide with a running node. */
    setenv("PKERNEL_LAN_PORT", "47351", 1);

    int id = net_lan_init();
    assert(id == 1);
    assert(wire_version == RELAY_VER_V2);
    assert(net_lan_peer_count() == 0);
    printf("[lan-mesh] init: node 1, v2 wire, 0 peers — OK\n");

    /* The key bytes net_lan decoded, so the peer can MAC with the same PSK. */
    u8 good_key[KEY_LEN];
    memcpy(good_key, key, KEY_LEN);

    /* Build the peer socket on loopback:47352 and learn where net_lan listens. */
    peer_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(peer_fd >= 0);
    memset(&peer_self, 0, sizeof(peer_self));
    peer_self.sin_family      = AF_INET;
    peer_self.sin_addr.s_addr = htonl(0x7F000001);
    peer_self.sin_port        = htons(47352);
    assert(bind(peer_fd, (struct sockaddr *)&peer_self, sizeof(peer_self)) == 0);

    memset(&peer_to_us, 0, sizeof(peer_to_us));
    peer_to_us.sin_family      = AF_INET;
    peer_to_us.sin_addr.s_addr = htonl(0x7F000001);
    peer_to_us.sin_port        = htons(47351);

    unsigned char frame[256], out[2048];
    int rc;

    /* T1: good-key v2 frame from node 2 -> accepted, payload intact, learned. */
    const char *msg1 = "ETH-FRAME-FROM-NODE-2";
    int fn = craft_v2(frame, 2, 1001, good_key, msg1, (int)strlen(msg1));
    send_from_peer(frame, fn);
    rc = net_lan_recv(out, sizeof(out));
    assert(rc == (int)strlen(msg1));
    assert(memcmp(out, msg1, rc) == 0);
    assert(net_lan_peer_count() == 1);
    {
        unsigned long ok, bad, rep;
        net_lan_stats(&ok, &bad, &rep);
        assert(ok == 1 && bad == 0 && rep == 0);
    }
    printf("[lan-mesh] T1 framing round-trip + learn: payload intact, peers=1 — OK\n");

    /* T2: second datagram from same source -> dedup, peers stays 1. */
    fn = craft_v2(frame, 2, 1002, good_key, msg1, (int)strlen(msg1));
    send_from_peer(frame, fn);
    rc = net_lan_recv(out, sizeof(out));
    assert(rc == (int)strlen(msg1));
    assert(net_lan_peer_count() == 1);
    printf("[lan-mesh] T2 learn-table dedup: peers still 1 — OK\n");

    /* T3: WRONG-key frame -> dropped, not delivered, not learned. */
    u8 bad_key[KEY_LEN];
    memset(bad_key, 0xAB, KEY_LEN);
    fn = craft_v2(frame, 3, 2001, bad_key, "SPOOF", 5);
    send_from_peer(frame, fn);
    rc = net_lan_recv(out, sizeof(out));
    assert(rc == 0);                       /* nothing delivered */
    assert(net_lan_peer_count() == 1);     /* spoofer not learned */
    {
        unsigned long ok, bad, rep;
        net_lan_stats(&ok, &bad, &rep);
        assert(bad == 1);
    }
    printf("[lan-mesh] T3 PSK boundary: wrong-key frame dropped, not learned — OK\n");

    /* T4: replay the exact T1 frame (same src+nonce) -> replay drop. */
    fn = craft_v2(frame, 2, 1001, good_key, msg1, (int)strlen(msg1));
    send_from_peer(frame, fn);
    rc = net_lan_recv(out, sizeof(out));
    assert(rc == 0);
    {
        unsigned long ok, bad, rep;
        net_lan_stats(&ok, &bad, &rep);
        assert(rep == 1);
    }
    printf("[lan-mesh] T4 replay window: replayed nonce dropped — OK\n");

    /* T5: a frame whose src == our own id (looped-back broadcast) -> ignored. */
    fn = craft_v2(frame, 1, 1003, good_key, "SELF", 4);
    send_from_peer(frame, fn);
    rc = net_lan_recv(out, sizeof(out));
    assert(rc == 0);
    printf("[lan-mesh] T5 self-echo filter: own broadcast ignored — OK\n");

    /* T6: net_lan_send() fans out to the LEARNED peer; the peer socket
     *     receives a v2 frame whose stripped payload is our raw bytes. */
    const char *msg2 = "HELLO-PEER-2";
    rc = net_lan_send(msg2, (int)strlen(msg2));
    assert(rc == (int)strlen(msg2));
    usleep(20000);
    {
        unsigned char rb[2048];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int fl = fcntl(peer_fd, F_GETFL, 0);
        fcntl(peer_fd, F_SETFL, fl | O_NONBLOCK);
        /* The unicast to the learned peer (127.0.0.1:47352) arrives; the
         * broadcast copy may or may not reach a bound unicast socket, so we
         * just require AT LEAST one v2 frame carrying msg2. */
        int got_payload = 0;
        for (;;) {
            ssize_t r = recvfrom(peer_fd, rb, sizeof(rb), 0,
                                 (struct sockaddr *)&src, &sl);
            if (r <= 0) break;
            int hdr = HEAD_LEN + AUTH_LEN;
            if (r >= hdr && (int)(r - hdr) == (int)strlen(msg2) &&
                memcmp(rb + hdr, msg2, strlen(msg2)) == 0) {
                got_payload = 1;
            }
        }
        assert(got_payload);
    }
    printf("[lan-mesh] T6 send fan-out to learned peer: payload delivered — OK\n");

    printf("[lan-mesh] UNIT CERT PASS (6/6) — net_lan framing/learn/PSK/replay verified\n");
    printf("[lan-mesh] NOTE: true 2-node LAN broadcast NOT run — ip-netns/unshare "
           "blocked in sandbox; device/host-confirmable on mk_pino's 2 phones.\n");
    return 0;
}
