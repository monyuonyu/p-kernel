/*
 *  samples/29_latency/latency_client.c — two-layer latency against the REAL relay.
 *
 *  survival-network.md §8: the reflex layer (near, low-delay) must stay
 *  immediate even while the deliberation layer (far, high-delay) lags. This
 *  harness *measures* that on a real UDP socket round-trip through the real
 *  ./relay binary, with the relay's distance→delay knob turned on
 *  (RELAY_FAR_NODES / RELAY_FAR_DELAY_MS).
 *
 *  Topology (all sockets held by this one process; same v2 wire as the
 *  kernel's net_relay.c under arch/linux):
 *      node 1 = "ship"  (threatened point; near to everything it talks to)
 *      node 2 = near reflex peer   (relay forwards to it immediately)
 *      node 3 = far  deliberation peer (relay holds packets to it FAR_DELAY ms)
 *
 *  Measurement: the ship emits round-trip probes (ship -> peer -> ship). The
 *  peers echo every DATA straight back to the ship. We time each probe by a
 *  millisecond send-stamp carried in the payload.
 *
 *  The decisive test (NON-INTERFERENCE): at t=0 we fire ONE far probe, then —
 *  while it is still held in the relay's delay queue — we fire a burst of near
 *  probes every NEAR_GAP ms. If the two layers are truly separated, every near
 *  probe round-trips promptly and returns BEFORE the single far probe does.
 *  If the relay head-of-line-blocked the near layer behind the far one, the
 *  near probes would all stall until the far delay elapsed. We assert it does
 *  not. That is §8 made into a number.
 *
 *  Usage: ./latency_client <port> <far_delay_ms>   (key via PKERNEL_RELAY_KEY)
 *  Exit 0 iff all assertions hold; prints one RESULT line for the caller.
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

#define MAGIC    0x52454C59U
#define VER_V2   2
#define HEAD     12
#define AUTH     24
#define HMAC16   16
#define REL_REG  1
#define REL_DATA 2

#define SHIP     1
#define NEAR     2
#define FAR      3

#define NEAR_GAP_MS   20      /* spacing of the near burst */
#define NEAR_COUNT    12      /* near probes fired while the far probe is in flight */
#define REFLEX_BUDGET_MS 50   /* a near round-trip must beat this (<< 1000ms debris deadline) */

static uint8_t KEY[32];

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
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

/* A probe payload: 8-byte send-stamp (ms) + 1-byte class ('N'/'F') + 4-byte seq. */
struct probe { uint64_t ts; char cls; uint32_t seq; };
static void put_probe(unsigned char *p, struct probe pr)
{
    store_u64_le(p, pr.ts); p[8] = (uint8_t)pr.cls;
    for (int i = 0; i < 4; i++) p[9 + i] = (uint8_t)((pr.seq >> (i * 8)) & 0xff);
}
static struct probe get_probe(const unsigned char *p)
{
    struct probe pr; pr.ts = 0;
    for (int i = 0; i < 8; i++) pr.ts |= (uint64_t)p[i] << (i * 8);
    pr.cls = (char)p[8]; pr.seq = 0;
    for (int i = 0; i < 4; i++) pr.seq |= (uint32_t)p[9 + i] << (i * 8);
    return pr;
}
#define PROBE_LEN 13

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <port> <far_delay_ms>\n", argv[0]); return 2; }
    int port = atoi(argv[1]);
    int far_delay = atoi(argv[2]);

    const char *hex = getenv("PKERNEL_RELAY_KEY");
    if (!hex || strlen(hex) != 64) { fprintf(stderr, "PKERNEL_RELAY_KEY must be 64 hex chars\n"); return 2; }
    for (int i = 0; i < 32; i++) {
        int hi = hex_nib(hex[i*2]), lo = hex_nib(hex[i*2+1]);
        if (hi < 0 || lo < 0) { fprintf(stderr, "bad key hex\n"); return 2; }
        KEY[i] = (uint8_t)((hi << 4) | lo);
    }

    memset(&g_relay, 0, sizeof(g_relay));
    g_relay.sin_family = AF_INET;
    g_relay.sin_addr.s_addr = htonl(0x7F000001);
    g_relay.sin_port = htons((uint16_t)port);

    int s1 = open_sock(), s2 = open_sock(), s3 = open_sock();
    if (s1 < 0 || s2 < 0 || s3 < 0) return 1;

    /* Register all three. */
    send_pkt(s1, REL_REG, SHIP, 0, NULL, 0);
    send_pkt(s2, REL_REG, NEAR, 0, NULL, 0);
    send_pkt(s3, REL_REG, FAR,  0, NULL, 0);
    usleep(200 * 1000);

    /* Round-trip results. */
    double near_rtt[64]; int near_n = 0;
    double far_rtt = -1.0;
    uint64_t far_recv_ts = 0;
    uint64_t last_near_recv_ts = 0;

    /* Fire ONE far probe at t=0, then a near burst while it is in flight. */
    uint64_t t0 = now_ms();
    {
        unsigned char pl[PROBE_LEN];
        put_probe(pl, (struct probe){ now_ms(), 'F', 0 });
        send_pkt(s1, REL_DATA, SHIP, FAR, pl, PROBE_LEN);
    }

    struct pollfd pfd[3] = {
        { .fd = s1, .events = POLLIN }, { .fd = s2, .events = POLLIN }, { .fd = s3, .events = POLLIN },
    };

    int near_fired = 0;
    uint64_t next_near = t0;                 /* fire first near immediately after far */
    uint64_t deadline = t0 + (uint64_t)far_delay + 600;   /* run past the far arrival */

    while (now_ms() < deadline) {
        uint64_t t = now_ms();
        if (near_fired < NEAR_COUNT && t >= next_near) {
            unsigned char pl[PROBE_LEN];
            put_probe(pl, (struct probe){ now_ms(), 'N', (uint32_t)near_fired });
            send_pkt(s1, REL_DATA, SHIP, NEAR, pl, PROBE_LEN);
            near_fired++;
            next_near += NEAR_GAP_MS;
        }
        int to = 5;
        int pr = poll(pfd, 3, to);
        if (pr <= 0) continue;

        unsigned char buf[1500];
        /* Peers echo any DATA straight back to the ship (immediate). */
        for (int idx = 1; idx <= 2; idx++) {
            if (pfd[idx].revents & POLLIN) {
                ssize_t n = recv(pfd[idx].fd, buf, sizeof(buf), 0);
                if (n >= HEAD + AUTH + PROBE_LEN) {
                    struct probe pr2 = get_probe(buf + HEAD + AUTH);
                    unsigned src = (idx == 1) ? NEAR : FAR;
                    unsigned char pl[PROBE_LEN]; put_probe(pl, pr2);
                    send_pkt(pfd[idx].fd, REL_DATA, src, SHIP, pl, PROBE_LEN);
                }
            }
        }
        /* Ship receives echoes and times the round trip. */
        if (pfd[0].revents & POLLIN) {
            ssize_t n = recv(s1, buf, sizeof(buf), 0);
            if (n >= HEAD + AUTH + PROBE_LEN) {
                struct probe pr2 = get_probe(buf + HEAD + AUTH);
                double rtt = (double)(now_ms() - pr2.ts);
                if (pr2.cls == 'N') {
                    if (near_n < 64) near_rtt[near_n++] = rtt;
                    last_near_recv_ts = now_ms();
                } else if (pr2.cls == 'F' && far_rtt < 0) {
                    far_rtt = rtt; far_recv_ts = now_ms();
                }
            }
        }
        if (far_rtt >= 0 && near_n >= NEAR_COUNT) break;
    }

    close(s1); close(s2); close(s3);

    /* ---- stats ---- */
    double nmin = 1e9, nmax = -1, nsum = 0;
    for (int i = 0; i < near_n; i++) {
        if (near_rtt[i] < nmin) nmin = near_rtt[i];
        if (near_rtt[i] > nmax) nmax = near_rtt[i];
        nsum += near_rtt[i];
    }
    double nmean = near_n ? nsum / near_n : -1;

    /* Non-interference: every near echo must have arrived before the far echo. */
    int near_before_far = (far_recv_ts > 0 && last_near_recv_ts > 0
                           && last_near_recv_ts < far_recv_ts);

    printf("[latency] near probes=%d  RTT min/mean/max = %.1f / %.1f / %.1f ms\n",
           near_n, nmin, nmean, nmax);
    printf("[latency] far  probe   RTT = %.1f ms  (injected far_delay = %d ms)\n",
           far_rtt, far_delay);
    printf("[latency] last near echo at +%llu ms,  far echo at +%llu ms\n",
           (unsigned long long)(last_near_recv_ts - t0),
           (unsigned long long)(far_recv_ts - t0));

    /* ---- assertions ---- */
    int a_near_got   = (near_n == NEAR_COUNT);
    int a_near_fast  = (near_n > 0 && nmax < REFLEX_BUDGET_MS);
    int a_far_got    = (far_rtt >= 0);
    int a_far_lagged = (far_rtt >= far_delay * 0.7);  /* one-leg injected delay actually shows up */
    int a_noninterf  = near_before_far;
    int a_separated  = (far_rtt > nmax * 4);          /* the two time-constants are far apart */

    int ok = a_near_got && a_near_fast && a_far_got && a_far_lagged
             && a_noninterf && a_separated;

    printf("RESULT: %s  near_max=%.1fms(<%dms reflex budget) far=%.1fms "
           "noninterference=%s separation=%.0fx\n",
           ok ? "PASS" : "FAIL", nmax, REFLEX_BUDGET_MS, far_rtt,
           a_noninterf ? "yes" : "no",
           (near_n && nmax > 0) ? far_rtt / nmax : 0.0);

    if (!ok) {
        fprintf(stderr, "  [fail detail] near_got=%d near_fast=%d far_got=%d "
                "far_lagged=%d noninterf=%d separated=%d\n",
                a_near_got, a_near_fast, a_far_got, a_far_lagged, a_noninterf, a_separated);
    }
    return ok ? 0 : 1;
}
