/*
 *  arch/linux/x86_64/net_lan.c
 *
 *  N-1 LAN-DIRECT: a third, relay-free transport backend for the
 *  rtl8139 driver shim. Nodes on the SAME local network mesh directly
 *  over UDP — no central relay process, no public endpoint.
 *
 *  Selected at runtime by net_dispatch.c when PKERNEL_LAN=1, with the
 *  same precedence shape as net_relay (env-gated). net_unix (loopback)
 *  and net_relay (public relay) are left UNCHANGED.
 *
 *  How it meshes (the Skype-like LAN rendezvous, see
 *  docs/architecture/20-architecture/p2p-overlay.md, slice N-1):
 *    - One UDP socket, AF_INET/SOCK_DGRAM, SO_REUSEADDR + SO_BROADCAST,
 *      bound to 0.0.0.0:PKERNEL_LAN_PORT (default 7351).
 *    - net_lan_send() broadcasts each outbound Ethernet frame to
 *      255.255.255.255:PORT (first-contact rendezvous) AND unicasts it
 *      to every peer we have already LEARNED. SWIM's existing once/sec
 *      beacon to IP4(255,255,255,255) (inside the frame) thus leaves the
 *      host as a real LAN UDP broadcast — no new discovery protocol.
 *    - net_lan_recv() recvfrom's, and on EVERY inbound datagram LEARNs
 *      the source sockaddr into a peer table. After first contact a node
 *      is reachable by direct unicast; broadcast is only the bootstrap.
 *
 *  PSK boundary: like net_relay, this TU uses the relay v2 wire
 *  (HMAC-SHA256 + 64-packet sliding nonce window) whenever
 *  PKERNEL_RELAY_KEY is set, so only nodes holding the shared PSK admit
 *  each other's frames. Without a key it falls back to the v1 plaintext
 *  wire (LAN bring-up against trusted hosts only). The wire byte layout
 *  is identical to net_relay's, but there is no relay hop: src/dst live
 *  in the frame's own synthetic 10.1.0.N IP stack, which rides opaque
 *  INSIDE the Ethernet payload and is never parsed here.
 *
 *  The synthetic IP stack is untouched — net_lan only moves whole
 *  Ethernet frames, exactly like net_unix/net_relay.
 *
 *  Same source compiled for x86_64 and aarch64 (per-arch to match the
 *  existing net_unix.c / net_relay.c layout). T-Kernel headers are NOT
 *  included here (same rule as net_unix.c / net_relay.c).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "sha256.h"   /* from relay/ (same include path net_relay uses);
                       * deliberately avoids <stdint.h> to coexist with the
                       * T-Kernel stdint shadow this TU is built under. */

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* errno shim (T-Kernel placeholder <errno.h> shadows the system one). */
extern int *__errno_location(void) __attribute__((__const__));
#define errno (*__errno_location())
extern char *strerror(int);

/* N-0: stable, distinct per-install default id (arch/linux/node_id.c). */
extern int pkernel_default_node_id(void);

/* --- wire constants (byte-identical to net_relay.c's v1/v2 framing) ----- */
#define RELAY_MAGIC        0x52454C59U   /* "RELY" little-endian */
#define RELAY_VER_V1       1
#define RELAY_VER_V2       2
#define HEAD_LEN           12
#define AUTH_LEN           24            /* nonce(8) + hmac16(16) */
#define HMAC_TRUNC_LEN     16
#define KEY_LEN            32
#define MAX_PAYLOAD        1380
#define MAX_PKT            (HEAD_LEN + AUTH_LEN + MAX_PAYLOAD)

#define REL_DATA           2
#define REL_BROADCAST      4

#define LAN_DEFAULT_PORT   7351

/* Mirror DNODE_MAX (drpc.h) — see the same note in net_relay.c. A node id
 * above this meshes on the wire but never joins drpc/pmesh/kdds. */
#define NET_CLUSTER_NODE_MAX 64

/* Learned-peer table. Broadcast is only first contact; once a peer's
 * source address is learned, subsequent sends go direct-unicast to it.
 * All state is static (never on a task stack — see
 * feedback_hosted_relay_stack_overflow). */
#define LAN_PEER_MAX 64
static struct sockaddr_in lan_peers[LAN_PEER_MAX];
static int  lan_peer_count = 0;

static int sock_fd      = -1;
static int my_node_id   = 1;
static int wire_version = RELAY_VER_V1;   /* upgraded to V2 if key loaded */
static u8  key[KEY_LEN];
static u64 next_nonce   = 0;
static struct sockaddr_in bcast_addr;     /* 255.255.255.255:PORT */

/* G4 inbound-auth state (same policy as net_relay). */
static int relay_strict     = 0;
static u32 mac_drop_count   = 0;
static int v1_permit_warned = 0;

/* Per-source 64-packet sliding replay window (identical to net_relay). */
#define RX_NODE_MAX 256
static u64 rx_nonce_max[RX_NODE_MAX];
static u64 rx_nonce_win[RX_NODE_MAX];
static u8  rx_nonce_armed[RX_NODE_MAX];
static u32 rx_ok_count       = 0;
static u32 replay_drop_count = 0;

/* --- helpers ------------------------------------------------------------ */

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, u8 *out, size_t out_len)
{
    size_t hl = strlen(hex);
    if (hl != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble((unsigned char)hex[i*2]);
        int lo = hex_nibble((unsigned char)hex[i*2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (u8)((hi << 4) | lo);
    }
    return 0;
}

static void store_u64_le(u8 *p, u64 v)
{
    for (int i = 0; i < 8; i++) p[i] = (u8)((v >> (i * 8)) & 0xff);
}

static u64 load_u64_le(const u8 *p)
{
    u64 v = 0;
    for (int i = 0; i < 8; i++) v |= (u64)p[i] << (i * 8);
    return v;
}

static int ct_eq(const u8 *a, const u8 *b, int n)
{
    u8 d = 0;
    for (int i = 0; i < n; i++) d |= (u8)(a[i] ^ b[i]);
    return d == 0;
}

static u64 mint_nonce(void) { return next_nonce++; }

/* Returns 1 if (src,nonce) is fresh (accept), 0 if replay/out-of-window. */
static int rx_replay_ok(unsigned src, u64 nonce)
{
    if (src >= RX_NODE_MAX) return 0;
    if (!rx_nonce_armed[src]) {
        rx_nonce_max[src]   = nonce;
        rx_nonce_win[src]   = 1;
        rx_nonce_armed[src] = 1;
        return 1;
    }
    if (nonce > rx_nonce_max[src]) {
        u64 shift = nonce - rx_nonce_max[src];
        rx_nonce_win[src] = (shift >= 64) ? 0 : (rx_nonce_win[src] << shift);
        rx_nonce_win[src] |= 1;
        rx_nonce_max[src]  = nonce;
        return 1;
    }
    u64 diff = rx_nonce_max[src] - nonce;
    if (diff >= 64) return 0;
    u64 bit = 1ULL << diff;
    if (rx_nonce_win[src] & bit) return 0;
    rx_nonce_win[src] |= bit;
    return 1;
}

/* HMAC-SHA256 over (ver, type, src, dst, nonce, payload), truncated 16 B.
 * Byte-identical recipe to net_relay.c::compute_mac so a PSK-sharing fleet
 * can mix relay and LAN transports if ever needed. */
static void compute_mac(unsigned version, unsigned type, unsigned src,
                        unsigned dst, u64 nonce,
                        const void *payload, int plen,
                        u8 out[HMAC_TRUNC_LEN])
{
    u8 preamble[12];
    preamble[0] = (u8)version;
    preamble[1] = (u8)type;
    preamble[2] = (u8)src;
    preamble[3] = (u8)dst;
    store_u64_le(preamble + 4, nonce);

    u8 k[SHA256_BLOCK_SIZE];
    u8 ipad[SHA256_BLOCK_SIZE], opad[SHA256_BLOCK_SIZE];
    u8 inner[SHA256_DIGEST_SIZE], outer[SHA256_DIGEST_SIZE];
    memcpy(k, key, KEY_LEN);
    memset(k + KEY_LEN, 0, SHA256_BLOCK_SIZE - KEY_LEN);
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, ipad, SHA256_BLOCK_SIZE);
    sha256_update(&c, preamble, sizeof(preamble));
    if (plen > 0) sha256_update(&c, payload, (size_t)plen);
    sha256_final(&c, inner);
    sha256_init(&c);
    sha256_update(&c, opad, SHA256_BLOCK_SIZE);
    sha256_update(&c, inner, SHA256_DIGEST_SIZE);
    sha256_final(&c, outer);
    memcpy(out, outer, HMAC_TRUNC_LEN);
}

/* Build a v1/v2 packet into buf (>= MAX_PKT). Returns total length. */
static int build_packet(unsigned char *buf, unsigned type,
                        unsigned src, unsigned dst,
                        const void *payload, int plen)
{
    if (plen < 0) plen = 0;
    if (plen > MAX_PAYLOAD) plen = MAX_PAYLOAD;

    buf[0] = (u8)(RELAY_MAGIC      & 0xff);
    buf[1] = (u8)((RELAY_MAGIC>>8) & 0xff);
    buf[2] = (u8)((RELAY_MAGIC>>16)& 0xff);
    buf[3] = (u8)((RELAY_MAGIC>>24)& 0xff);
    buf[4] = (u8)wire_version;
    buf[5] = (u8)type;
    buf[6] = (u8)src;
    buf[7] = (u8)dst;
    buf[8] = buf[9] = buf[10] = buf[11] = 0;

    if (wire_version == RELAY_VER_V1) {
        if (payload && plen > 0) memcpy(buf + HEAD_LEN, payload, (size_t)plen);
        return HEAD_LEN + plen;
    }
    u64 nonce = mint_nonce();
    store_u64_le(buf + HEAD_LEN, nonce);
    compute_mac(RELAY_VER_V2, type, src, dst, nonce, payload, plen,
                buf + HEAD_LEN + 8);
    if (payload && plen > 0)
        memcpy(buf + HEAD_LEN + AUTH_LEN, payload, (size_t)plen);
    return HEAD_LEN + AUTH_LEN + plen;
}

/* Learn a peer's source address from an inbound datagram. Dedup by
 * (addr,port); the broadcast source (our own loopback'd bcast) is never
 * a distinct learnable peer because we filter our own src node id below. */
static void learn_peer(const struct sockaddr_in *from)
{
    for (int i = 0; i < lan_peer_count; i++) {
        if (lan_peers[i].sin_addr.s_addr == from->sin_addr.s_addr &&
            lan_peers[i].sin_port        == from->sin_port)
            return;   /* already known */
    }
    if (lan_peer_count >= LAN_PEER_MAX) return;   /* table full — broadcast still reaches it */
    lan_peers[lan_peer_count++] = *from;
}

/* Low-stack one-shot logger (same discipline as net_relay's loggers). */
static void note_v1_permit(void)
{
    if (v1_permit_warned) return;
    v1_permit_warned = 1;
    static const char m[] =
        "[net_lan] WARNING: permissive mode accepted an unauthenticated "
        "v1 frame (set PKERNEL_RELAY_STRICT=1 to drop)\n";
    (void)write(2, m, sizeof(m) - 1);
}

/* Read-only counters for the shell (mirrors net_relay_stats). */
void net_lan_stats(unsigned long *ok, unsigned long *badmac,
                   unsigned long *replay)
{
    if (ok)     *ok     = (unsigned long)rx_ok_count;
    if (badmac) *badmac = (unsigned long)mac_drop_count;
    if (replay) *replay = (unsigned long)replay_drop_count;
}

int net_lan_peer_count(void) { return lan_peer_count; }

/* --- public API --------------------------------------------------------- */

int net_lan_init(void)
{
    const char *env_id   = getenv("PKERNEL_NODE_ID");
    const char *env_key  = getenv("PKERNEL_RELAY_KEY");
    const char *env_port = getenv("PKERNEL_LAN_PORT");

    /* N-0: distinct, stable per-install id when PKERNEL_NODE_ID is unset
     * (two fresh nodes both defaulting to 1 self-echo-filter in SWIM and
     * never mesh). pkernel_default_node_id honours the env override. */
    my_node_id = env_id ? atoi(env_id) : pkernel_default_node_id();
    if (my_node_id < 1 || my_node_id > 255) {
        dprintf(2, "[net_lan] PKERNEL_NODE_ID=%d out of range (1..255) — "
                   "defaulting to 1\n", my_node_id);
        my_node_id = 1;
    } else if (my_node_id > NET_CLUSTER_NODE_MAX) {
        dprintf(2, "[net_lan] WARNING: PKERNEL_NODE_ID=%d exceeds cluster "
                   "DNODE_MAX=%d — this node will mesh on the wire but NOT "
                   "join the drpc/pmesh/kdds cluster\n",
                   my_node_id, NET_CLUSTER_NODE_MAX);
    }

    int port = env_port ? atoi(env_port) : LAN_DEFAULT_PORT;
    if (port <= 0 || port > 65535) port = LAN_DEFAULT_PORT;

    if (env_key && *env_key) {
        if (hex_decode(env_key, key, KEY_LEN) < 0) {
            dprintf(2, "[net_lan] PKERNEL_RELAY_KEY must be %d hex chars\n",
                    KEY_LEN * 2);
            return -1;
        }
        wire_version = RELAY_VER_V2;
    } else {
        wire_version = RELAY_VER_V1;
        dprintf(2, "[net_lan] WARNING: no PKERNEL_RELAY_KEY — using v1 wire "
                   "(LAN peers are unauthenticated)\n");
    }

    {
        const char *env_strict = getenv("PKERNEL_RELAY_STRICT");
        relay_strict = (env_strict && *env_strict && env_strict[0] != '0');
    }

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        dprintf(2, "[net_lan] socket: %s\n", strerror(errno));
        return -1;
    }

    int one = 1;
    /* SO_REUSEADDR so a fast restart can re-bind the port. We do NOT set
     * SO_REUSEPORT: it would load-balance two processes on one host and
     * silently break a same-host two-node test (see CERT note in the PR). */
    (void)setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (setsockopt(sock_fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0) {
        dprintf(2, "[net_lan] SO_BROADCAST: %s\n", strerror(errno));
        close(sock_fd); sock_fd = -1;
        return -1;
    }

    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* 0.0.0.0 */
    addr.sin_port        = htons((unsigned short)port);
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        dprintf(2, "[net_lan] bind 0.0.0.0:%d: %s\n", port, strerror(errno));
        close(sock_fd); sock_fd = -1;
        return -1;
    }

    memset(&bcast_addr, 0, sizeof(bcast_addr));
    bcast_addr.sin_family      = AF_INET;
    bcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);   /* 255.255.255.255 */
    bcast_addr.sin_port        = htons((unsigned short)port);

    lan_peer_count = 0;

    /* Initial nonce: wall clock seconds in the upper bits so a restart
     * always exceeds a peer's stored max_nonce (same as net_relay). */
    next_nonce = ((u64)time(NULL) << 24) | 1;

    dprintf(2, "[net_lan] node %d on 0.0.0.0:%d (wire v%d, LAN-direct, no relay)\n",
            my_node_id, port, wire_version);
    return my_node_id;
}

int net_lan_send(const void *frame, int len)
{
    if (sock_fd < 0 || len <= 0 || len > MAX_PAYLOAD) return -1;

    static unsigned char buf[MAX_PKT];
    int n = build_packet(buf, REL_BROADCAST,
                         (unsigned)my_node_id, 0, frame, len);

    /* 1) Rendezvous broadcast — first contact for peers we haven't learned. */
    (void)sendto(sock_fd, buf, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL,
                 (struct sockaddr *)&bcast_addr, sizeof(bcast_addr));

    /* 2) Direct unicast to every learned peer (works even where broadcast is
     *    filtered once contact is established). */
    for (int i = 0; i < lan_peer_count; i++) {
        (void)sendto(sock_fd, buf, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL,
                     (struct sockaddr *)&lan_peers[i], sizeof(lan_peers[i]));
    }
    return len;
}

int net_lan_recv(void *out, int maxlen)
{
    if (sock_fd < 0) return 0;
    static unsigned char buf[MAX_PKT];

    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(sock_fd, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) return 0;

        if (n < HEAD_LEN) continue;
        if (buf[0] != (u8)(RELAY_MAGIC      & 0xff) ||
            buf[1] != (u8)((RELAY_MAGIC>>8) & 0xff) ||
            buf[2] != (u8)((RELAY_MAGIC>>16)& 0xff) ||
            buf[3] != (u8)((RELAY_MAGIC>>24)& 0xff)) continue;

        unsigned ver  = buf[4];
        unsigned type = buf[5];
        unsigned src  = buf[6];

        /* Drop our own broadcast echoed back (we receive frames we sent to
         * 255.255.255.255 on the same bound port). */
        if (src == (unsigned)my_node_id) continue;

        int hdr = (ver == RELAY_VER_V2) ? (HEAD_LEN + AUTH_LEN) : HEAD_LEN;
        if (n < hdr) continue;

        u64 rx_nonce      = 0;
        int rx_have_nonce = 0;

        if (wire_version == RELAY_VER_V2) {
            if (ver == RELAY_VER_V2) {
                u64 nonce = load_u64_le(buf + HEAD_LEN);
                int plen  = (int)n - hdr;
                u8 want[HMAC_TRUNC_LEN];
                compute_mac(RELAY_VER_V2, buf[5], buf[6], buf[7], nonce,
                            buf + hdr, plen, want);
                if (!ct_eq(want, buf + HEAD_LEN + 8, HMAC_TRUNC_LEN)) {
                    mac_drop_count++;
                    continue;   /* not a PSK-holding peer — never learned */
                }
                rx_nonce      = nonce;
                rx_have_nonce = 1;
            } else {
                if (relay_strict) { mac_drop_count++; continue; }
                note_v1_permit();
            }
        }

        /* Authenticated (or permissively-accepted): LEARN the source so
         * subsequent sends go direct-unicast. */
        learn_peer(&from);

        if (type != REL_DATA && type != REL_BROADCAST) continue;

        if (rx_have_nonce && !rx_replay_ok(src, rx_nonce)) {
            replay_drop_count++;
            continue;
        }

        int payload_len = (int)n - hdr;
        if (payload_len > maxlen) payload_len = maxlen;
        if (payload_len > 0) memcpy(out, buf + hdr, (size_t)payload_len);
        rx_ok_count++;
        return payload_len;
    }
}

int net_lan_node_id(void) { return my_node_id; }
