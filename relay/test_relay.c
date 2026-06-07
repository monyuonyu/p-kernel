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

    fprintf(stderr, "\n[relay-test] %s (%d failure%s)\n",
            fails == 0 ? "PASS — all 6 scenarios green" : "FAIL",
            fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
