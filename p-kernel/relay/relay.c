/*
 *  relay/relay.c — UMP relay server (Phase B v2: HMAC + replay window).
 *
 *  See docs/phase_b_relay.md for the wire format and threat model.
 *  In one line: maintain {node_id → (peer_address, last_seen)} and
 *  forward DATA by dst_node; v2 additionally verifies an HMAC-SHA256
 *  over each packet and enforces a 64-packet sliding nonce window per
 *  src to drop replays.
 *
 *  Single UDP socket, default port 7400. No filesystem state. Static
 *  binary suitable for a 1-vCPU VPS.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "sha256.h"

#define RELAY_DEFAULT_PORT 7400
#define RELAY_MAGIC        0x52454C59U   /* "RELY" (little-endian) */
#define RELAY_VERSION_V1   1
#define RELAY_VERSION_V2   2
#define RELAY_HEADER_LEN   12
#define RELAY_AUTH_LEN     24            /* nonce(8) + hmac16(16) */
#define HMAC_TRUNC_LEN     16
#define MAX_PAYLOAD        1380
#define MAX_PKT            (RELAY_HEADER_LEN + RELAY_AUTH_LEN + MAX_PAYLOAD)
#define NODE_MAX           256
#define IDLE_TIMEOUT       300           /* seconds before an entry is evicted */
#define KEY_LEN            32

enum {
    REL_REGISTER  = 1,
    REL_DATA      = 2,
    REL_KEEPALIVE = 3,
    REL_BROADCAST = 4,
};

typedef struct {
    struct sockaddr_in addr;
    time_t  last_seen;
    int     active;
    int     leased;    /* 1 if this id was handed out via auto-lease (src=0) */
} NodeEntry;

typedef struct {
    uint64_t max_nonce;    /* highest nonce ever accepted from this src */
    uint64_t window_bits;  /* bit i = (max - i) has been seen */
    int      armed;        /* 0 until first packet observed */
} ReplayEntry;

static NodeEntry   table[NODE_MAX];
static ReplayEntry replay[NODE_MAX];
static int         verbose  = 0;
static int         insecure = 0;
static uint8_t     key[KEY_LEN];
static int         have_key = 0;
static int         idle_timeout = IDLE_TIMEOUT;  /* overridable via PKERNEL_RELAY_IDLE (testing) */
static uint64_t    grant_nonce  = 1;             /* nonce for relay-originated v2 lease grants */

/* --- helpers ------------------------------------------------------------- */

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    size_t hl = strlen(hex);
    if (hl != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble((unsigned char)hex[i*2]);
        int lo = hex_nibble((unsigned char)hex[i*2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* Constant-time byte compare. Returns 1 if equal. */
static int ct_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static uint64_t load_u64_le(const uint8_t *p)
{
    return  (uint64_t)p[0]
         | ((uint64_t)p[1] << 8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

static void store_u64_le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (i * 8)) & 0xff);
}

/* --- v2 packet parsing + auth ------------------------------------------- */

typedef struct {
    unsigned       version;
    unsigned       type;
    unsigned       src;
    unsigned       dst;
    uint64_t       nonce;       /* v2 only */
    const uint8_t *hmac;        /* v2 only — 16 bytes */
    const uint8_t *payload;
    int            payload_len;
} ParsedPkt;

static int parse_packet(const unsigned char *buf, int len, ParsedPkt *out)
{
    if (len < RELAY_HEADER_LEN) return -1;
    uint32_t magic = (uint32_t)buf[0]
                   | ((uint32_t)buf[1] << 8)
                   | ((uint32_t)buf[2] << 16)
                   | ((uint32_t)buf[3] << 24);
    if (magic != RELAY_MAGIC) return -1;

    out->version = buf[4];
    out->type    = buf[5];
    out->src     = buf[6];
    out->dst     = buf[7];

    if (out->version == RELAY_VERSION_V1) {
        out->nonce       = 0;
        out->hmac        = NULL;
        out->payload     = buf + RELAY_HEADER_LEN;
        out->payload_len = len - RELAY_HEADER_LEN;
        return 0;
    }
    if (out->version == RELAY_VERSION_V2) {
        if (len < RELAY_HEADER_LEN + RELAY_AUTH_LEN) return -1;
        out->nonce       = load_u64_le(buf + RELAY_HEADER_LEN);
        out->hmac        = buf + RELAY_HEADER_LEN + 8;
        out->payload     = buf + RELAY_HEADER_LEN + RELAY_AUTH_LEN;
        out->payload_len = len - RELAY_HEADER_LEN - RELAY_AUTH_LEN;
        return 0;
    }
    return -1;
}

/* Recompute the v2 MAC over (ver, type, src, dst, nonce, payload). */
static void compute_mac(const ParsedPkt *p, uint8_t out[HMAC_TRUNC_LEN])
{
    uint8_t preamble[12];
    preamble[0] = (uint8_t)p->version;
    preamble[1] = (uint8_t)p->type;
    preamble[2] = (uint8_t)p->src;
    preamble[3] = (uint8_t)p->dst;
    for (int i = 0; i < 8; i++) {
        preamble[4 + i] = (uint8_t)((p->nonce >> (i * 8)) & 0xff);
    }
    sha256_ctx c;
    /* Manually drive HMAC so we can stream preamble + payload separately. */
    uint8_t k[SHA256_BLOCK_SIZE];
    uint8_t ipad[SHA256_BLOCK_SIZE];
    uint8_t opad[SHA256_BLOCK_SIZE];
    uint8_t inner[SHA256_DIGEST_SIZE];
    uint8_t outer[SHA256_DIGEST_SIZE];

    memcpy(k, key, KEY_LEN);
    memset(k + KEY_LEN, 0, SHA256_BLOCK_SIZE - KEY_LEN);
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    sha256_init(&c);
    sha256_update(&c, ipad, SHA256_BLOCK_SIZE);
    sha256_update(&c, preamble, sizeof(preamble));
    if (p->payload_len > 0) sha256_update(&c, p->payload, (size_t)p->payload_len);
    sha256_final(&c, inner);
    sha256_init(&c);
    sha256_update(&c, opad, SHA256_BLOCK_SIZE);
    sha256_update(&c, inner, SHA256_DIGEST_SIZE);
    sha256_final(&c, outer);

    memcpy(out, outer, HMAC_TRUNC_LEN);
}

/* Returns 1 if MAC verifies. */
static int verify_mac(const ParsedPkt *p)
{
    uint8_t want[HMAC_TRUNC_LEN];
    compute_mac(p, want);
    return ct_eq(want, p->hmac, HMAC_TRUNC_LEN);
}

/* --- replay window ------------------------------------------------------- */

/* Returns 1 if nonce is fresh (accept), 0 if replay (drop). */
static int replay_check_and_update(unsigned src, uint64_t nonce)
{
    if (src < 1 || src >= NODE_MAX) {
        /* G7: surface the reason instead of a silent drop. */
        if (verbose) fprintf(stderr,
            "[relay] drop: replay-check src node id %u out of range "
            "(valid 1..%d)\n", src, NODE_MAX - 1);
        return 0;
    }
    ReplayEntry *r = &replay[src];
    if (!r->armed) {
        r->max_nonce   = nonce;
        r->window_bits = 1;   /* bit 0 = current nonce */
        r->armed       = 1;
        return 1;
    }
    if (nonce > r->max_nonce) {
        uint64_t shift = nonce - r->max_nonce;
        r->window_bits = (shift >= 64) ? 0 : (r->window_bits << shift);
        r->window_bits |= 1;
        r->max_nonce    = nonce;
        return 1;
    }
    uint64_t diff = r->max_nonce - nonce;
    if (diff >= 64) return 0;  /* outside window */
    uint64_t bit = 1ULL << diff;
    if (r->window_bits & bit) return 0;
    r->window_bits |= bit;
    return 1;
}

/* --- table maintenance -------------------------------------------------- */

static void update(int src, const struct sockaddr_in *from)
{
    if (src < 1 || src >= NODE_MAX) {
        /* G7: don't drop an out-of-range src silently — say why. */
        if (verbose) fprintf(stderr,
            "[relay] drop: REGISTER/DATA src node id %d out of range "
            "(valid 1..%d)\n", src, NODE_MAX - 1);
        return;
    }
    int was_active = table[src].active;
    table[src].addr      = *from;
    table[src].last_seen = time(NULL);
    table[src].active    = 1;
    if (!was_active) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from->sin_addr, ip, sizeof(ip));
        fprintf(stderr, "[relay] node %d registered: %s:%u\n",
                src, ip, ntohs(from->sin_port));
    }
}

static void evict_stale(time_t now)
{
    for (int n = 1; n < NODE_MAX; n++) {
        if (table[n].active && now - table[n].last_seen > idle_timeout) {
            table[n].active = 0;
            fprintf(stderr, "[relay] node %d evicted (idle)\n", n);
            /* NOTE: replay[n] is intentionally preserved across eviction
             *       so an attacker can't clear the window by waiting. */
        }
    }
}

/* --- dynamic node-id lease (src=0 auto-REGISTER) ------------------------ */
/*
 *  A client that does not have a human-assigned PKERNEL_NODE_ID sends a
 *  REGISTER with src=0 — a value that was always invalid (and silently
 *  dropped) in v1/v2, so no existing client uses it. The relay picks the
 *  smallest free node id, claims that table slot for the requester, and
 *  replies with a REGISTER grant carrying the leased id in the dst field
 *  (src=0 marks the packet as "this is a grant, not a peer's REGISTER").
 *  For v2 the grant is HMAC-signed so the client can trust the id.
 *
 *  Scope (this wave): single relay. Lease release is by the same idle
 *  eviction as static nodes — when a leased slot goes idle it is freed
 *  and its id becomes reusable. Multi-relay lease arbitration is design
 *  only; see docs/architecture/dynamic-id.md.
 */

static int addr_eq(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr
        && a->sin_port        == b->sin_port;
}

/* Pick an id for an auto-REGISTER from `from`. Idempotent: if this exact
 * address already holds a lease, return the same id (so a retransmitted
 * or replayed auto-REGISTER never consumes a second id). Otherwise the
 * smallest free slot. Returns 0 if the pool (1..NODE_MAX-1) is full. */
static int lease_pick(const struct sockaddr_in *from)
{
    for (int n = 1; n < NODE_MAX; n++) {
        if (table[n].active && table[n].leased && addr_eq(&table[n].addr, from))
            return n;
    }
    for (int n = 1; n < NODE_MAX; n++) {
        if (!table[n].active) return n;
    }
    return 0;
}

/* Build + send a REGISTER grant (src=0, dst=leased_id) back to `to`.
 * leased_id==0 signals lease denied (pool exhausted). Matches request
 * version so v1 (--insecure) clients get a v1 grant and v2 clients a
 * signed v2 grant. */
static void send_lease_grant(int sock, const struct sockaddr_in *to,
                             unsigned version, int leased_id)
{
    unsigned char out[RELAY_HEADER_LEN + RELAY_AUTH_LEN];
    out[0] = (uint8_t)(RELAY_MAGIC        & 0xff);
    out[1] = (uint8_t)((RELAY_MAGIC >> 8) & 0xff);
    out[2] = (uint8_t)((RELAY_MAGIC >> 16)& 0xff);
    out[3] = (uint8_t)((RELAY_MAGIC >> 24)& 0xff);
    out[4] = (uint8_t)version;
    out[5] = REL_REGISTER;
    out[6] = 0;                       /* src=0 → lease grant */
    out[7] = (uint8_t)leased_id;      /* dst = the id we are granting */
    out[8] = out[9] = out[10] = out[11] = 0;

    int len = RELAY_HEADER_LEN;
    if (version == RELAY_VERSION_V2) {
        uint64_t nonce = grant_nonce++;
        store_u64_le(out + RELAY_HEADER_LEN, nonce);
        ParsedPkt gp = {
            .version = version, .type = REL_REGISTER,
            .src = 0, .dst = (unsigned)leased_id, .nonce = nonce,
            .hmac = NULL, .payload = NULL, .payload_len = 0,
        };
        compute_mac(&gp, out + RELAY_HEADER_LEN + 8);
        len = RELAY_HEADER_LEN + RELAY_AUTH_LEN;
    }
    if (sendto(sock, out, (size_t)len, 0,
               (struct sockaddr *)to, sizeof(*to)) < 0 && verbose) {
        fprintf(stderr, "[relay] lease grant to %u: %s\n",
                (unsigned)leased_id, strerror(errno));
    }
}

static void handle_lease(int sock, const struct sockaddr_in *from,
                         unsigned version)
{
    int id = lease_pick(from);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from->sin_addr, ip, sizeof(ip));
    if (id == 0) {
        fprintf(stderr, "[relay] lease denied: id pool 1..%d exhausted (req %s:%u)\n",
                NODE_MAX - 1, ip, ntohs(from->sin_port));
        send_lease_grant(sock, from, version, 0);
        return;
    }
    int reissue = table[id].active && table[id].leased && addr_eq(&table[id].addr, from);
    table[id].addr      = *from;
    table[id].last_seen = time(NULL);
    table[id].active    = 1;
    table[id].leased    = 1;
    fprintf(stderr, "[relay] node %d %s to %s:%u\n",
            id, reissue ? "lease re-issued" : "leased (auto)",
            ip, ntohs(from->sin_port));
    send_lease_grant(sock, from, version, id);
}

/* --- forwarding --------------------------------------------------------- */

static void forward(int sock, int dst_node,
                    const unsigned char *buf, int len, time_t now)
{
    if (dst_node < 1 || dst_node >= NODE_MAX) {
        /* G7: an out-of-range dst is a routing dead-end — say why. */
        if (verbose) fprintf(stderr,
            "[relay] drop: dst node id %d out of range (valid 1..%d)\n",
            dst_node, NODE_MAX - 1);
        return;
    }
    if (!table[dst_node].active) {
        if (verbose) fprintf(stderr, "[relay] no route to dst=%d\n", dst_node);
        return;
    }
    if (now - table[dst_node].last_seen > idle_timeout) {
        table[dst_node].active = 0;
        return;
    }
    ssize_t sent = sendto(sock, buf, (size_t)len, 0,
                          (struct sockaddr *)&table[dst_node].addr,
                          sizeof(table[dst_node].addr));
    if (sent < 0 && verbose) {
        fprintf(stderr, "[relay] sendto node %d: %s\n", dst_node, strerror(errno));
    }
}

/* --- main loop ---------------------------------------------------------- */

static volatile sig_atomic_t stop = 0;
static void on_signal(int s) { (void)s; stop = 1; }

static void load_key_or_die(void)
{
    const char *hex = getenv("PKERNEL_RELAY_KEY");
    if (!hex || !*hex) {
        if (insecure) {
            fprintf(stderr, "[relay] WARNING: --insecure mode, no key, "
                            "v1 packets accepted, no auth, no replay protection\n");
            return;
        }
        fprintf(stderr,
            "[relay] PKERNEL_RELAY_KEY not set — refusing to start.\n"
            "        Set a 64-char hex key (32 bytes), or pass --insecure\n"
            "        for v1-compatible no-auth mode.\n");
        exit(2);
    }
    if (hex_decode(hex, key, KEY_LEN) < 0) {
        fprintf(stderr, "[relay] PKERNEL_RELAY_KEY must be exactly %d hex chars (got %zu)\n",
                KEY_LEN * 2, strlen(hex));
        exit(2);
    }
    have_key = 1;
    fprintf(stderr, "[relay] key loaded (%d bytes)\n", KEY_LEN);
}

int main(int argc, char **argv)
{
    int port = RELAY_DEFAULT_PORT;

    static struct option longopts[] = {
        {"insecure", no_argument, NULL, 'I'},
        {"port",     required_argument, NULL, 'p'},
        {"verbose",  no_argument, NULL, 'v'},
        {"help",     no_argument, NULL, 'h'},
        {0,0,0,0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "p:vh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'v': verbose = 1; break;
        case 'I': insecure = 1; break;
        case 'h':
        default:
            fprintf(stderr, "usage: %s [-p port] [-v] [--insecure]\n", argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (sha256_self_test() != 0) {
        fprintf(stderr, "[relay] FATAL: SHA-256 self-test failed\n");
        return 3;
    }

    load_key_or_die();

    /* Optional override of the idle-eviction window (seconds). Keeps the
     * 300 s default for production; the dynamic-id test sets it low so it
     * can observe a leased id being reclaimed and reused without waiting. */
    {
        const char *idle_env = getenv("PKERNEL_RELAY_IDLE");
        int v = idle_env ? atoi(idle_env) : 0;
        if (v > 0) {
            idle_timeout = v;
            fprintf(stderr, "[relay] idle timeout overridden: %d s\n", idle_timeout);
        }
    }

    /* sigaction without SA_RESTART so recvfrom() returns EINTR on
     * SIGTERM/SIGINT — signal() defaults to SA_RESTART on glibc. */
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }

    fprintf(stderr, "[relay] listening on 0.0.0.0:%d (verbose=%d, insecure=%d)\n",
            port, verbose, insecure);

    unsigned char buf[MAX_PKT];
    while (!stop) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &flen);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            continue;
        }

        ParsedPkt pkt;
        if (parse_packet(buf, (int)n, &pkt) < 0) {
            if (verbose) fprintf(stderr, "[relay] drop: bad header (%zd B)\n", n);
            continue;
        }

        /* Version gating. v1 only accepted in --insecure mode. */
        if (pkt.version == RELAY_VERSION_V1 && !insecure) {
            if (verbose) fprintf(stderr, "[relay] drop: v1 packet in secure mode\n");
            continue;
        }
        if (pkt.version == RELAY_VERSION_V2 && !have_key) {
            if (verbose) fprintf(stderr, "[relay] drop: v2 packet but no key loaded\n");
            continue;
        }

        /* v2: verify MAC, then check replay window. Order matters —
         * an unauthenticated nonce must NOT affect replay state. */
        if (pkt.version == RELAY_VERSION_V2) {
            if (!verify_mac(&pkt)) {
                if (verbose) fprintf(stderr, "[relay] drop: bad HMAC src=%u type=%u\n",
                                     pkt.src, pkt.type);
                continue;
            }
            /* An auto-REGISTER (src=0) has no per-node replay state yet —
             * it's authenticated by HMAC above and deduped by source
             * address in handle_lease(), so it bypasses the src-keyed
             * replay window (which would otherwise drop src=0 outright). */
            if (!(pkt.type == REL_REGISTER && pkt.src == 0)) {
                if (!replay_check_and_update(pkt.src, pkt.nonce)) {
                    if (verbose) fprintf(stderr, "[relay] drop: replay src=%u nonce=%llu\n",
                                         pkt.src, (unsigned long long)pkt.nonce);
                    continue;
                }
            }
        }

        time_t now = time(NULL);
        evict_stale(now);

        if (verbose) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            fprintf(stderr, "[relay] rx v%u type=%u src=%u dst=%u from %s:%u (%zd B)\n",
                    pkt.version, pkt.type, pkt.src, pkt.dst,
                    ip, ntohs(from.sin_port), n);
        }

        switch (pkt.type) {
        case REL_REGISTER:
            if (pkt.src == 0) {
                /* Auto-lease request: hand out a dynamic node id. */
                handle_lease(sock, &from, pkt.version);
            } else {
                update((int)pkt.src, &from);
            }
            break;

        case REL_KEEPALIVE:
            update((int)pkt.src, &from);
            /* Relay-HA liveness pong: reflect the (already verified)
             * keepalive back to its sender so clients can distinguish
             * "relay alive" from "relay dead" and run deterministic
             * failover/failback (docs/architecture/relay-ha.md). The
             * wire format is untouched — this echoes the same packet. */
            if (sendto(sock, buf, (size_t)n, 0,
                       (struct sockaddr *)&from, flen) < 0 && verbose) {
                fprintf(stderr, "[relay] keepalive echo to src=%u: %s\n",
                        pkt.src, strerror(errno));
            }
            break;

        case REL_DATA:
            update((int)pkt.src, &from);
            forward(sock, (int)pkt.dst, buf, (int)n, now);
            break;

        case REL_BROADCAST:
            update((int)pkt.src, &from);
            for (int n_id = 1; n_id < NODE_MAX; n_id++) {
                if (n_id == (int)pkt.src) continue;
                if (!table[n_id].active)   continue;
                forward(sock, n_id, buf, (int)n, now);
            }
            break;

        default:
            if (verbose) fprintf(stderr, "[relay] drop: unknown type %u\n", pkt.type);
        }
    }

    fprintf(stderr, "[relay] shutdown\n");
    close(sock);
    return 0;
}
