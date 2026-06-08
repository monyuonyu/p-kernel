/*
 *  samples/36_relay_measure/measure.c — REAL relay round-trip latency under
 *  load + a real end-to-end energy proxy (survival-network.md §4).
 *
 *  Audit G31/G25 said §4's core ("MoE sparsity = the answer to the light-speed
 *  + ENERGY constraint") was still largely UNMEASURED on the real path:
 *    - locality.md (wave-12) measured kernel-side traffic + an energy proxy
 *      from kdds counters, but its (D) left real per-packet LATENCY unmeasured.
 *    - latency.md (wave-15) injected a MODELLED far-delay and measured the
 *      two-layer separation, but not the relay's OWN forwarding RTT, and not
 *      under load.
 *  This harness measures, on a REAL ./relay over REAL UDP sockets:
 *
 *    rtt   — the relay's intrinsic forwarding round-trip time (ship->relay->
 *            ship via the existing KEEPALIVE echo) as a function of OFFERED
 *            LOAD (burst depth = in-flight probes). With the relay's opt-in
 *            probe-stamp ON it also decomposes RTT into network vs relay
 *            residence (the relay-side rx->tx time it appends to the echo).
 *
 *    energy — a per-message energy proxy measured END-TO-END: it sends M DATA
 *            messages of payload P to a near sink and a far sink, counts the
 *            REAL bytes that traverse the relay wire (2 hops: src->relay,
 *            relay->dst), and reports both the measured byte totals and a
 *            MODELLED joule figure (J/byte radio cost) with the far-link
 *            weight K. Measured (bytes) and modelled (joules, K) are split.
 *
 *    stampcheck — sends ONE probe keepalive and prints how many extra bytes
 *            the relay appended (0 => verbatim/flag off, 16 => stamped). Used
 *            by run.sh to PROVE the wire change is additive and off-by-default.
 *
 *  Same v2 wire as the kernel's net_relay.c (HEAD 12 + AUTH 24 + payload).
 *
 *  Usage:
 *    ./measure rtt        <port>
 *    ./measure energy     <port> [M] [P]
 *    ./measure stampcheck <port>
 *  Key via PKERNEL_RELAY_KEY (64 hex chars). Exit 0 on success.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "sha256.h"

#define MAGIC      0x52454C59U
#define VER_V2     2
#define HEAD       12
#define AUTH       24
#define HMAC16     16
#define FRAME      (HEAD + AUTH)     /* per-message v2 framing overhead = 36 B */
#define REL_REG    1
#define REL_DATA   2
#define REL_KA     3

#define SHIP   1
#define NEAR   2
#define FAR    3

/* ---- modelled constants (NOT measured — labelled honestly in output) ----- */
#define HOPS_PER_MSG        2        /* src->relay, relay->dst (relay topology) */
#define ENERGY_FAR_WEIGHT   5        /* K: far link byte costs ~K x a near one  */
                                     /* (locality.md §4: K=(tau+penalty)/tau=5) */
#define RADIO_NJ_PER_BYTE   1000.0   /* ~1 uJ/byte: order-of-magnitude mobile   */
                                     /* radio TX energy. ABSOLUTE JOULES MODEL. */

static uint8_t KEY[32];

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int hex_nib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void store_u64_le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (i * 8)) & 0xff);
}
static uint64_t load_u64_le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

/* Build a v2 packet with valid HMAC (MAC over ver||type||src||dst||nonce||payload). */
static int pkt_make(unsigned char *buf, unsigned type, unsigned src, unsigned dst,
                    uint64_t nonce, const void *payload, int plen)
{
    buf[0] = (uint8_t)(MAGIC      & 0xff);
    buf[1] = (uint8_t)((MAGIC>>8) & 0xff);
    buf[2] = (uint8_t)((MAGIC>>16)& 0xff);
    buf[3] = (uint8_t)((MAGIC>>24)& 0xff);
    buf[4] = VER_V2; buf[5] = (uint8_t)type; buf[6] = (uint8_t)src; buf[7] = (uint8_t)dst;
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    store_u64_le(buf + HEAD, nonce);

    uint8_t pre[12];
    pre[0] = VER_V2; pre[1] = (uint8_t)type; pre[2] = (uint8_t)src; pre[3] = (uint8_t)dst;
    store_u64_le(pre + 4, nonce);

    sha256_ctx c;
    uint8_t k[SHA256_BLOCK_SIZE], ipad[SHA256_BLOCK_SIZE], opad[SHA256_BLOCK_SIZE];
    uint8_t inner[SHA256_DIGEST_SIZE], outer[SHA256_DIGEST_SIZE];
    memcpy(k, KEY, 32); memset(k + 32, 0, SHA256_BLOCK_SIZE - 32);
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    sha256_init(&c); sha256_update(&c, ipad, SHA256_BLOCK_SIZE);
    sha256_update(&c, pre, sizeof(pre));
    if (plen > 0) sha256_update(&c, payload, (size_t)plen);
    sha256_final(&c, inner);
    sha256_init(&c); sha256_update(&c, opad, SHA256_BLOCK_SIZE);
    sha256_update(&c, inner, SHA256_DIGEST_SIZE); sha256_final(&c, outer);
    memcpy(buf + HEAD + 8, outer, HMAC16);

    if (payload && plen > 0) memcpy(buf + HEAD + AUTH, payload, (size_t)plen);
    return HEAD + AUTH + plen;
}

static struct sockaddr_in g_relay;
static uint64_t g_nonce[4] = {0, 1, 1, 1};   /* per-src nonce counters */

static int open_sock(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return -1; }
    int big = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    struct sockaddr_in any = {0}; any.sin_family = AF_INET;
    if (bind(s, (struct sockaddr *)&any, sizeof(any)) < 0) { perror("bind"); close(s); return -1; }
    return s;
}

static int send_pkt(int s, unsigned type, unsigned src, unsigned dst,
                    const void *payload, int plen)
{
    unsigned char buf[1500];
    int n = pkt_make(buf, type, src, dst, g_nonce[src]++, payload, plen);
    return (int)sendto(s, buf, (size_t)n, 0, (struct sockaddr *)&g_relay, sizeof(g_relay));
}

static int load_key(void)
{
    const char *hex = getenv("PKERNEL_RELAY_KEY");
    if (!hex || strlen(hex) != 64) {
        fprintf(stderr, "PKERNEL_RELAY_KEY must be 64 hex chars\n"); return -1;
    }
    for (int i = 0; i < 32; i++) {
        int hi = hex_nib(hex[i*2]), lo = hex_nib(hex[i*2+1]);
        if (hi < 0 || lo < 0) { fprintf(stderr, "bad key hex\n"); return -1; }
        KEY[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static void set_relay(int port)
{
    memset(&g_relay, 0, sizeof(g_relay));
    g_relay.sin_family = AF_INET;
    g_relay.sin_addr.s_addr = htonl(0x7F000001);
    g_relay.sin_port = htons((uint16_t)port);
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}
static double pct(double *v, int n, double p)   /* v must be sorted ascending */
{
    if (n <= 0) return 0;
    int idx = (int)(p / 100.0 * (n - 1) + 0.5);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return v[idx];
}

/* ---- probe payload: "PRB1" magic + 8-byte client send-stamp(us) + 4-byte seq */
#define PROBE_MAGIC "PRB1"
#define PROBE_LEN   16                 /* 4 magic + 8 ts + 4 seq */

/* ========================================================================= *
 *  rtt — relay forwarding RTT vs offered load (burst depth)
 * ========================================================================= */
static int do_rtt(int port)
{
    (void)port;
    int s = open_sock();
    if (s < 0) return 1;
    send_pkt(s, REL_REG, SHIP, 0, NULL, 0);
    usleep(200 * 1000);

    const int bursts[] = { 1, 16, 64, 256, 1024 };
    const int NB = (int)(sizeof(bursts) / sizeof(bursts[0]));

    printf("[rtt] relay forwarding RTT (ship->relay->ship via KEEPALIVE echo), "
           "REAL UDP, REAL relay\n");
    printf("[rtt] %-8s %-6s %8s %8s %8s %8s %8s %10s\n",
           "offered", "recv", "min", "mean", "p50", "p95", "max", "resid_mean");
    printf("[rtt] %-8s %-6s %8s %8s %8s %8s %8s %10s\n",
           "(depth)", "", "(ms)", "(ms)", "(ms)", "(ms)", "(ms)", "(ms)");

    int ok = 1;
    double idle_mean = -1, load_mean = -1;

    for (int bi = 0; bi < NB; bi++) {
        int B = bursts[bi];
        double *rtt = calloc((size_t)B, sizeof(double));
        double *res = calloc((size_t)B, sizeof(double));
        int got = 0, got_res = 0;

        /* Fire the whole burst back-to-back (offered load = B in flight). */
        uint64_t *sent_at = calloc((size_t)B, sizeof(uint64_t));
        for (int i = 0; i < B; i++) {
            unsigned char pl[PROBE_LEN];
            memcpy(pl, PROBE_MAGIC, 4);
            uint64_t t = now_us();
            store_u64_le(pl + 4, t);
            for (int k = 0; k < 4; k++) pl[12 + k] = (uint8_t)((i >> (k * 8)) & 0xff);
            sent_at[i] = t;
            send_pkt(s, REL_KA, SHIP, 0, pl, PROBE_LEN);
        }

        /* Drain echoes until quiet (200 ms idle) or all received. */
        struct pollfd pfd = { .fd = s, .events = POLLIN };
        for (;;) {
            int pr = poll(&pfd, 1, 200);
            if (pr <= 0) break;                  /* timeout => burst drained */
            unsigned char buf[1500];
            ssize_t n = recv(s, buf, sizeof(buf), 0);
            if (n < FRAME + PROBE_LEN) continue;
            const unsigned char *p = buf + FRAME;
            if (memcmp(p, PROBE_MAGIC, 4) != 0) continue;
            uint64_t ts = load_u64_le(p + 4);
            double r = (double)(now_us() - ts) / 1000.0;
            if (got < B) rtt[got++] = r;
            /* relay residence, if the relay appended its rx/tx stamps. */
            if (n >= (ssize_t)(FRAME + PROBE_LEN + 16)) {
                uint64_t rx = load_u64_le(buf + n - 16);
                uint64_t tx = load_u64_le(buf + n - 8);
                if (tx >= rx) res[got_res++] = (double)(tx - rx) / 1000.0;
            }
            if (got >= B) break;
        }

        qsort(rtt, (size_t)got, sizeof(double), cmp_dbl);
        double mn = got ? rtt[0] : 0, mx = got ? rtt[got-1] : 0, sum = 0;
        for (int i = 0; i < got; i++) sum += rtt[i];
        double mean = got ? sum / got : 0;
        double rsum = 0; for (int i = 0; i < got_res; i++) rsum += res[i];
        double rmean = got_res ? rsum / got_res : -1;

        printf("[rtt] %-8d %-6d %8.3f %8.3f %8.3f %8.3f %8.3f ",
               B, got, mn, mean, pct(rtt, got, 50), pct(rtt, got, 95), mx);
        if (rmean >= 0) printf("%10.3f\n", rmean); else printf("%10s\n", "n/a");

        if (got < B) { /* loss under load is reported, not fatal on localhost */
            fprintf(stderr, "[rtt]   note: %d/%d echoes returned (UDP loss at depth %d)\n",
                    got, B, B);
        }
        if (B == 1)    idle_mean = mean;
        if (B == 1024) load_mean = mean;

        free(rtt); free(res); free(sent_at);
    }
    close(s);

    /* Sanity assertions: we actually measured something, and it is fast. */
    if (idle_mean < 0 || idle_mean > 50.0) ok = 0;   /* idle relay RTT < 50ms */
    if (load_mean < 0) ok = 0;
    printf("RESULT: %s  idle_rtt_mean=%.3fms load_rtt_mean(depth1024)=%.3fms\n",
           ok ? "PASS" : "FAIL", idle_mean, load_mean);
    return ok ? 0 : 1;
}

/* ========================================================================= *
 *  energy — per-message energy proxy measured end-to-end
 * ========================================================================= */
static void drain_into(int s, uint64_t *bytes, uint64_t *msgs, int ms)
{
    struct pollfd pfd = { .fd = s, .events = POLLIN };
    for (;;) {
        int pr = poll(&pfd, 1, ms);
        if (pr <= 0) return;
        unsigned char buf[2048];
        ssize_t n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return;
        *bytes += (uint64_t)n;
        (*msgs)++;
    }
}

static int do_energy(int port, int M, int P)
{
    (void)port;
    int ssend = open_sock(), snear = open_sock(), sfar = open_sock();
    if (ssend < 0 || snear < 0 || sfar < 0) return 1;
    send_pkt(ssend, REL_REG, SHIP, 0, NULL, 0);
    send_pkt(snear, REL_REG, NEAR, 0, NULL, 0);
    send_pkt(sfar,  REL_REG, FAR,  0, NULL, 0);
    usleep(250 * 1000);

    unsigned char *pl = malloc((size_t)P);
    memset(pl, 0xAB, (size_t)P);

    uint64_t deliv_bytes_near = 0, deliv_msgs_near = 0;
    uint64_t deliv_bytes_far  = 0, deliv_msgs_far  = 0;

    uint64_t sent_bytes_near = 0, sent_bytes_far = 0;
    /* near leg: M messages ship->NEAR. Drain the sink inline so neither the
     * relay's nor the sink's socket buffer overflows under a sustained burst
     * (that loss would corrupt the byte accounting, not just the latency). */
    for (int i = 0; i < M; i++) {
        int n = send_pkt(ssend, REL_DATA, SHIP, NEAR, pl, P);
        if (n > 0) sent_bytes_near += (uint64_t)n;
        if ((i & 0x1f) == 0) {
            usleep(150);                                    /* gentle pacing */
            drain_into(snear, &deliv_bytes_near, &deliv_msgs_near, 0);
        }
    }
    drain_into(snear, &deliv_bytes_near, &deliv_msgs_near, 400);
    /* far leg: M messages ship->FAR */
    for (int i = 0; i < M; i++) {
        int n = send_pkt(ssend, REL_DATA, SHIP, FAR, pl, P);
        if (n > 0) sent_bytes_far += (uint64_t)n;
        if ((i & 0x1f) == 0) {
            usleep(150);
            drain_into(sfar, &deliv_bytes_far, &deliv_msgs_far, 0);
        }
    }
    drain_into(sfar,  &deliv_bytes_far,  &deliv_msgs_far,  400);

    free(pl);
    close(ssend); close(snear); close(sfar);

    /* MEASURED: real wire bytes per leg = send leg + delivery leg. */
    uint64_t wire_near = sent_bytes_near + deliv_bytes_near;
    uint64_t wire_far  = sent_bytes_far  + deliv_bytes_far;
    uint64_t app_near  = (uint64_t)M * (uint64_t)P;   /* application payload bytes */

    /* MODELLED: joules, and the far-link weight K. */
    double E_near_J = (double)wire_near * RADIO_NJ_PER_BYTE * 1e-9;
    double E_far_J  = (double)wire_far  * RADIO_NJ_PER_BYTE * 1e-9 * ENERGY_FAR_WEIGHT;
    /* locality.md-style byte proxy from REAL wire bytes (vs its kdds counters). */
    double Eproxy_near = (double)wire_near * 1.0;
    double Eproxy_far  = (double)wire_far  * (double)ENERGY_FAR_WEIGHT;

    printf("[energy] per-message energy proxy, REAL bytes over REAL relay "
           "(M=%d msgs/leg, P=%d payload B)\n", M, P);
    printf("[energy] --- MEASURED (real socket byte counts) ---\n");
    printf("[energy]   v2 framing overhead     : %d B/msg (HEAD %d + AUTH %d)\n",
           FRAME, HEAD, AUTH);
    printf("[energy]   NEAR  sent=%llu B  delivered=%llu B (%llu msgs)  wire=%llu B\n",
           (unsigned long long)sent_bytes_near, (unsigned long long)deliv_bytes_near,
           (unsigned long long)deliv_msgs_near, (unsigned long long)wire_near);
    printf("[energy]   FAR   sent=%llu B  delivered=%llu B (%llu msgs)  wire=%llu B\n",
           (unsigned long long)sent_bytes_far, (unsigned long long)deliv_bytes_far,
           (unsigned long long)deliv_msgs_far, (unsigned long long)wire_far);
    printf("[energy]   wire amplification (wire/app) NEAR=%.2fx  "
           "(2 hops x (%d+P)/P)\n",
           app_near ? (double)wire_near / app_near : 0.0, FRAME);
    printf("[energy] --- MODELLED (joule conversion %.0f nJ/B, far weight K=%d) ---\n",
           RADIO_NJ_PER_BYTE, ENERGY_FAR_WEIGHT);
    printf("[energy]   E_proxy(byte) NEAR=%.0f  FAR=%.0f  (FAR/NEAR=%.2fx via K)\n",
           Eproxy_near, Eproxy_far, Eproxy_near ? Eproxy_far / Eproxy_near : 0.0);
    printf("[energy]   E(joule) NEAR=%.6f J  FAR=%.6f J  per-msg NEAR=%.3f uJ\n",
           E_near_J, E_far_J, M ? E_near_J / M * 1e6 : 0.0);

    /* End-to-end success: delivery legs actually moved the bytes we sent
     * (allow modest localhost UDP loss). */
    int ok = (deliv_msgs_near >= (uint64_t)(M * 0.9))
          && (deliv_msgs_far  >= (uint64_t)(M * 0.9));
    double loss_near = M ? 100.0 * (1.0 - (double)deliv_msgs_near / M) : 100.0;
    double loss_far  = M ? 100.0 * (1.0 - (double)deliv_msgs_far  / M) : 100.0;
    printf("RESULT: %s  wire_near=%lluB wire_far=%lluB hops=%d "
           "loss_near=%.1f%% loss_far=%.1f%%\n",
           ok ? "PASS" : "FAIL",
           (unsigned long long)wire_near, (unsigned long long)wire_far,
           HOPS_PER_MSG, loss_near, loss_far);
    return ok ? 0 : 1;
}

/* ========================================================================= *
 *  stampcheck — prove the wire change is additive + off-by-default
 * ========================================================================= */
static int do_stampcheck(int port)
{
    (void)port;
    int s = open_sock();
    if (s < 0) return 1;
    send_pkt(s, REL_REG, SHIP, 0, NULL, 0);
    usleep(150 * 1000);

    unsigned char pl[PROBE_LEN];
    memcpy(pl, PROBE_MAGIC, 4);
    store_u64_le(pl + 4, now_us());
    pl[12] = pl[13] = pl[14] = pl[15] = 0;
    int sent = send_pkt(s, REL_KA, SHIP, 0, pl, PROBE_LEN);

    struct pollfd pfd = { .fd = s, .events = POLLIN };
    int extra = -1;
    if (poll(&pfd, 1, 1000) > 0) {
        unsigned char buf[1500];
        ssize_t n = recv(s, buf, sizeof(buf), 0);
        if (n >= sent) extra = (int)n - sent;
    }
    close(s);
    if (extra < 0) { printf("stampcheck: NO ECHO (relay down?)\n"); return 1; }
    printf("stampcheck: echo_extra=%d B\n", extra);
    return 0;          /* run.sh interprets the value */
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s {rtt|energy|stampcheck} <port> [M] [P]\n", argv[0]);
        return 2;
    }
    if (load_key() < 0) return 2;
    int port = atoi(argv[2]);
    set_relay(port);

    if (strcmp(argv[1], "rtt") == 0)        return do_rtt(port);
    if (strcmp(argv[1], "stampcheck") == 0) return do_stampcheck(port);
    if (strcmp(argv[1], "energy") == 0) {
        int M = (argc > 3) ? atoi(argv[3]) : 2000;
        int P = (argc > 4) ? atoi(argv[4]) : 256;
        if (M <= 0) M = 2000;
        if (P <= 0 || P > 1380) P = 256;
        return do_energy(port, M, P);
    }
    fprintf(stderr, "unknown mode '%s'\n", argv[1]);
    return 2;
}
