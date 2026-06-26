/*
 *  arch/linux/x86_64/net_relay.c
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
 *  Seed bootstrap (N-4, thread-N decentralization): a relay is just ONE
 *  optional seed, not a mandatory central dependency. PKERNEL_SEED takes
 *  the SAME "host[:port],..." list shape as PKERNEL_RELAY and is parsed
 *  by the SAME parse_relay_list() into relay_list[] — at the wire a relay
 *  is simply a seed that answers REL_REGISTER, so the two are identical
 *  once configured. The only behavior change is SCOPED to seed-mode: when
 *  PKERNEL_SEED is set but its list is empty/unusable, the node BOOTS
 *  SOLO (relay_count=0, net_relay_send is a no-op via the idx>=relay_count
 *  guard) instead of hard-failing. The plain PKERNEL_RELAY / legacy
 *  PKERNEL_RELAY_HOST paths are UNCHANGED, INCLUDING the hard return -1
 *  when neither relay env is set — solo-degrade never applies to them.
 *  The pure selection core seed_select_next() (deterministic lowest-index-
 *  not-yet-failed, wrapping, -1 when exhausted) is what the seed-bootstrap
 *  cert drives in-proc with no sockets. SWIM broadcast discovery is
 *  unchanged (no unicast seed-discovery is added here).
 *
 *  Env vars:
 *    PKERNEL_NODE_ID      our node_id (1..255), default 1
 *    PKERNEL_SEED         "host[:port],host[:port][,...]" (max 4) — seed/
 *                         peer list; a relay is just a seed that answers
 *                         REL_REGISTER. If set, takes precedence over
 *                         PKERNEL_RELAY; an empty/unusable list boots SOLO.
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
/* connect-anywhere SLICE 1: this MUST stay safely below the ~30 s UDP
 * mapping lifetime that home NAT/AP firewalls give an outbound flow
 * (NAT_TIMEOUT_FLOOR). At 15 s a node can lose ONE keepalive entirely and
 * still refresh the return mapping (2*15 = 30) before it ages out, so an
 * inbound admission grant can always find its way back. 25 s left no margin
 * for a single drop. Also < IDLE_TIMEOUT/2 in the relay (eviction guard). */
#define KEEPALIVE_SEC      15
#define NAT_TIMEOUT_FLOOR  30            /* conservative NAT/AP UDP map life (s) */
/* G7/G23: the relay wire allows node ids 1..255, but the kernel cluster
 * (drpc) only tracks 1..DNODE_MAX. This MUST mirror DNODE_MAX in
 * arch/common/include/drpc.h (T-Kernel headers are not includable in this
 * linux TU, so it cannot be #derived — keep the two in sync by hand). A node
 * id above this registers with the relay but never joins drpc/pmesh/kdds, so
 * we warn at init instead of vanishing. */
#define NET_CLUSTER_NODE_MAX 64   /* == DNODE_MAX (drpc.h); bump together */

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

/* connect-anywhere SLICE 1 — heartbeat cert seam. In a normal build HB_NOW()
 * is exactly time(NULL) and the divert is compiled out, so production .text is
 * byte-for-byte unchanged. Under -DHEARTBEAT_CERT the self-test drives a mock
 * monotonic clock and counts packets WITHOUT touching a socket. */
#ifdef HEARTBEAT_CERT
static long hb_mock_now    = 0;   /* mock monotonic seconds                  */
static int  hb_use_mock    = 0;   /* 1 = divert sendto + use the mock clock  */
static int  hb_sendto_count= 0;   /* keepalives the seam "emitted"           */
static int  hb_peer_count  = 0;   /* cert-controlled; 0 = isolated/un-admitted */
#define HB_NOW()  (hb_use_mock ? hb_mock_now : (long)time(NULL))
#else
#define HB_NOW()  ((long)time(NULL))
#endif

/* G4 inbound-auth state. */
static int relay_strict        = 0;   /* PKERNEL_RELAY_STRICT: drop v1 in */
static u32 mac_drop_count      = 0;   /* total inbound frames failing auth */
static u64 mac_drop_last_log_ms = 0;  /* rate-limit for the drop log       */
static int v1_permit_warned    = 0;   /* one-shot permissive-mode warning  */

/* Receive-side replay window (the bonus hole surfaced in wave 10).
 *
 * G4 verifies the HMAC of every inbound v2 frame, but a captured-and-resent
 * frame carries a *valid* MAC, so HMAC verification alone re-admits replays.
 * The relay has its own per-source nonce window, but it lives in another
 * process and protects a different hop — an injector that can reach our UDP
 * tuple from the relay's address (or a buggy/compromised relay) bypasses it
 * entirely. So the client keeps its OWN per-source 64-packet sliding nonce
 * window here, keyed by src node id, with the same logic as relay.c's
 * replay_check_and_update. A fresh nonce is NEVER dropped, so legitimate
 * traffic always passes; a repeated or too-old nonce is dropped and counted.
 * All state is static — never on a task stack (see
 * feedback_hosted_relay_stack_overflow). */
#define RX_NODE_MAX 256
static u64 rx_nonce_max[RX_NODE_MAX];
static u64 rx_nonce_win[RX_NODE_MAX];
static u8  rx_nonce_armed[RX_NODE_MAX];
static u32 rx_ok_count          = 0;  /* authenticated, fresh frames passed */
static u32 replay_drop_count    = 0;  /* inbound frames dropped as replays  */
static u64 replay_drop_last_log_ms = 0; /* rate-limit for the replay log    */

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
    if (diff >= 64) return 0;                /* too old — outside the window  */
    u64 bit = 1ULL << diff;
    if (rx_nonce_win[src] & bit) return 0;   /* already seen — replay         */
    rx_nonce_win[src] |= bit;
    return 1;
}

/* Read-only counters for the shell (`rx` -> [rx-relay]). */
void net_relay_stats(unsigned long *ok, unsigned long *badmac,
                     unsigned long *replay)
{
    if (ok)     *ok     = (unsigned long)rx_ok_count;
    if (badmac) *badmac = (unsigned long)mac_drop_count;
    if (replay) *replay = (unsigned long)replay_drop_count;
}

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
#ifdef HEARTBEAT_CERT
    if (hb_use_mock) {
        /* cert seam: count the emit, stamp the mock clock, no real socket. */
        hb_sendto_count++;
        if (idx == cur_relay) last_send_ts = (time_t)hb_mock_now;
        return;
    }
#endif
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

/* Rate-limited (<=1/sec) replay-drop counter/log. Same low-stack discipline
 * as note_mac_drop (static buffer + write(2), never glibc stdio on a task
 * stack). Format is "[net_relay] replay drop n=<count>\n". */
static void note_replay_drop(void)
{
    replay_drop_count++;
    u64 now = now_ms();
    if (replay_drop_last_log_ms != 0 && now - replay_drop_last_log_ms < 1000) return;
    replay_drop_last_log_ms = now;
    static char line[48];
    int pos = 0;
    rl_append(line, (int)sizeof(line), &pos, "[net_relay] replay drop n=");
    rl_append_dec(line, (int)sizeof(line), &pos, replay_drop_count);
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

/* Resolve host (dotted-quad) + port into *out. 0 on success, -1 on skip.
 *
 * DEGRADE-NOT-CRASH (resolve-crash fix): relay/seed endpoints in the hosted
 * p-kernel are NUMERIC IPs (the overlay uses 10.1.0.x; real relays are given
 * by IP — see samples/11_distributed/*.sh, all PKERNEL_RELAY*=127.0.0.1 /
 * numeric). A NON-numeric host is treated as UNRESOLVABLE: we log it and
 * return -1 so the caller drops the entry and falls through to solo/loopback,
 * NEVER crash.
 *
 * Why not getaddrinfo(): this TU is built under the T-Kernel runtime and runs
 * inside a T-Kernel task (small stack, fixed mmap arena). glibc getaddrinfo()
 * SIGSEGVs in that environment when handed a name that needs NSS resolution
 * (the fault is INSIDE getaddrinfo, before its error return is ever seen — the
 * immune-system audit's repro: PKERNEL_RELAY=garbage:notaport -> exit 139).
 * The standalone getaddrinfo works, so this is specific to the kernel-task
 * context; calling it here is a latent crash. Numeric-IP-only is the chosen
 * crash-safe contract; DNS relay names are unsupported in this build. The
 * getaddrinfo path is retained ONLY behind RESOLVE_NO_GUARD so the cert's
 * falsifier can re-expose the crash and prove this guard is load-bearing. */
static int resolve_relay(const char *host, int port, struct sockaddr_in *out)
{
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = htons((unsigned short)port);

    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) return 0;

#ifndef RESOLVE_NO_GUARD
    /* Non-numeric host: unresolvable in this build — skip, do not crash. */
    dprintf(2, "[net_relay] unresolvable host '%s' (numeric IP required in "
               "this build) — skipping\n", host);
    return -1;
#else
    /* FALSIFIER ONLY (RESOLVE_NO_GUARD): the crash-prone path. */
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
#endif
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
        /* DEGRADE-NOT-CRASH: skip an unresolvable entry rather than failing
         * the whole list. A list that is ALL-unresolvable yields count==0,
         * which the caller turns into a solo/loopback degrade (never a crash).
         * A mixed list keeps its resolvable (numeric) endpoints. */
        if (resolve_relay(tok, port, &relay_list[count]) < 0) continue;
        count++;
    }
    return count;
}

/* N-4 seed bootstrap — PURE selection core. Given the index we last tried
 * (cur), a liveness vector alive[] (1 = answers REGISTER, 0 = dead/exhausted)
 * over `count` endpoints, return the next endpoint index to try:
 * deterministic lowest-index-not-yet-failed, scanning forward and wrapping;
 * -1 when every endpoint is exhausted. Integer-only, allocation-free, and
 * bounded by `count` (at most `count` probes — never an infinite loop). The
 * sabotage hook below makes the advance a no-op so the cert can prove the
 * advance is load-bearing (Falsifier B). */
int seed_select_next(int cur, const unsigned char *alive, int count)
{
#ifdef SEED_NO_ADVANCE
    return cur;
#endif
    if (count <= 0 || !alive) return -1;
    /* Scan the next `count` indices starting just after cur, wrapping; the
     * loop bound guarantees termination even when all are dead. */
    for (int step = 1; step <= count; step++) {
        int idx = ((cur < 0 ? -1 : cur) + step) % count;
        if (idx < 0) idx += count;
        if (alive[idx]) return idx;
    }
    return -1;
}

/* --- public API --------------------------------------------------------- */

int net_relay_init(void)
{
    const char *env_id   = getenv("PKERNEL_NODE_ID");
    const char *env_seed = getenv("PKERNEL_SEED");
    const char *env_list = getenv("PKERNEL_RELAY");
    const char *env_host = getenv("PKERNEL_RELAY_HOST");
    const char *env_port = getenv("PKERNEL_RELAY_PORT");
    const char *env_key  = getenv("PKERNEL_RELAY_KEY");

    /* N-0: distinct, stable per-install id when PKERNEL_NODE_ID is unset. */
    my_node_id = env_id ? atoi(env_id) : pkernel_default_node_id();
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

    if (env_seed && *env_seed) {
        /* N-4 seed-mode: a relay is just a seed that answers REL_REGISTER, so
         * the seed list is parsed into the SAME relay_list[] with the SAME
         * shape. Solo-degrade is SCOPED here: an empty/unusable seed list boots
         * the node SOLO (relay_count=0 -> net_relay_send is a no-op via the
         * idx>=relay_count guard) and returns success, rather than hard-failing
         * the way the plain PKERNEL_RELAY path does. */
        relay_count = parse_relay_list(env_seed);
        if (relay_count <= 0) {
            dprintf(2, "[net_relay] no usable seed — running solo\n");
            relay_count = 0;
            cur_relay   = 0;
            return my_node_id;
        }
    } else if (env_list && *env_list) {
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

        /* For v2 frames the nonce is carried right after the head; remember
         * it so the replay window can be consulted once the MAC is verified
         * and control packets are filtered out. v1 frames carry no nonce. */
        u64 rx_nonce     = 0;
        int rx_have_nonce = 0;

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
                rx_nonce      = nonce;
                rx_have_nonce = 1;
            } else {
                /* v1 frame while we hold a key: unauthenticated. */
                if (relay_strict) { note_mac_drop(); continue; }
                note_v1_permit();
            }
        }

        ha_mark_rx(ridx);   /* liveness + deterministic failback */

        /* KEEPALIVE echoes (and stray REGISTERs) are control-plane only.
         * They are filtered out BEFORE the replay window so control traffic
         * never consumes a data nonce slot. */
        if (type != REL_DATA && type != REL_BROADCAST) continue;

        /* Replay window: a captured v2 data frame carries a valid HMAC, so
         * the MAC check above re-admits replays. Drop any nonce already seen
         * from this src (per-source 64-packet window). Fresh nonces are never
         * dropped, so legitimate traffic always passes. v1 frames carry no
         * nonce and so are not replay-protected (that is what v2 is for). */
        if (rx_have_nonce && !rx_replay_ok(buf[6], rx_nonce)) {
            note_replay_drop();
            continue;
        }

        int payload_len = (int)n - hdr;
        if (payload_len > maxlen) payload_len = maxlen;
        if (payload_len > 0) memcpy(out, buf + hdr, (size_t)payload_len);
        rx_ok_count++;
        return payload_len;
    }
}

int net_relay_node_id(void) { return my_node_id; }

/* connect-anywhere SLICE 1 — the UNCONDITIONAL relay keepalive.
 *
 * This is exactly ha_tick()'s first duty (the legacy keepalive clause),
 * lifted into its own exported entry so a dedicated hosted task can beat it
 * on a fixed cadence that is INDEPENDENT of:
 *   - peer discovery (SWIM) — we beat even with zero known peers, and
 *   - cluster admission   — we beat even while drpc_my_node == 0xFF, i.e.
 *     before (or without ever) being granted a cluster id.
 * That breaks the observed two-machine deadlock: a node registered, sent a
 * handful of packets, then originated NO cluster traffic while waiting for an
 * admission grant; with nothing leaving, the NAT/AP return mapping aged out
 * and the grant could never arrive. A steady < NAT_TIMEOUT_FLOOR keepalive
 * keeps that return path open regardless.
 *
 * Depends ONLY on sock_fd + cur_relay (both set in net_relay_init), never on
 * peers or admission state. The udp_send_to() guard (idx >= relay_count)
 * keeps a solo node (relay_count == 0, no relay configured) a clean no-op —
 * a solo node with no relay beats nothing. */
void net_relay_heartbeat(void)
{
#if defined(HEARTBEAT_CERT) && defined(HEARTBEAT_FALSIFIER)
    /* FALSIFIER build: the BROKEN design that gates the beat on cluster
     * admission / peer presence. An isolated, un-admitted node (hb_peer_count
     * == 0 — the exact deadlock this slice cures) then emits ZERO keepalives,
     * and the self-test's "emitted >= floor(T/KEEPALIVE_SEC)" assertion FAILS.
     * Proof the cert has teeth. */
    if (hb_peer_count == 0) return;
#endif
    if (HB_NOW() - (long)last_send_ts >= KEEPALIVE_SEC)
        send_keepalive_to(cur_relay);
}

#ifdef SEED_BOOTSTRAP_CERT
/* [seed-bootstrap] — N-4 seed-bootstrap cert. In-proc, NO sockets, NO host
 * (mirror supernode_forward_self_test / swim_cap_gossip_self_test). Drives the
 * PURE seed_select_next() through a tiny harness: seed_alive[i] = 1 means
 * "seed i answers REL_REGISTER", and register_sent_to[i] counts the
 * cert-shimmed REGISTERs (NO UDP). A "join" walks seed_select_next() from a
 * cold start (cur=-1) following the deterministic rule, sends a shimmed
 * REGISTER to each candidate, and stops at the first LIVE seed; if the
 * selector exhausts (-1) the node is solo and joined stays 0.
 *
 * HONEST SCOPE: this certifies the PURE selection + degrade contract only. The
 * real "A boots with PKERNEL_SEED=B, unicasts B over real sockets, joins" is a
 * DEFERRED [live] row (samples/11_distributed/run_seed_bootstrap.sh) needing a
 * real host; it is NOT covered here. Back-compat of the plain PKERNEL_RELAY
 * path is load-bearing and is asserted by the unchanged hard-fail leg in
 * net_relay_init (verified by the auditor, not simulable in-proc). */

static unsigned char seed_alive[MAX_RELAYS];        /* 1 = answers REGISTER  */
static int           register_sent_to[MAX_RELAYS];  /* cert-shim, NO UDP     */
static int           sb_fail;

static void sb_putdec(void (*pr)(const char *), int v)
{
    char b[12]; int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) b[i++] = '0';
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    char o[14]; int j = 0;
    if (neg) o[j++] = '-';
    while (i > 0) o[j++] = b[--i];
    o[j] = '\0';
    pr(o);
}

static void sb_check(void (*pr)(const char *), int ok, const char *desc)
{
    pr(ok ? "[seed-bootstrap]   PASS " : "[seed-bootstrap]   FAIL ");
    pr(desc); pr("\r\n");
    if (!ok) sb_fail = 1;
}

/* Cold-start join over the pure selector. Returns the index joined on, or -1
 * (solo). steps_out = number of selector advances taken (loop-bound proof).
 * joined_out = 1 if a live seed answered. */
static int sb_join(int count, int *joined_out, int *steps_out)
{
    int cur = -1, steps = 0, joined = 0, chosen = -1;
    for (;;) {
        if (steps > count) break;            /* HARD bound — never hang */
        int nxt = seed_select_next(cur, seed_alive, count);
        if (nxt < 0) break;                  /* exhausted -> solo */
        steps++;
        register_sent_to[nxt]++;             /* shimmed REGISTER (no UDP) */
        if (seed_alive[nxt]) { joined = 1; chosen = nxt; break; }
        cur = nxt;                           /* dead -> advance */
    }
    if (joined_out) *joined_out = joined;
    if (steps_out)  *steps_out  = steps;
    return chosen;
}

void seed_bootstrap_self_test(void (*pr)(const char *))
{
    sb_fail = 0;
    pr("[seed-bootstrap] N-4 seed-bootstrap cert (pure selection + degrade)\r\n");

    /* ---- CURE: 3 seeds, only the LAST is live (alive={0,0,1}) -> join
     * happens on idx 2, NOT the dead head idx 0. -------------------------- */
    {
        for (int i = 0; i < MAX_RELAYS; i++) { seed_alive[i] = 0; register_sent_to[i] = 0; }
        seed_alive[2] = 1;
        int joined = 0, steps = 0;
        int chosen = sb_join(3, &joined, &steps);
        sb_check(pr, joined == 1, "CURE: a live seed answered (joined)");
        sb_check(pr, chosen == 2, "CURE: chosen_idx==2 (joined via the LIVE seed)");
        sb_check(pr, register_sent_to[2] > 0, "CURE: register_sent_to[2] > 0");
        sb_check(pr, chosen != 0, "CURE: did NOT settle on the dead head idx 0");
        sb_check(pr, steps <= 3, "CURE: bounded (steps <= count, no hang)");
        pr("[seed-bootstrap]   info chosen="); sb_putdec(pr, chosen);
        pr(" steps="); sb_putdec(pr, steps); pr("\r\n");
    }

    /* ---- FALSIFIER A (degrade, load-bearing): single seed, dead
     * (alive={0}) -> selector exhausts (-1), node is solo, NO HANG, does
     * NOT falsely report joined. ----------------------------------------- */
    {
        for (int i = 0; i < MAX_RELAYS; i++) { seed_alive[i] = 0; register_sent_to[i] = 0; }
        int joined = 1, steps = 99;
        int chosen = sb_join(1, &joined, &steps);
        int exhausted = (seed_select_next(0, seed_alive, 1) == -1);
        sb_check(pr, exhausted, "FAL-A: seed_select_next exhausts -> -1");
        sb_check(pr, joined == 0, "FAL-A: joined==0 (node is solo)");
        sb_check(pr, chosen == -1, "FAL-A: no seed chosen (solo)");
        sb_check(pr, steps <= 1, "FAL-A: bounded (steps <= count, NO HANG)");
        pr("[seed-bootstrap]   info solo chosen="); sb_putdec(pr, chosen);
        pr(" steps="); sb_putdec(pr, steps); pr("\r\n");
    }

    /* ---- FINAL RESULT. Under -DSEED_NO_ADVANCE the CURE leg lands on the
     * dead head idx 0 (never advances) -> joined==0 -> CURE asserts FAIL ->
     * RESULT: FAIL, proving the advance is load-bearing. ------------------ */
    if (sb_fail) pr("[seed-bootstrap] RESULT: FAIL\r\n");
    else         pr("[seed-bootstrap] RESULT: 9/9 PASS\r\n");
}
#endif /* SEED_BOOTSTRAP_CERT */

#ifdef HEARTBEAT_CERT
/* [heartbeat] connect-anywhere SLICE 1 cert — the UNCONDITIONAL relay
 * keepalive. In-proc, NO sockets, NO host (mirror seed_bootstrap_self_test):
 * it drives the SHIPPED net_relay_heartbeat() across T simulated seconds with
 * a mock monotonic clock, ZERO inbound and ZERO net_relay_send() calls — i.e.
 * an ISOLATED, UN-ADMITTED node (no peers, drpc would read 0xFF). The
 * udp_send_to() seam (hb_use_mock) counts emitted keepalives without a socket.
 *
 * ASSERTS: at least floor(T/KEEPALIVE_SEC) keepalives emitted AND the max
 * inter-keepalive gap stays < NAT_TIMEOUT_FLOOR (so the NAT return mapping
 * never ages out). The FALSIFIER build (-DHEARTBEAT_FALSIFIER) gates the beat
 * on admission (hb_peer_count>0) -> emits ZERO -> RESULT: FAIL (teeth). */
static int hb_fail;

static void hb_check(void (*pr)(const char *), int ok, const char *desc)
{
    pr(ok ? "[heartbeat]   PASS " : "[heartbeat]   FAIL ");
    pr(desc); pr("\r\n");
    if (!ok) hb_fail = 1;
}

static void hb_putdec(void (*pr)(const char *), int v)
{
    char b[12]; int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) b[i++] = '0';
    while (v > 0) { b[i++] = (char)('0' + v % 10); v /= 10; }
    char o[14]; int j = 0;
    if (neg) o[j++] = '-';
    while (i > 0) o[j++] = b[--i];
    o[j] = '\0';
    pr(o);
}

void net_relay_heartbeat_self_test(void (*pr)(const char *))
{
    hb_fail = 0;
    pr("[heartbeat] connect-anywhere SLICE 1 cert (unconditional relay keepalive)\r\n");

    /* Save the production statics we borrow, so a real run after the cert is
     * unaffected (the seed cert touches cert-only statics; we touch real ones). */
    int     sv_relay_count = relay_count, sv_cur = cur_relay, sv_sock = sock_fd;
    int     sv_node = my_node_id, sv_wire = wire_version;
    time_t  sv_last = last_send_ts;

    /* Arrange an ISOLATED, UN-ADMITTED node: exactly ONE relay configured,
     * NO peers, never admitted (hb_peer_count==0 == drpc_my_node 0xFF). */
    relay_count  = 1;
    cur_relay    = 0;
    sock_fd      = 999;            /* >=0 so the udp_send_to guard passes      */
    my_node_id   = 7;
    wire_version = RELAY_VER_V1;   /* keyless build_packet path; no socket I/O */
    hb_use_mock     = 1;
    hb_sendto_count = 0;
    hb_peer_count   = 0;           /* isolated: no peers, NOT admitted         */
    hb_mock_now     = 1000;        /* arbitrary monotonic base                 */
    last_send_ts    = (time_t)hb_mock_now;   /* just registered               */

    const long T = 120;           /* 120 s of total isolation                 */
    int  emitted = 0;
    long max_gap = 0, last_emit_at = hb_mock_now;
    for (long t = 1; t <= T; t++) {
        hb_mock_now = 1000 + t;
        int pre = hb_sendto_count;
        net_relay_heartbeat();            /* the SHIPPED entry point          */
        if (hb_sendto_count > pre) {
            long gap = hb_mock_now - last_emit_at;
            if (gap > max_gap) max_gap = gap;
            last_emit_at = hb_mock_now;
            emitted += (hb_sendto_count - pre);
        }
    }
    /* also count the silent tail (last emit -> end) as a gap */
    {
        long tail = hb_mock_now - last_emit_at;
        if (tail > max_gap) max_gap = tail;
    }

    int expect_min = (int)(T / KEEPALIVE_SEC);

    pr("[heartbeat]   info emitted="); hb_putdec(pr, emitted);
    pr(" expect_min=");                hb_putdec(pr, expect_min);
    pr(" max_gap=");                   hb_putdec(pr, (int)max_gap);
    pr(" KEEPALIVE_SEC=");             hb_putdec(pr, (int)KEEPALIVE_SEC);
    pr(" NAT_FLOOR=");                 hb_putdec(pr, (int)NAT_TIMEOUT_FLOOR);
    pr("\r\n");

    hb_check(pr, emitted >= expect_min,
             "isolated un-admitted node STILL beats (emitted >= floor(T/KEEPALIVE_SEC))");
    hb_check(pr, max_gap < NAT_TIMEOUT_FLOOR,
             "max inter-keepalive gap < NAT_TIMEOUT_FLOOR (mapping stays open)");
    hb_check(pr, emitted > 0,
             "at least one keepalive WITHOUT any peer or admission");

    /* restore */
    relay_count = sv_relay_count; cur_relay = sv_cur; sock_fd = sv_sock;
    my_node_id  = sv_node; wire_version = sv_wire; last_send_ts = sv_last;
    hb_use_mock = 0;

    if (hb_fail) pr("[heartbeat] RESULT: FAIL\r\n");
    else         pr("[heartbeat] RESULT: 3/3 PASS\r\n");
}
#endif /* HEARTBEAT_CERT */
