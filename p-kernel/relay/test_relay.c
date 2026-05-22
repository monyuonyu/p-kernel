/*
 *  relay/test_relay.c — two-client round-trip test for the UMP relay.
 *
 *  Spawns ./relay as a child, then forks two simulated client processes
 *  A (src=1) and B (src=2). Each client registers, then sends a DATA
 *  packet to the other. Each client expects to receive exactly the
 *  other's payload through the relay. Parent reaps everyone, reports
 *  pass/fail.
 *
 *  Uses a non-default port (PORT below) so it doesn't clash with a
 *  long-running production relay on the same host.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAGIC       0x52454C59U
#define VER         1
#define HEAD        12
#define PORT        27400      /* test port, away from the default 7400 */
#define REL_REG     1
#define REL_DATA    2

/* --- packet helpers ------------------------------------------------------ */

static int pkt_make(unsigned char *buf, unsigned type, unsigned src,
                    unsigned dst, const char *payload, int plen)
{
    buf[0] = (unsigned char)(MAGIC      & 0xff);
    buf[1] = (unsigned char)((MAGIC>>8) & 0xff);
    buf[2] = (unsigned char)((MAGIC>>16)& 0xff);
    buf[3] = (unsigned char)((MAGIC>>24)& 0xff);
    buf[4] = VER;
    buf[5] = (unsigned char)type;
    buf[6] = (unsigned char)src;
    buf[7] = (unsigned char)dst;
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    if (payload && plen > 0) memcpy(buf + HEAD, payload, (size_t)plen);
    return HEAD + plen;
}

/* --- client subprocess --------------------------------------------------- */

static int run_client(unsigned my_node, unsigned peer_node,
                      const char *send_msg, const char *expect_msg)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("client socket"); return 1; }

    struct sockaddr_in any = {0};
    any.sin_family = AF_INET;
    if (bind(s, (struct sockaddr *)&any, sizeof(any)) < 0) {
        perror("client bind"); return 1;
    }

    struct sockaddr_in relay = {0};
    relay.sin_family      = AF_INET;
    relay.sin_addr.s_addr = htonl(0x7F000001);
    relay.sin_port        = htons(PORT);

    unsigned char buf[1400];
    int n;

    /* 1. REGISTER */
    n = pkt_make(buf, REL_REG, my_node, 0, NULL, 0);
    if (sendto(s, buf, (size_t)n, 0,
               (struct sockaddr *)&relay, sizeof(relay)) < 0) {
        perror("sendto REGISTER"); return 1;
    }
    /* Wait briefly so the peer's REGISTER is also seen before we DATA. */
    usleep(300 * 1000);

    /* 2. DATA */
    n = pkt_make(buf, REL_DATA, my_node, peer_node,
                 send_msg, (int)strlen(send_msg));
    if (sendto(s, buf, (size_t)n, 0,
               (struct sockaddr *)&relay, sizeof(relay)) < 0) {
        perror("sendto DATA"); return 1;
    }

    /* 3. Receive — 2 second timeout. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t got = recv(s, buf, sizeof(buf), 0);
    if (got < HEAD) {
        fprintf(stderr, "[client n%u] no data received (got=%zd)\n",
                my_node, got);
        return 1;
    }

    int expect_len = (int)strlen(expect_msg);
    int payload_len = (int)got - HEAD;
    if (payload_len != expect_len ||
        memcmp(buf + HEAD, expect_msg, (size_t)expect_len) != 0) {
        char shown[64] = {0};
        int copy = payload_len < 60 ? payload_len : 60;
        memcpy(shown, buf + HEAD, (size_t)copy);
        fprintf(stderr, "[client n%u] mismatch: got '%s' (%d B) want '%s'\n",
                my_node, shown, payload_len, expect_msg);
        return 1;
    }

    fprintf(stderr, "[client n%u] received: '%s' (%d B)\n",
            my_node, expect_msg, payload_len);
    close(s);
    return 0;
}

/* --- main: spawn relay, spawn two clients, reap, report ------------------ */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Spawn the relay. Verbose so its log goes to stderr alongside ours. */
    pid_t relay_pid = fork();
    if (relay_pid < 0) { perror("fork relay"); return 1; }
    if (relay_pid == 0) {
        char p[16]; snprintf(p, sizeof(p), "%d", PORT);
        execl("./relay", "./relay", "-p", p, "-v", (char *)NULL);
        perror("execl ./relay");
        _exit(127);
    }

    /* Give the relay a beat to bind. */
    usleep(500 * 1000);

    pid_t a = fork();
    if (a == 0) _exit(run_client(1, 2, "hello from A", "hello from B"));
    pid_t b = fork();
    if (b == 0) _exit(run_client(2, 1, "hello from B", "hello from A"));

    int sa = 0, sb = 0;
    waitpid(a, &sa, 0);
    waitpid(b, &sb, 0);

    /* Clean up the relay. */
    kill(relay_pid, SIGTERM);
    waitpid(relay_pid, NULL, 0);

    int ok = WIFEXITED(sa) && WEXITSTATUS(sa) == 0
          && WIFEXITED(sb) && WEXITSTATUS(sb) == 0;

    fprintf(stderr, "\n[relay-test] %s\n",
            ok ? "PASS — both payloads round-tripped through relay"
               : "FAIL");
    return ok ? 0 : 1;
}
