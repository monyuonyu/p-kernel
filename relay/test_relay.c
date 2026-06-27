/*
 *  relay/test_relay.c — v2 relay test harness.
 *
 *  Six sequential scenarios drive ./relay through a 32-byte test key:
 *    1. happy        — A→B and B→A DATA round-trip with valid HMAC
 *    2. bad_hmac     — DATA with corrupted MAC is dropped by relay
 *    3. replay       — DATA with reused nonce is dropped
 *    4. out_of_window— DATA with nonce << max is dropped
 *    5. missing_key  — relay refuses to start with no key + no --insecure
 *    6. insecure_v1  — relay started with --insecure forwards v1 packets
 *                      (backward-compat path used by the existing single-
 *                      host pmesh demos)
 *
 *  Scenarios 1..4 share a single relay child (spawned once at top); the
 *  parent opens two sockets, registers them, and orchestrates each test
 *  by sendto+recv with timeouts. Scenarios 5 and 6 each spawn their own
 *  relay on a different port.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "sha256.h"
#include "tcp_frame.h"   /* SLICE 3: the shared [u16 len][pkt] de-framer */

#define MAGIC       0x52454C59U
#define VER_V1      1
#define VER_V2      2
#define HEAD        12
#define AUTH        24
#define HMAC16      16
#define PORT        27400
#define REL_REG     1
#define REL_DATA    2
#define REL_KA      3

/* Test key — 32 bytes, all 0xA5 so it's recognisable in any hex dump. */
static const uint8_t TEST_KEY[32] = {
    0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,
    0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,
    0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,
    0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,
};
static const char TEST_KEY_HEX[] =
    "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5"
    "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5";

/* --- packet helpers ------------------------------------------------------ */

static void store_u64_le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (i * 8)) & 0xff);
}

/* Build a v2 packet with valid HMAC. Returns total length. */
static int pkt_make_v2(unsigned char *buf,
                       unsigned type, unsigned src, unsigned dst,
                       uint64_t nonce,
                       const void *payload, int plen)
{
    /* HEAD(12) */
    buf[0] = (uint8_t)(MAGIC      & 0xff);
    buf[1] = (uint8_t)((MAGIC>>8) & 0xff);
    buf[2] = (uint8_t)((MAGIC>>16)& 0xff);
    buf[3] = (uint8_t)((MAGIC>>24)& 0xff);
    buf[4] = VER_V2;
    buf[5] = (uint8_t)type;
    buf[6] = (uint8_t)src;
    buf[7] = (uint8_t)dst;
    buf[8] = buf[9] = buf[10] = buf[11] = 0;

    /* AUTH(24) — nonce + hmac16. MAC covers ver||type||src||dst||nonce||payload. */
    store_u64_le(buf + HEAD, nonce);

    uint8_t preamble[12];
    preamble[0] = VER_V2;
    preamble[1] = (uint8_t)type;
    preamble[2] = (uint8_t)src;
    preamble[3] = (uint8_t)dst;
    store_u64_le(preamble + 4, nonce);

    sha256_ctx c;
    uint8_t k[SHA256_BLOCK_SIZE];
    uint8_t ipad[SHA256_BLOCK_SIZE];
    uint8_t opad[SHA256_BLOCK_SIZE];
    uint8_t inner[SHA256_DIGEST_SIZE];
    uint8_t outer[SHA256_DIGEST_SIZE];
    memcpy(k, TEST_KEY, sizeof(TEST_KEY));
    memset(k + sizeof(TEST_KEY), 0, SHA256_BLOCK_SIZE - sizeof(TEST_KEY));
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    sha256_init(&c);
    sha256_update(&c, ipad, SHA256_BLOCK_SIZE);
    sha256_update(&c, preamble, sizeof(preamble));
    if (plen > 0) sha256_update(&c, payload, (size_t)plen);
    sha256_final(&c, inner);
    sha256_init(&c);
    sha256_update(&c, opad, SHA256_BLOCK_SIZE);
    sha256_update(&c, inner, SHA256_DIGEST_SIZE);
    sha256_final(&c, outer);

    memcpy(buf + HEAD + 8, outer, HMAC16);

    if (payload && plen > 0) memcpy(buf + HEAD + AUTH, payload, (size_t)plen);
    return HEAD + AUTH + plen;
}

/* --- socket helpers ----------------------------------------------------- */

static int open_client_socket(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return -1; }
    struct sockaddr_in any = {0};
    any.sin_family = AF_INET;
    if (bind(s, (struct sockaddr *)&any, sizeof(any)) < 0) {
        perror("bind"); close(s); return -1;
    }
    return s;
}

static void relay_addr(struct sockaddr_in *out)
{
    memset(out, 0, sizeof(*out));
    out->sin_family      = AF_INET;
    out->sin_addr.s_addr = htonl(0x7F000001);
    out->sin_port        = htons(PORT);
}

/* Send packet to relay. */
static int send_to_relay(int s, const unsigned char *buf, int len)
{
    struct sockaddr_in to;
    relay_addr(&to);
    return (int)sendto(s, buf, (size_t)len, 0,
                       (struct sockaddr *)&to, sizeof(to));
}

/* Receive with timeout. Returns payload length, -1 on timeout, -2 on error. */
static int recv_with_timeout(int s, unsigned char *buf, int buflen, int ms)
{
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(s, buf, (size_t)buflen, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -2;
    }
    return (int)n;
}

/* Drain any leftover packets so a previous test doesn't bleed into the next. */
static void drain(int s)
{
    unsigned char buf[1500];
    for (;;) {
        int r = recv_with_timeout(s, buf, sizeof(buf), 50);
        if (r < 0) return;
    }
}

/* --- scenarios ---------------------------------------------------------- */

static int test_happy(int sa, int sb)
{
    unsigned char buf[1500];
    int n;

    /* REGISTER both with non-overlapping nonces. */
    n = pkt_make_v2(buf, REL_REG, 1, 0, 1, NULL, 0);
    if (send_to_relay(sa, buf, n) < 0) { perror("sendto A REG"); return 1; }
    n = pkt_make_v2(buf, REL_REG, 2, 0, 1, NULL, 0);
    if (send_to_relay(sb, buf, n) < 0) { perror("sendto B REG"); return 1; }
    usleep(150 * 1000);

    const char *msg_ab = "hello from A";
    n = pkt_make_v2(buf, REL_DATA, 1, 2, 2, msg_ab, (int)strlen(msg_ab));
    if (send_to_relay(sa, buf, n) < 0) { perror("sendto A DATA"); return 1; }

    int got = recv_with_timeout(sb, buf, sizeof(buf), 1000);
    if (got < HEAD + AUTH + (int)strlen(msg_ab)) {
        fprintf(stderr, "[happy] B receive failed (got=%d)\n", got);
        return 1;
    }
    if (memcmp(buf + HEAD + AUTH, msg_ab, strlen(msg_ab)) != 0) {
        fprintf(stderr, "[happy] B payload mismatch\n");
        return 1;
    }

    const char *msg_ba = "hello from B";
    n = pkt_make_v2(buf, REL_DATA, 2, 1, 2, msg_ba, (int)strlen(msg_ba));
    if (send_to_relay(sb, buf, n) < 0) { perror("sendto B DATA"); return 1; }

    got = recv_with_timeout(sa, buf, sizeof(buf), 1000);
    if (got < HEAD + AUTH + (int)strlen(msg_ba)) {
        fprintf(stderr, "[happy] A receive failed (got=%d)\n", got);
        return 1;
    }
    if (memcmp(buf + HEAD + AUTH, msg_ba, strlen(msg_ba)) != 0) {
        fprintf(stderr, "[happy] A payload mismatch\n");
        return 1;
    }
    fprintf(stderr, "[happy] PASS — A↔B round-trip with valid HMAC\n");
    return 0;
}

static int test_bad_hmac(int sa, int sb)
{
    unsigned char buf[1500];
    const char *msg = "should not arrive";

    /* Build a valid packet, then flip one HMAC byte. Use a fresh nonce so
     * replay isn't the reason for the drop. */
    int n = pkt_make_v2(buf, REL_DATA, 1, 2, 10, msg, (int)strlen(msg));
    buf[HEAD + 8] ^= 0x01;   /* corrupt first MAC byte */

    if (send_to_relay(sa, buf, n) < 0) { perror("sendto bad-mac"); return 1; }

    int got = recv_with_timeout(sb, buf, sizeof(buf), 500);
    if (got >= 0) {
        fprintf(stderr, "[bad_hmac] B received %d B — should have been dropped\n", got);
        return 1;
    }
    fprintf(stderr, "[bad_hmac] PASS — corrupted MAC dropped\n");
    return 0;
}

static int test_replay(int sa, int sb)
{
    unsigned char buf[1500];
    const char *msg = "first send only";

    int n = pkt_make_v2(buf, REL_DATA, 1, 2, 20, msg, (int)strlen(msg));

    /* Send #1 — should arrive. */
    if (send_to_relay(sa, buf, n) < 0) { perror("sendto replay #1"); return 1; }
    int got = recv_with_timeout(sb, buf, sizeof(buf), 1000);
    if (got < HEAD + AUTH + (int)strlen(msg)) {
        fprintf(stderr, "[replay] B missed legit send (got=%d)\n", got);
        return 1;
    }

    /* Send #2 — identical packet, replay. Must be dropped. */
    n = pkt_make_v2(buf, REL_DATA, 1, 2, 20, msg, (int)strlen(msg));
    if (send_to_relay(sa, buf, n) < 0) { perror("sendto replay #2"); return 1; }
    got = recv_with_timeout(sb, buf, sizeof(buf), 500);
    if (got >= 0) {
        fprintf(stderr, "[replay] B received replayed packet — should drop\n");
        return 1;
    }
    fprintf(stderr, "[replay] PASS — duplicate nonce dropped\n");
    return 0;
}

static int test_out_of_window(int sa, int sb)
{
    unsigned char buf[1500];
    const char *msg = "high water mark";

    /* Advance the window with a high nonce. */
    int n = pkt_make_v2(buf, REL_DATA, 1, 2, 1000, msg, (int)strlen(msg));
    if (send_to_relay(sa, buf, n) < 0) { perror("sendto OoW #1"); return 1; }
    int got = recv_with_timeout(sb, buf, sizeof(buf), 1000);
    if (got < HEAD + AUTH + (int)strlen(msg)) {
        fprintf(stderr, "[out_of_window] B missed high-nonce send (got=%d)\n", got);
        return 1;
    }

    /* Now send with a nonce far below max-64. Must be dropped. */
    const char *late = "ancient";
    n = pkt_make_v2(buf, REL_DATA, 1, 2, 100, late, (int)strlen(late));
    if (send_to_relay(sa, buf, n) < 0) { perror("sendto OoW #2"); return 1; }
    got = recv_with_timeout(sb, buf, sizeof(buf), 500);
    if (got >= 0) {
        fprintf(stderr, "[out_of_window] B received out-of-window packet (got=%d)\n", got);
        return 1;
    }
    fprintf(stderr, "[out_of_window] PASS — nonce << max dropped\n");
    return 0;
}

/* --- scenario 5: missing_key startup refusal ---------------------------- */

static int test_missing_key(void)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork mk"); return 1; }
    if (pid == 0) {
        /* Child: unset key, no --insecure, expect immediate exit(2). */
        unsetenv("PKERNEL_RELAY_KEY");
        /* Redirect stderr to /dev/null to keep test output clean. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
        execl("./relay", "./relay", "-p", "27490", (char *)NULL);
        _exit(127);
    }
    int st = 0;
    /* Give it up to 1s to refuse. */
    for (int i = 0; i < 20; i++) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        usleep(50 * 1000);
    }
    if (!WIFEXITED(st)) {
        kill(pid, SIGKILL); waitpid(pid, NULL, 0);
        fprintf(stderr, "[missing_key] FAIL — relay did not exit promptly\n");
        return 1;
    }
    if (WEXITSTATUS(st) != 2) {
        fprintf(stderr, "[missing_key] FAIL — wrong exit code %d (want 2)\n",
                WEXITSTATUS(st));
        return 1;
    }
    fprintf(stderr, "[missing_key] PASS — relay refused to start without key\n");
    return 0;
}

/* --- scenario 6: --insecure + v1 wire ---------------------------------- */

/* Build a v1 packet (no AUTH block). Returns total length. */
static int pkt_make_v1(unsigned char *buf,
                       unsigned type, unsigned src, unsigned dst,
                       const void *payload, int plen)
{
    buf[0] = (uint8_t)(MAGIC      & 0xff);
    buf[1] = (uint8_t)((MAGIC>>8) & 0xff);
    buf[2] = (uint8_t)((MAGIC>>16)& 0xff);
    buf[3] = (uint8_t)((MAGIC>>24)& 0xff);
    buf[4] = VER_V1;
    buf[5] = (uint8_t)type;
    buf[6] = (uint8_t)src;
    buf[7] = (uint8_t)dst;
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    if (payload && plen > 0) memcpy(buf + HEAD, payload, (size_t)plen);
    return HEAD + plen;
}

static int test_insecure_v1(void)
{
    const int port = 27410;
    pid_t pid = fork();
    if (pid < 0) { perror("fork v1"); return 1; }
    if (pid == 0) {
        unsetenv("PKERNEL_RELAY_KEY");
        char p[16]; snprintf(p, sizeof(p), "%d", port);
        execl("./relay", "./relay", "--insecure", "-p", p, "-v", (char *)NULL);
        _exit(127);
    }
    usleep(400 * 1000);

    int sa = open_client_socket();
    int sb = open_client_socket();
    if (sa < 0 || sb < 0) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        return 1;
    }

    /* Override default relay_addr port for this test by sendto-ing manually. */
    struct sockaddr_in to = {0};
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = htonl(0x7F000001);
    to.sin_port        = htons(port);

    unsigned char buf[1500];
    int n;
    int rv = 0;

    n = pkt_make_v1(buf, REL_REG, 1, 0, NULL, 0);
    if (sendto(sa, buf, (size_t)n, 0, (struct sockaddr*)&to, sizeof(to)) < 0) goto fail;
    n = pkt_make_v1(buf, REL_REG, 2, 0, NULL, 0);
    if (sendto(sb, buf, (size_t)n, 0, (struct sockaddr*)&to, sizeof(to)) < 0) goto fail;
    usleep(150 * 1000);

    const char *msg = "v1 plaintext";
    n = pkt_make_v1(buf, REL_DATA, 1, 2, msg, (int)strlen(msg));
    if (sendto(sa, buf, (size_t)n, 0, (struct sockaddr*)&to, sizeof(to)) < 0) goto fail;

    int got = recv_with_timeout(sb, buf, sizeof(buf), 1000);
    if (got < HEAD + (int)strlen(msg)
        || memcmp(buf + HEAD, msg, strlen(msg)) != 0) {
        fprintf(stderr, "[insecure_v1] B did not receive v1 payload (got=%d)\n", got);
        rv = 1;
    } else {
        fprintf(stderr, "[insecure_v1] PASS — v1 packet forwarded in --insecure mode\n");
    }

    goto cleanup;
fail:
    perror("insecure_v1 sendto");
    rv = 1;
cleanup:
    close(sa); close(sb);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    return rv;
}

/* --- SLICE 3 scenario 7: de-framer unit cert ---------------------------- */
/*
 *  The load-bearing claim of tcp_frame.h: a length-prefixed TCP byte stream
 *  re-splits into the EXACT original packets no matter how TCP chops the
 *  stream, because the 2-byte length prefix carries the boundary a raw byte
 *  stream lacks. This cert proves it AND its falsifier: a naive concat with
 *  NO length prefix mis-reads the boundary and the recovery fails.
 */

/* Feed `stream` (length slen) through a TcpReasm in `chunk`-byte bites
 * (chunk<=0 = all at once) and return 1 iff it recovers EXACTLY the two
 * expected payloads, byte-identical. */
static int deframe_recovers(const unsigned char *stream, int slen, int chunk,
                            const unsigned char *e1, int l1,
                            const unsigned char *e2, int l2)
{
    TcpReasm r;
    tcp_reasm_init(&r);
    unsigned char got[2][2048];
    int gotlen[2] = { -1, -1 };
    int ngot = 0;
    int off  = 0;
    while (off < slen) {
        int take = (chunk <= 0) ? (slen - off) : chunk;
        if (take > slen - off) take = slen - off;
        int pushed = tcp_reasm_push(&r, stream + off, take);
        off += pushed;
        if (pushed < take) break;                  /* reasm buffer full        */
        for (;;) {
            unsigned char out[2048];
            int plen = tcp_reasm_next(&r, out, (int)sizeof(out));
            if (plen == 0) break;                  /* need more bytes          */
            if (plen < 0)  return 0;               /* frame > out cap          */
            if (ngot < 2) { memcpy(got[ngot], out, (size_t)plen); gotlen[ngot] = plen; }
            ngot++;
        }
    }
    if (ngot != 2) return 0;
    if (gotlen[0] != l1 || memcmp(got[0], e1, (size_t)l1) != 0) return 0;
    if (gotlen[1] != l2 || memcmp(got[1], e2, (size_t)l2) != 0) return 0;
    return 1;
}

static int test_deframer(void)
{
    /* Two distinct payloads with DIFFERENT lengths. p1's first two bytes are
     * forced to 0x00 0x05 so a NAIVE concat (no length prefix) mis-reads the
     * frame length as 5 and mis-splits — giving the falsifier real teeth. */
    unsigned char p1[19], p2[37];
    for (int i = 0; i < (int)sizeof(p1); i++) p1[i] = (unsigned char)(0x41 + (i % 26));
    for (int i = 0; i < (int)sizeof(p2); i++) p2[i] = (unsigned char)(0x80 ^ (i * 7));
    p1[0] = 0x00; p1[1] = 0x05;

    /* Correctly framed stream: [len p1][p1][len p2][p2]. */
    unsigned char framed[2 * TCP_FRAME_LENPFX + sizeof(p1) + sizeof(p2)];
    int o = 0;
    o += tcp_frame_encode(framed + o, p1, (int)sizeof(p1));
    o += tcp_frame_encode(framed + o, p2, (int)sizeof(p2));

    int ok_1 = deframe_recovers(framed, o, 1, p1, sizeof(p1), p2, sizeof(p2));
    int ok_3 = deframe_recovers(framed, o, 3, p1, sizeof(p1), p2, sizeof(p2));
    int ok_7 = deframe_recovers(framed, o, 7, p1, sizeof(p1), p2, sizeof(p2));
    int ok_all = deframe_recovers(framed, o, 0, p1, sizeof(p1), p2, sizeof(p2));

    /* FALSIFIER: a naive concat with NO length prefix must FAIL to recover —
     * proving the length prefix is load-bearing, not decoration. */
    unsigned char naive[sizeof(p1) + sizeof(p2)];
    memcpy(naive, p1, sizeof(p1));
    memcpy(naive + sizeof(p1), p2, sizeof(p2));
    int falsifier_recovers =
        deframe_recovers(naive, (int)sizeof(naive), 1, p1, sizeof(p1), p2, sizeof(p2));

    if (!(ok_1 && ok_3 && ok_7 && ok_all) || falsifier_recovers) {
        fprintf(stderr, "[deframer] FAIL — 1=%d 3=%d 7=%d all=%d falsifier_recovers=%d\n",
                ok_1, ok_3, ok_7, ok_all, falsifier_recovers);
        return 1;
    }
    fprintf(stderr, "[deframer] PASS — length-framed stream re-splits to the exact "
                    "2 payloads under 1/3/7/all-byte chunking\n");
    fprintf(stderr, "[deframer] FALSIFIER FAILS as required — naive concat (no length "
                    "prefix) does NOT recover the payloads (recovered=%d, want 0)\n",
                    falsifier_recovers);
    return 0;
}

/* --- SLICE 3 scenario 8: relay TCP↔UDP round-trip ----------------------- */

/* Write one length-framed packet to a TCP stream (blocking, drains partials). */
static int tcp_send_framed_test(int fd, const unsigned char *pkt, int len)
{
    unsigned char out[TCP_FRAME_LENPFX + 2048];
    int total = tcp_frame_encode(out, pkt, len);
    if (total < 0) return -1;
    int off = 0;
    while (off < total) {
        ssize_t w = send(fd, out + off, (size_t)(total - off), MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += (int)w;
    }
    return 0;
}

/* Receive ONE length-framed packet with timeout, reassembling across reads. */
static int tcp_recv_framed_test(int fd, TcpReasm *r,
                                unsigned char *out, int outcap, int ms)
{
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (;;) {
        int plen = tcp_reasm_next(r, out, outcap);
        if (plen > 0) return plen;
        if (plen < 0) return -2;
        unsigned char tmp[2048];
        ssize_t k = recv(fd, tmp, sizeof(tmp), 0);
        if (k == 0) return -3;
        if (k < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
            return -2;
        }
        tcp_reasm_push(r, tmp, (int)k);
    }
}

static int test_tcp_roundtrip(void)
{
    const int port = 27420;
    pid_t pid = fork();
    if (pid < 0) { perror("fork tcp"); return 1; }
    if (pid == 0) {
        setenv("PKERNEL_RELAY_KEY", TEST_KEY_HEX, 1);
        char p[16]; snprintf(p, sizeof(p), "%d", port);
        execl("./relay", "./relay", "-p", p, "-v", (char *)NULL);
        _exit(127);
    }
    usleep(500 * 1000);   /* let relay bind UDP + TCP */

    int rv  = 1;
    int udp = open_client_socket();
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    unsigned char buf[1500];
    TcpReasm rr;
    tcp_reasm_init(&rr);

    struct sockaddr_in to = {0};
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = htonl(0x7F000001);
    to.sin_port        = htons(port);

    if (udp < 0 || tcp < 0) goto done;
    if (connect(tcp, (struct sockaddr *)&to, sizeof(to)) < 0) {
        perror("[tcp_roundtrip] connect"); goto done;
    }

    /* Register: UDP node 2 via sendto, TCP node 3 via the length-framed stream. */
    int n = pkt_make_v2(buf, REL_REG, 2, 0, 1, NULL, 0);
    sendto(udp, buf, (size_t)n, 0, (struct sockaddr *)&to, sizeof(to));
    n = pkt_make_v2(buf, REL_REG, 3, 0, 1, NULL, 0);
    if (tcp_send_framed_test(tcp, buf, n) < 0) goto done;
    usleep(200 * 1000);

    /* TCP(3) -> UDP(2): the UDP peer must receive the de-framed payload. */
    const char *msg_tu = "tcp->udp via relay";
    n = pkt_make_v2(buf, REL_DATA, 3, 2, 2, msg_tu, (int)strlen(msg_tu));
    if (tcp_send_framed_test(tcp, buf, n) < 0) goto done;
    int got = recv_with_timeout(udp, buf, sizeof(buf), 1500);
    if (got < HEAD + AUTH + (int)strlen(msg_tu) ||
        memcmp(buf + HEAD + AUTH, msg_tu, strlen(msg_tu)) != 0) {
        fprintf(stderr, "[tcp_roundtrip] UDP node missed TCP->UDP payload (got=%d)\n", got);
        goto done;
    }

    /* UDP(2) -> TCP(3): the TCP peer must receive it length-framed. */
    const char *msg_ut = "udp->tcp via relay";
    n = pkt_make_v2(buf, REL_DATA, 2, 3, 2, msg_ut, (int)strlen(msg_ut));
    sendto(udp, buf, (size_t)n, 0, (struct sockaddr *)&to, sizeof(to));
    unsigned char rxp[2048];
    int plen = tcp_recv_framed_test(tcp, &rr, rxp, sizeof(rxp), 1500);
    if (plen < HEAD + AUTH + (int)strlen(msg_ut) ||
        memcmp(rxp + HEAD + AUTH, msg_ut, strlen(msg_ut)) != 0) {
        fprintf(stderr, "[tcp_roundtrip] TCP node missed UDP->TCP payload (plen=%d)\n", plen);
        goto done;
    }

    fprintf(stderr, "[tcp_roundtrip] PASS — TCP node and UDP node exchanged A↔B "
                    "through the relay (one TCP, one UDP, same node table)\n");
    rv = 0;
done:
    if (tcp >= 0) close(tcp);
    if (udp >= 0) close(udp);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    return rv;
}

/* --- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Spawn relay with key. Verbose stderr alongside ours. */
    pid_t relay_pid = fork();
    if (relay_pid < 0) { perror("fork relay"); return 1; }
    if (relay_pid == 0) {
        setenv("PKERNEL_RELAY_KEY", TEST_KEY_HEX, 1);
        char p[16]; snprintf(p, sizeof(p), "%d", PORT);
        execl("./relay", "./relay", "-p", p, "-v", (char *)NULL);
        perror("execl ./relay");
        _exit(127);
    }
    usleep(500 * 1000);  /* let relay bind */

    int sa = open_client_socket();
    int sb = open_client_socket();
    if (sa < 0 || sb < 0) {
        kill(relay_pid, SIGTERM); waitpid(relay_pid, NULL, 0);
        return 1;
    }

    int fails = 0;
    fails += test_happy(sa, sb);
    drain(sa); drain(sb);
    fails += test_bad_hmac(sa, sb);
    drain(sa); drain(sb);
    fails += test_replay(sa, sb);
    drain(sa); drain(sb);
    fails += test_out_of_window(sa, sb);
    drain(sa); drain(sb);

    close(sa); close(sb);
    kill(relay_pid, SIGTERM);
    waitpid(relay_pid, NULL, 0);

    /* Scenarios 5 and 6 spawn their own relay. */
    fails += test_missing_key();
    fails += test_insecure_v1();

    /* SLICE 3 — scenario 7 (pure de-framer unit cert, no relay) and
     * scenario 8 (TCP↔UDP round-trip, spawns its own relay). */
    fails += test_deframer();
    fails += test_tcp_roundtrip();

    fprintf(stderr, "\n[relay-test] %s (%d failure%s)\n",
            fails == 0 ? "PASS — all 8 scenarios green (6 UDP + de-framer + TCP↔UDP)"
                       : "FAIL",
            fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
