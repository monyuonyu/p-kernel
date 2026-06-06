/*
 *  arch/linux/aarch64/net_relay.c
 *
 *  POSIX-only relay-transport backend for the rtl8139 driver shim,
 *  paired with arch/common's relay v2 wire protocol (see
 *  docs/phase_b_relay.md). Wraps each outbound frame from the
 *  netstack as a RelayPacket{BROADCAST} addressed to no specific
 *  peer; the relay fans out to every registered node. Inbound
 *  packets have their 12+24 byte header stripped before the payload
 *  is returned to the rtl8139 shim.
 *
 *  Selected at runtime by net_dispatch.c when PKERNEL_RELAY (or the
 *  legacy PKERNEL_RELAY_HOST) is set. Wire version (v2 with HMAC, or
 *  v1 plaintext) is chosen by whether PKERNEL_RELAY_KEY is set —
 *  useful for both production (key required) and bring-up against an
 *  --insecure relay.
 *
 *  Relay HA (multi-relay failover, see docs/architecture/relay-ha.md):
 *  PKERNEL_RELAY takes an ordered, comma-separated list of up to 4
 *  relay endpoints sharing one PSK. Every node holds the SAME list in
 *  the SAME order and follows one deterministic rule: "use the first
 *  relay on the list that is alive". Liveness is probed with the
 *  existing REL_KEEPALIVE packet, which the relay echoes back (pong).
 *  No coordinator is needed — because the rule is a pure function of
 *  (shared list, per-relay liveness), all nodes converge on the same
 *  relay; periodic probing of higher-priority relays pulls everyone
 *  back to the list head when it recovers.
 *
 *  Env vars:
 *    PKERNEL_NODE_ID      our node_id (1..255), default 1
 *    PKERNEL_RELAY        "host[:port],host[:port][,...]" (max 4)
 *    PKERNEL_RELAY_HOST   legacy single relay host (used when
 *                         PKERNEL_RELAY is not set)
 *    PKERNEL_RELAY_PORT   legacy single relay port, default 7400
 *    PKERNEL_RELAY_KEY    32 bytes as 64 hex chars; if absent, v1 wire
 *
 *  T-Kernel headers are NOT included here (same rule as net_unix.c).
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
#include <netdb.h>

#include "sha256.h"   /* from relay/, added to include path by Makefile.
                       * NOTE: sha256.h deliberately avoids <stdint.h> so it
                       * coexists with the T-Kernel stdint shadow that this
                       * TU is built under. We use plain `unsigned char` /
                       * `unsigned long long` here for the same reason. */

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* errno shim (T-Kernel placeholder shadows the system one). */
extern int *__errno_location(void) __attribute__((__const__));
#define errno (*__errno_location())
extern char *strerror(int);

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
#define KEEPALIVE_SEC      25            /* < IDLE_TIMEOUT/2 in relay */
/* G7: the relay wire allows node ids 1..255, but the kernel cluster (drpc)
 * only tracks 1..DNODE_MAX. Mirror DNODE_MAX here (T-Kernel headers are not
 * included in this TU); a node id above this registers with the relay but
 * never joins drpc/pmesh/kdds, so we warn at init instead of vanishing. */
#define NET_CLUSTER_NODE_MAX 32

/* Relay-HA failover/failback time constants (ms). All nodes share these,
 * so the "first live relay on the list" rule resolves identically
 * everywhere. See docs/architecture/relay-ha.md. */
#define MAX_RELAYS         4
#define HA_RX_IDLE_MS      5000   /* no rx for this long -> probe current */
#define HA_PROBE_TMO_MS    3000   /* probe unanswered -> advance to next  */
#define HA_FAILBACK_MS    10000   /* period for probing higher-prio relays */

#define REL_REGISTER       1
#define REL_DATA           2
#define REL_KEEPALIVE      3
#define REL_BROADCAST      4

static int sock_fd        = -1;
static int my_node_id     = 1;
static int wire_version   = RELAY_VER_V1;   /* upgraded to V2 if key loaded */
static u8 key[KEY_LEN];
static struct sockaddr_in relay_list[MAX_RELAYS];
static int      relay_count = 0;
static int      cur_relay   = 0;            /* index into relay_list */
static u64 next_nonce = 0;
static time_t   last_send_ts = 0;

/* G4 inbound-auth state. */
static int relay_strict        = 0;   /* PKERNEL_RELAY_STRICT: drop v1 in */
static u32 mac_drop_count      = 0;   /* total inbound frames failing auth */
static u64 mac_drop_last_log_ms = 0;  /* rate-limit for the drop log       */
static int v1_permit_warned    = 0;   /* one-shot permissive-mode warning  */

/* HA liveness state (CLOCK_MONOTONIC milliseconds). */
static u64 ha_last_rx_ms       = 0;  /* last packet from any relay        */
static u64 ha_probe_sent_ms    = 0;  /* 0 = no liveness probe outstanding */
static u64 ha_last_failback_ms = 0;  /* last higher-priority probe sweep  */

static u64 now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000ULL + (u64)(ts.tv_nsec / 1000000L);
}

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

/* Constant-time compare of n bytes. Returns 1 if equal. */
static int ct_eq(const u8 *a, const u8 *b, int n)
{
    u8 d = 0;
    for (int i = 0; i < n; i++) d |= (u8)(a[i] ^ b[i]);
    return d == 0;
}

static u64 mint_nonce(void)
{
    return next_nonce++;
}

/* HMAC-SHA256 over (ver, type, src, dst, nonce, payload), truncated to 16 B. */
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

/* Build a packet (v1 or v2 depending on wire_version) into buf.
 * Returns total length. Caller must ensure buf >= MAX_PKT. */
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
    /* v2 */
    u64 nonce = mint_nonce();
    store_u64_le(buf + HEAD_LEN, nonce);
    compute_mac(RELAY_VER_V2, type, src, dst, nonce, payload, plen,
                buf + HEAD_LEN + 8);
    if (payload && plen > 0) {
        memcpy(buf + HEAD_LEN + AUTH_LEN, payload, (size_t)plen);
    }
    return HEAD_LEN + AUTH_LEN + plen;
}

/* Fire-and-forget to relay_list[idx]. Never blocks the caller. */
static void udp_send_to(int idx, const unsigned char *buf, int len)
{
    if (sock_fd < 0 || idx < 0 || idx >= relay_count) return;
    ssize_t r = sendto(sock_fd, buf, (size_t)len,
                       MSG_DONTWAIT | MSG_NOSIGNAL,
                       (struct sockaddr *)&relay_list[idx],
                       sizeof(relay_list[idx]));
    if (r < 0) {
        /* EAGAIN etc. silently ignored — next packet will retry. */
        return;
    }
    if (idx == cur_relay) last_send_ts = time(NULL);
}

static void send_register_to(int idx)
{
    static unsigned char buf[MAX_PKT];
    int n = build_packet(buf, REL_REGISTER, (unsigned)my_node_id, 0, NULL, 0);
    udp_send_to(idx, buf, n);
}

static void send_keepalive_to(int idx)
{
    static unsigned char buf[MAX_PKT];
    int n = build_packet(buf, REL_KEEPALIVE, (unsigned)my_node_id, 0, NULL, 0);
    udp_send_to(idx, buf, n);
}

/* Low-stack loggers for the failover hot path.
 *
 * glibc dprintf() funnels through vfprintf(), which burns ~4 KB of
 * stack. ha_tick()/ha_mark_rx() run on the netstack worker tasks
 * (net_task, the kdds publishers, ...), all of which have only ~4 KB
 * T-Kernel stacks. Calling dprintf() from these paths at the failover
 * instant overflows the task stack and silently corrupts a neighboring
 * task's saved context — producing the deterministic garbage-PC
 * SIGSEGV (pc=0x2000000024, addr=0) that only appeared once relay-HA's
 * failover prints met wave-7's guarded tasks. So we format by hand into
 * a static buffer and write(2) it. See docs note
 * feedback_hosted_relay_stack_overflow: net_relay scratch must never
 * live on the task stack. */
static void rl_append(char *dst, int cap, int *pos, const char *s)
{
    while (*s && *pos < cap - 1) dst[(*pos)++] = *s++;
    dst[*pos] = '\0';
}

static void rl_append_dec(char *dst, int cap, int *pos, unsigned v)
{
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0 && *pos < cap - 1) dst[(*pos)++] = t[--n];
    dst[*pos] = '\0';
}

/* "[net_relay] <what> relay#<idx> <a.b.c.d>:<port>\n" — byte-identical
 * to the previous dprintf format the failover test greps for. */
static void log_relay(const char *what, int idx)
{
    static char line[96];
    int pos = 0;
    unsigned ip = (unsigned)ntohl(relay_list[idx].sin_addr.s_addr);
    rl_append(line, (int)sizeof(line), &pos, "[net_relay] ");
    rl_append(line, (int)sizeof(line), &pos, what);
    rl_append(line, (int)sizeof(line), &pos, " relay#");
    rl_append_dec(line, (int)sizeof(line), &pos, (unsigned)idx);
    rl_append(line, (int)sizeof(line), &pos, " ");
    rl_append_dec(line, (int)sizeof(line), &pos, (ip >> 24) & 0xff);
    rl_append(line, (int)sizeof(line), &pos, ".");
    rl_append_dec(line, (int)sizeof(line), &pos, (ip >> 16) & 0xff);
    rl_append(line, (int)sizeof(line), &pos, ".");
    rl_append_dec(line, (int)sizeof(line), &pos, (ip >> 8) & 0xff);
    rl_append(line, (int)sizeof(line), &pos, ".");
    rl_append_dec(line, (int)sizeof(line), &pos, ip & 0xff);
    rl_append(line, (int)sizeof(line), &pos, ":");
    rl_append_dec(line, (int)sizeof(line), &pos,
                  (unsigned)ntohs(relay_list[idx].sin_port));
    rl_append(line, (int)sizeof(line), &pos, "\n");
    (void)write(2, line, (size_t)pos);
}

/* "[net_relay] relay#<idx> unresponsive\n" — same low-stack discipline. */
static void log_unresponsive(int idx)
{
    static char line[48];
    int pos = 0;
    rl_append(line, (int)sizeof(line), &pos, "[net_relay] relay#");
    rl_append_dec(line, (int)sizeof(line), &pos, (unsigned)idx);
    rl_append(line, (int)sizeof(line), &pos, " unresponsive\n");
    (void)write(2, line, (size_t)pos);
}

/* G4: rate-limited (<=1/sec) authentication-failure counter. Same low-stack
 * discipline as log_relay (static buffer + write(2), never glibc stdio on a
 * task stack — see the rl_append comment block above). Format is
 * "[net_relay] mac drop n=<count>\n". */
static void note_mac_drop(void)
{
    mac_drop_count++;
    u64 now = now_ms();
    if (mac_drop_last_log_ms != 0 && now - mac_drop_last_log_ms < 1000) return;
    mac_drop_last_log_ms = now;
    static char line[48];
    int pos = 0;
    rl_append(line, (int)sizeof(line), &pos, "[net_relay] mac drop n=");
    rl_append_dec(line, (int)sizeof(line), &pos, mac_drop_count);
    rl_append(line, (int)sizeof(line), &pos, "\n");
    (void)write(2, line, (size_t)pos);
}

/* G4: one-shot warning when permissive mode accepts an unauthenticated v1
 * frame while we hold a key (new/old migration aid). */
static void note_v1_permit(void)
{
    if (v1_permit_warned) return;
    v1_permit_warned = 1;
    static const char m[] =
        "[net_relay] WARNING: permissive mode accepted an unauthenticated "
        "v1 frame (set PKERNEL_RELAY_STRICT=1 to drop)\n";
    (void)write(2, m, sizeof(m) - 1);
}

/* HA driver — called from every send/recv. Three duties:
 *  1. legacy 25 s keepalive so the current relay doesn't evict us;
 *  2. every HA_FAILBACK_MS, probe all higher-priority relays so the
 *     whole fleet drifts back to the list head once it recovers;
 *  3. if nothing has been received for HA_RX_IDLE_MS, probe the
 *     current relay; if the probe goes unanswered for HA_PROBE_TMO_MS,
 *     advance (mod relay_count) and re-REGISTER there. */
static void ha_tick(void)
{
    u64 now = now_ms();

    if (time(NULL) - last_send_ts >= KEEPALIVE_SEC) {
        send_keepalive_to(cur_relay);
    }

    if (cur_relay > 0 && now - ha_last_failback_ms >= HA_FAILBACK_MS) {
        ha_last_failback_ms = now;
        for (int i = 0; i < cur_relay; i++) send_keepalive_to(i);
    }

    if (now - ha_last_rx_ms >= HA_RX_IDLE_MS) {
        if (ha_probe_sent_ms == 0) {
            send_keepalive_to(cur_relay);
            ha_probe_sent_ms = now;
        } else if (now - ha_probe_sent_ms >= HA_PROBE_TMO_MS) {
            int next = (cur_relay + 1) % relay_count;
            if (next != cur_relay) {
                log_unresponsive(cur_relay);
                cur_relay = next;
                log_relay("failover ->", cur_relay);
            }
            ha_probe_sent_ms = 0;
            ha_last_rx_ms    = now;   /* grace period for the new relay */
            send_register_to(cur_relay);
        }
    }
}

/* Called whenever a packet arrives from relay_list[idx]. Refreshes
 * liveness; if a higher-priority relay answered (idx < cur_relay),
 * switch back to it — deterministic failback. */
static void ha_mark_rx(int idx)
{
    if (idx == cur_relay) {
        ha_last_rx_ms    = now_ms();
        ha_probe_sent_ms = 0;
    } else if (idx >= 0 && idx < cur_relay) {
        cur_relay = idx;
        log_relay("failback ->", cur_relay);
        ha_last_rx_ms    = now_ms();
        ha_probe_sent_ms = 0;
        send_register_to(cur_relay);
    }
    /* idx > cur_relay: a lower-priority relay is alive — irrelevant
     * while a higher-priority one serves us; ignore for liveness. */
}

/* Match a UDP source address against the configured relay list.
 * Returns the index, or -1 if the sender is not a known relay. */
static int relay_index_of(const struct sockaddr_in *from)
{
    for (int i = 0; i < relay_count; i++) {
        if (relay_list[i].sin_addr.s_addr == from->sin_addr.s_addr &&
            relay_list[i].sin_port        == from->sin_port) {
            return i;
        }
    }
    return -1;
}

/* Resolve host (DNS name or dotted-quad) + port into *out. 0 on success. */
static int resolve_relay(const char *host, int port, struct sockaddr_in *out)
{
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = htons((unsigned short)port);

    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) return 0;

    struct addrinfo hints = {0};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, NULL, &hints, &res);
    if (gai != 0 || !res) {
        dprintf(2, "[net_relay] resolve '%s' failed: %s\n",
                host, gai_strerror(gai));
        return -1;
    }
    struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
    out->sin_addr = sin->sin_addr;
    freeaddrinfo(res);
    return 0;
}

/* Parse PKERNEL_RELAY="host[:port],host[:port][,...]" (max MAX_RELAYS)
 * into relay_list[]. Returns the number of relays, or -1 on error. */
static int parse_relay_list(const char *spec)
{
    char work[256];
    size_t sl = strlen(spec);
    if (sl == 0 || sl >= sizeof(work)) {
        dprintf(2, "[net_relay] PKERNEL_RELAY empty or too long\n");
        return -1;
    }
    memcpy(work, spec, sl + 1);

    int count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(work, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        if (count >= MAX_RELAYS) {
            dprintf(2, "[net_relay] PKERNEL_RELAY: more than %d entries, "
                       "ignoring the rest\n", MAX_RELAYS);
            break;
        }
        while (*tok == ' ') tok++;
        int port = DEFAULT_PORT;
        char *colon = strrchr(tok, ':');
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
            if (port <= 0 || port > 65535) port = DEFAULT_PORT;
        }
        if (!*tok) continue;
        if (resolve_relay(tok, port, &relay_list[count]) < 0) return -1;
        count++;
    }
    return count;
}

/* --- public API --------------------------------------------------------- */

int net_relay_init(void)
{
    const char *env_id   = getenv("PKERNEL_NODE_ID");
    const char *env_list = getenv("PKERNEL_RELAY");
    const char *env_host = getenv("PKERNEL_RELAY_HOST");
    const char *env_port = getenv("PKERNEL_RELAY_PORT");
    const char *env_key  = getenv("PKERNEL_RELAY_KEY");

    my_node_id = env_id ? atoi(env_id) : 1;
    if (my_node_id < 1 || my_node_id > 255) {
        /* G7: don't silently rewrite a bad id to 1 (which would collide with
         * node 1) — say what happened. */
        dprintf(2, "[net_relay] PKERNEL_NODE_ID=%d out of range (1..255) — "
                   "defaulting to 1\n", my_node_id);
        my_node_id = 1;
    } else if (my_node_id > NET_CLUSTER_NODE_MAX) {
        /* G7: valid on the relay wire, but the kernel cluster can't see it.
         * This is the silent-dropout case the audit flagged — make it loud. */
        dprintf(2, "[net_relay] WARNING: PKERNEL_NODE_ID=%d exceeds cluster "
                   "DNODE_MAX=%d — this node will register with the relay but "
                   "NOT join the drpc/pmesh/kdds cluster\n",
                   my_node_id, NET_CLUSTER_NODE_MAX);
    }

    if (env_key && *env_key) {
        if (hex_decode(env_key, key, KEY_LEN) < 0) {
            dprintf(2, "[net_relay] PKERNEL_RELAY_KEY must be %d hex chars\n",
                    KEY_LEN * 2);
            return -1;
        }
        wire_version = RELAY_VER_V2;
    } else {
        wire_version = RELAY_VER_V1;
        dprintf(2, "[net_relay] WARNING: no PKERNEL_RELAY_KEY — using v1 wire "
                   "(relay must run with --insecure)\n");
    }

    /* G4: inbound-auth policy. Default permissive — with a key we always
     * verify v2 MACs and drop mismatches, but an unauthenticated v1 frame is
     * accepted (once-warned) to ease new/old migration. PKERNEL_RELAY_STRICT
     * makes v1 inbound a hard drop too. See docs/phase_b_relay.md. */
    {
        const char *env_strict = getenv("PKERNEL_RELAY_STRICT");
        relay_strict = (env_strict && *env_strict && env_strict[0] != '0');
    }

    if (env_list && *env_list) {
        relay_count = parse_relay_list(env_list);
        if (relay_count <= 0) {
            dprintf(2, "[net_relay] PKERNEL_RELAY has no usable entries\n");
            return -1;
        }
    } else if (env_host && *env_host) {
        /* Legacy single-relay configuration. */
        int port = env_port ? atoi(env_port) : DEFAULT_PORT;
        if (port <= 0 || port > 65535) port = DEFAULT_PORT;
        if (resolve_relay(env_host, port, &relay_list[0]) < 0) return -1;
        relay_count = 1;
    } else {
        dprintf(2, "[net_relay] neither PKERNEL_RELAY nor PKERNEL_RELAY_HOST set\n");
        return -1;
    }
    cur_relay = 0;

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        dprintf(2, "[net_relay] socket: %s\n", strerror(errno));
        return -1;
    }
    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);

    /* Bind an ephemeral port — needed so the relay learns our peer addr
     * from REGISTER's source UDP tuple. */
    struct sockaddr_in any = {0};
    any.sin_family = AF_INET;
    if (bind(sock_fd, (struct sockaddr *)&any, sizeof(any)) < 0) {
        dprintf(2, "[net_relay] bind: %s\n", strerror(errno));
        close(sock_fd); sock_fd = -1;
        return -1;
    }

    /* Initial nonce uses wall clock seconds in the upper 40 bits so that
     * a client restart always exceeds the relay's stored max_nonce (see
     * docs/phase_b_relay.md, replay protection section). */
    next_nonce = ((u64)time(NULL) << 24) | 1;

    ha_last_rx_ms = now_ms();   /* grace period before the first probe */

    {
        char ipbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &relay_list[0].sin_addr, ipbuf, sizeof(ipbuf));
        dprintf(2, "[net_relay] node %d → %s:%d (wire v%d, %d relay%s)\n",
                my_node_id, ipbuf, (int)ntohs(relay_list[0].sin_port),
                wire_version, relay_count, relay_count == 1 ? "" : "s");
    }

    send_register_to(cur_relay);
    return my_node_id;
}

int net_relay_send(const void *frame, int len)
{
    if (sock_fd < 0 || len <= 0 || len > MAX_PAYLOAD) return -1;
    ha_tick();
    static unsigned char buf[MAX_PKT];
    int n = build_packet(buf, REL_BROADCAST,
                         (unsigned)my_node_id, 0, frame, len);
    udp_send_to(cur_relay, buf, n);
    return len;
}

int net_relay_recv(void *out, int maxlen)
{
    if (sock_fd < 0) return 0;
    ha_tick();
    static unsigned char buf[MAX_PKT];

    /* Drain control packets (keepalive echoes) so a data frame is never
     * starved behind them; return on the first data payload. */
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(sock_fd, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) return 0;

        /* Only accept packets from a configured relay. */
        int ridx = relay_index_of(&from);
        if (ridx < 0) continue;

        /* Strip the relay header. */
        if (n < HEAD_LEN) continue;
        if (buf[0] != (u8)(RELAY_MAGIC      & 0xff) ||
            buf[1] != (u8)((RELAY_MAGIC>>8) & 0xff) ||
            buf[2] != (u8)((RELAY_MAGIC>>16)& 0xff) ||
            buf[3] != (u8)((RELAY_MAGIC>>24)& 0xff)) continue;

        unsigned ver  = buf[4];
        unsigned type = buf[5];
        int hdr = (ver == RELAY_VER_V2) ? (HEAD_LEN + AUTH_LEN) : HEAD_LEN;
        if (n < hdr) continue;

        /* G4: authenticate the frame BEFORE trusting it for liveness OR data.
         * The relay forwards frames verbatim, so a v2 frame still carries the
         * originator's HMAC-SHA256 over the shared PSK — we recompute it over
         * (ver,type,src,dst,nonce,payload) and constant-time compare. A
         * spoofed relay source, a tampered frame, or an injected one all fail
         * this and are dropped (rate-limited counter). Without this the client
         * was wide open: anyone who could send to our UDP tuple from the
         * relay's address could inject arbitrary frames into the stack. */
        if (wire_version == RELAY_VER_V2) {
            if (ver == RELAY_VER_V2) {
                u64 nonce = load_u64_le(buf + HEAD_LEN);
                int plen  = (int)n - hdr;
                u8 want[HMAC_TRUNC_LEN];
                compute_mac(RELAY_VER_V2, buf[5], buf[6], buf[7], nonce,
                            buf + hdr, plen, want);
                if (!ct_eq(want, buf + HEAD_LEN + 8, HMAC_TRUNC_LEN)) {
                    note_mac_drop();
                    continue;
                }
            } else {
                /* v1 frame while we hold a key: unauthenticated. */
                if (relay_strict) { note_mac_drop(); continue; }
                note_v1_permit();
            }
        }

        ha_mark_rx(ridx);   /* liveness + deterministic failback */

        /* KEEPALIVE echoes (and stray REGISTERs) are control-plane only. */
        if (type != REL_DATA && type != REL_BROADCAST) continue;

        int payload_len = (int)n - hdr;
        if (payload_len > maxlen) payload_len = maxlen;
        if (payload_len > 0) memcpy(out, buf + hdr, (size_t)payload_len);
        return payload_len;
    }
}

int net_relay_node_id(void) { return my_node_id; }
