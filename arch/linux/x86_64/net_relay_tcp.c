/*
 *  arch/linux/x86_64/net_relay_tcp.c
 *
 *  connect-anywhere SLICE 3 — PLAIN-TCP relay fallback backend.
 *
 *  A sibling of net_relay.c that joins the SAME relay mesh over a TCP
 *  stream, for networks that block UDP entirely (corporate/cafe/some
 *  carriers). It speaks the IDENTICAL Phase B v2 wire (magic/ver/type/
 *  src/dst/nonce/HMAC — see docs/phase_b_relay.md) reused VERBATIM; the
 *  ONLY addition is a 2-byte big-endian length prefix per packet on the
 *  stream (relay/tcp_frame.h), because TCP has no datagram boundaries and
 *  a naive socat tunnel mis-splits concatenated frames.
 *
 *  Exposes the same 4 symbols net_dispatch.c dispatches through:
 *    net_relay_tcp_init / _send / _recv / _node_id
 *  selected by PKERNEL_RELAY_TCP=1 (net_dispatch.c). When unset, behaviour
 *  is exactly as today (UDP net_relay backend).
 *
 *  Scope of THIS slice: a single TCP relay endpoint (the first entry of
 *  PKERNEL_RELAY, or PKERNEL_RELAY_HOST/PORT, or the first PKERNEL_SEED).
 *  Multi-relay HA over TCP and the §4 happy-eyeballs ladder are later work.
 *  NO TLS, NO 443 here — that is a later sub-slice.
 *
 *  HOSTED-ONLY: never linked into a bare-metal image (crown-safe by
 *  construction). T-Kernel headers are NOT included (same rule as
 *  net_unix.c / net_relay.c); we avoid <stdint.h> so this TU coexists with
 *  the T-Kernel stdint shadow it is built under.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
/* NOT <netinet/tcp.h>: on glibc it includes <stdint.h>, which (with -Iinclude)
 * resolves to the T-Kernel stdint shadow and conflicts with the glibc int64_t
 * already pulled by <stdlib.h>. We need only two universal constants from it. */
#ifndef IPPROTO_TCP
#define IPPROTO_TCP  6
#endif
#ifndef TCP_NODELAY
#define TCP_NODELAY  1
#endif
#include <arpa/inet.h>

#include "sha256.h"      /* same canonical relay/ HMAC-SHA256 as net_relay.c */
#include "tcp_frame.h"   /* the shared [u16 len][pkt] de-framer (load-bearing) */

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* errno shim (T-Kernel placeholder shadows the system one). */
extern int *__errno_location(void) __attribute__((__const__));
#define errno (*__errno_location())
extern char *strerror(int);

/* The T-Kernel build shadows <errno.h>, so the few errno constants this TU
 * needs are defined locally (asm-generic values, identical on aarch64 and
 * x86_64) — the same pattern as galaxy_posix.c (EAGAIN) / selfc_proc.c (EINTR). */
#ifndef EINTR
#define EINTR        4
#endif
#ifndef EAGAIN
#define EAGAIN       11
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK  EAGAIN
#endif
#ifndef EINPROGRESS
#define EINPROGRESS  115
#endif

/* N-0: stable, distinct per-install default id (arch/linux/node_id.c). */
extern int pkernel_default_node_id(void);

#define RELAY_MAGIC        0x52454C59U   /* "RELY" little-endian */
#define RELAY_VER_V1       1
#define RELAY_VER_V2       2
#define HEAD_LEN           12
#define AUTH_LEN           24            /* nonce(8) + hmac16(16) */
#define HMAC_TRUNC_LEN     16
#define KEY_LEN            32
#define MAX_PAYLOAD        1380
#define MAX_PKT            (HEAD_LEN + AUTH_LEN + MAX_PAYLOAD)
#define DEFAULT_PORT       7400
#define KEEPALIVE_SEC      15            /* mirror net_relay.c (slice 1)       */
#define CONNECT_TMO_MS     3000          /* bounded TCP connect wait           */

#define REL_REGISTER       1
#define REL_DATA           2
#define REL_KEEPALIVE      3
#define REL_BROADCAST      4

static int tcp_fd        = -1;
static int my_node_id    = 1;
static int wire_version  = RELAY_VER_V1;   /* upgraded to V2 if key loaded */
static u8  key[KEY_LEN];
static struct sockaddr_in relay_addr;
static u64    next_nonce   = 0;
static time_t last_send_ts = 0;

/* Per-connection reassembler for the single relay stream. STATIC — never on
 * a task stack (see feedback_hosted_relay_stack_overflow). */
static TcpReasm rx_reasm;

/* Receive-side replay window, identical logic to net_relay.c. */
#define RX_NODE_MAX 256
static u64 rx_nonce_max[RX_NODE_MAX];
static u64 rx_nonce_win[RX_NODE_MAX];
static u8  rx_nonce_armed[RX_NODE_MAX];

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

/* --- helpers (reused VERBATIM from net_relay.c) ------------------------- */

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

/* HMAC-SHA256 over (ver, type, src, dst, nonce, payload), truncated to 16 B.
 * Byte-identical to net_relay.c / relay.c so a TCP node and a UDP node share
 * one authenticated wire through the relay. */
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

/* Build a v1/v2 packet into buf. Returns total length (the v2 packet body,
 * WITHOUT the TCP length prefix — the prefix is added at write time). */
static int build_packet(unsigned char *buf,
                        unsigned type, unsigned src, unsigned dst,
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
    if (payload && plen > 0) {
        memcpy(buf + HEAD_LEN + AUTH_LEN, payload, (size_t)plen);
    }
    return HEAD_LEN + AUTH_LEN + plen;
}

/* Resolve a NUMERIC-ONLY host:port into *out (same crash-safe contract as
 * net_relay.c's resolve_relay — no in-task getaddrinfo). 0 on success. */
static int resolve_numeric(const char *host, int port, struct sockaddr_in *out)
{
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) return 0;
    dprintf(2, "[net_relay_tcp] unresolvable host '%s' (numeric IP required) "
               "— skipping\n", host);
    return -1;
}

/* Pick the relay endpoint: first entry of PKERNEL_RELAY / PKERNEL_SEED, else
 * PKERNEL_RELAY_HOST[:PKERNEL_RELAY_PORT]. Returns 0 on success. */
static int pick_endpoint(struct sockaddr_in *out)
{
    const char *list = getenv("PKERNEL_RELAY");
    const char *seed = getenv("PKERNEL_SEED");
    const char *host = getenv("PKERNEL_RELAY_HOST");
    const char *port_s = getenv("PKERNEL_RELAY_PORT");

    const char *spec = NULL;
    if (seed && *seed)      spec = seed;
    else if (list && *list) spec = list;

    if (spec) {
        char work[256];
        size_t sl = strlen(spec);
        if (sl == 0 || sl >= sizeof(work)) return -1;
        memcpy(work, spec, sl + 1);
        /* first comma-separated entry only (single TCP endpoint this slice) */
        char *comma = strchr(work, ',');
        if (comma) *comma = '\0';
        char *tok = work;
        while (*tok == ' ') tok++;
        int port = DEFAULT_PORT;
        char *colon = strrchr(tok, ':');
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
            if (port <= 0 || port > 65535) port = DEFAULT_PORT;
        }
        if (!*tok) return -1;
        return resolve_numeric(tok, port, out);
    }
    if (host && *host) {
        int port = port_s ? atoi(port_s) : DEFAULT_PORT;
        if (port <= 0 || port > 65535) port = DEFAULT_PORT;
        return resolve_numeric(host, port, out);
    }
    dprintf(2, "[net_relay_tcp] no PKERNEL_RELAY/SEED/HOST set\n");
    return -1;
}

/* Write all `len` bytes of buf to the (non-blocking) TCP socket, flushing
 * partial writes with a bounded POLLOUT wait. Returns 0 on success, -1 on a
 * hard error / disconnect. */
static int write_all(const unsigned char *buf, int len)
{
    int off = 0;
    while (off < len) {
        ssize_t w = send(tcp_fd, buf + off, (size_t)(len - off), MSG_NOSIGNAL);
        if (w > 0) { off += (int)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { .fd = tcp_fd, .events = POLLOUT, .revents = 0 };
            if (poll(&p, 1, 1000) <= 0) return -1;
            continue;
        }
        return -1;   /* EPIPE / ECONNRESET / etc. */
    }
    return 0;
}

/* Frame one v2 packet ([u16 len][pkt]) and write it. */
static void send_framed(const unsigned char *pkt, int pktlen)
{
    if (tcp_fd < 0) return;
    static unsigned char out[TCP_FRAME_LENPFX + MAX_PKT];
    int total = tcp_frame_encode(out, pkt, pktlen);
    if (total < 0) return;
    if (write_all(out, total) < 0) {
        dprintf(2, "[net_relay_tcp] stream write failed (%s) — closing\n",
                strerror(errno));
        close(tcp_fd);
        tcp_fd = -1;
    }
}

static void send_register(void)
{
    static unsigned char buf[MAX_PKT];
    int n = build_packet(buf, REL_REGISTER, (unsigned)my_node_id, 0, NULL, 0);
    send_framed(buf, n);
    last_send_ts = time(NULL);
}

static void send_keepalive(void)
{
    static unsigned char buf[MAX_PKT];
    int n = build_packet(buf, REL_KEEPALIVE, (unsigned)my_node_id, 0, NULL, 0);
    send_framed(buf, n);
    last_send_ts = time(NULL);
}

/* Hold the TCP/proxy/NAT stream state open with a steady keepalive frame,
 * the TCP analogue of slice 1's UDP heartbeat. Cheap; runs off send/recv. */
static void tcp_tick(void)
{
    if (tcp_fd < 0) return;
    if (time(NULL) - last_send_ts >= KEEPALIVE_SEC) send_keepalive();
}

/* --- public API --------------------------------------------------------- */

int net_relay_tcp_init(void)
{
    const char *env_id  = getenv("PKERNEL_NODE_ID");
    const char *env_key = getenv("PKERNEL_RELAY_KEY");

    my_node_id = env_id ? atoi(env_id) : pkernel_default_node_id();
    if (my_node_id < 1 || my_node_id > 255) {
        dprintf(2, "[net_relay_tcp] PKERNEL_NODE_ID=%d out of range (1..255) "
                   "— defaulting to 1\n", my_node_id);
        my_node_id = 1;
    }

    if (env_key && *env_key) {
        if (hex_decode(env_key, key, KEY_LEN) < 0) {
            dprintf(2, "[net_relay_tcp] PKERNEL_RELAY_KEY must be %d hex chars\n",
                    KEY_LEN * 2);
            return -1;
        }
        wire_version = RELAY_VER_V2;
    } else {
        wire_version = RELAY_VER_V1;
        dprintf(2, "[net_relay_tcp] WARNING: no PKERNEL_RELAY_KEY — using v1 "
                   "wire (relay must run with --insecure)\n");
    }

    if (pick_endpoint(&relay_addr) < 0) return -1;

    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) {
        dprintf(2, "[net_relay_tcp] socket: %s\n", strerror(errno));
        return -1;
    }
    /* TCP_NODELAY: our frames are small and latency-sensitive. */
    { int one = 1; setsockopt(tcp_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }

    /* Non-blocking connect with a bounded POLLOUT wait so a dead relay never
     * hangs the task indefinitely. */
    int flags = fcntl(tcp_fd, F_GETFL, 0);
    fcntl(tcp_fd, F_SETFL, flags | O_NONBLOCK);
    int cr = connect(tcp_fd, (struct sockaddr *)&relay_addr, sizeof(relay_addr));
    if (cr < 0 && errno == EINPROGRESS) {
        struct pollfd p = { .fd = tcp_fd, .events = POLLOUT, .revents = 0 };
        int pr = poll(&p, 1, CONNECT_TMO_MS);
        if (pr <= 0) {
            dprintf(2, "[net_relay_tcp] connect timed out\n");
            close(tcp_fd); tcp_fd = -1; return -1;
        }
        int soerr = 0; socklen_t sl = sizeof(soerr);
        getsockopt(tcp_fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
        if (soerr != 0) {
            dprintf(2, "[net_relay_tcp] connect failed: %s\n", strerror(soerr));
            close(tcp_fd); tcp_fd = -1; return -1;
        }
    } else if (cr < 0) {
        dprintf(2, "[net_relay_tcp] connect: %s\n", strerror(errno));
        close(tcp_fd); tcp_fd = -1; return -1;
    }

    tcp_reasm_init(&rx_reasm);

    /* Initial nonce: wall-clock seconds in the upper bits so a restart always
     * exceeds the relay's stored max_nonce (same contract as net_relay.c). */
    next_nonce = ((u64)time(NULL) << 24) | 1;

    {
        char ipbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &relay_addr.sin_addr, ipbuf, sizeof(ipbuf));
        dprintf(2, "[net_relay_tcp] node %d -> %s:%d (TCP, wire v%d)\n",
                my_node_id, ipbuf, (int)ntohs(relay_addr.sin_port),
                wire_version);
    }

    send_register();
    return my_node_id;
}

int net_relay_tcp_send(const void *frame, int len)
{
    if (tcp_fd < 0 || len <= 0 || len > MAX_PAYLOAD) return -1;
    tcp_tick();
    static unsigned char buf[MAX_PKT];
    int n = build_packet(buf, REL_BROADCAST, (unsigned)my_node_id, 0, frame, len);
    send_framed(buf, n);
    return len;
}

int net_relay_tcp_recv(void *out, int maxlen)
{
    if (tcp_fd < 0) return 0;
    tcp_tick();

    static unsigned char pkt[MAX_PKT];
    static unsigned char chunk[2048];

    for (;;) {
        /* 1. Try to pop a complete frame already buffered. */
        int flen = tcp_reasm_next(&rx_reasm, pkt, (int)sizeof(pkt));
        if (flen < 0) {
            /* Framed length exceeds our packet buffer — corrupt/hostile peer.
             * Drop the stream rather than desync forever. */
            dprintf(2, "[net_relay_tcp] oversized frame — closing stream\n");
            close(tcp_fd); tcp_fd = -1; return 0;
        }
        if (flen == 0) {
            /* 2. Need more bytes: non-blocking read of the next chunk. */
            ssize_t n = recv(tcp_fd, chunk, sizeof(chunk), MSG_DONTWAIT);
            if (n == 0) {                       /* peer closed the stream */
                dprintf(2, "[net_relay_tcp] relay closed stream\n");
                close(tcp_fd); tcp_fd = -1; return 0;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                if (errno == EINTR) continue;
                dprintf(2, "[net_relay_tcp] recv: %s — closing\n", strerror(errno));
                close(tcp_fd); tcp_fd = -1; return 0;
            }
            tcp_reasm_push(&rx_reasm, chunk, (int)n);
            continue;   /* loop back and try to pop a frame */
        }

        /* 3. We have one complete v2 packet in pkt[0..flen). Parse it the
         *    same way net_relay_recv does. */
        if (flen < HEAD_LEN) continue;
        if (pkt[0] != (u8)(RELAY_MAGIC      & 0xff) ||
            pkt[1] != (u8)((RELAY_MAGIC>>8) & 0xff) ||
            pkt[2] != (u8)((RELAY_MAGIC>>16)& 0xff) ||
            pkt[3] != (u8)((RELAY_MAGIC>>24)& 0xff)) continue;

        unsigned ver  = pkt[4];
        unsigned type = pkt[5];
        int hdr = (ver == RELAY_VER_V2) ? (HEAD_LEN + AUTH_LEN) : HEAD_LEN;
        if (flen < hdr) continue;

        u64 rx_nonce = 0;
        int rx_have_nonce = 0;

        if (wire_version == RELAY_VER_V2 && ver == RELAY_VER_V2) {
            u64 nonce = load_u64_le(pkt + HEAD_LEN);
            int plen  = flen - hdr;
            u8 want[HMAC_TRUNC_LEN];
            compute_mac(RELAY_VER_V2, pkt[5], pkt[6], pkt[7], nonce,
                        pkt + hdr, plen, want);
            if (!ct_eq(want, pkt + HEAD_LEN + 8, HMAC_TRUNC_LEN)) continue;
            rx_nonce = nonce;
            rx_have_nonce = 1;
        }

        /* Control frames (keepalive echoes, stray REGISTERs) never carry data
         * and must not consume a data nonce slot. */
        if (type != REL_DATA && type != REL_BROADCAST) continue;

        if (rx_have_nonce && !rx_replay_ok(pkt[6], rx_nonce)) continue;

        int payload_len = flen - hdr;
        if (payload_len > maxlen) payload_len = maxlen;
        if (payload_len > 0) memcpy(out, pkt + hdr, (size_t)payload_len);
        return payload_len;
    }
}

int net_relay_tcp_node_id(void) { return my_node_id; }
