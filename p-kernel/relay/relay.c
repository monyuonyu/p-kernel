/*
 *  relay/relay.c — UMP relay server (Phase B v1)
 *
 *  See docs/phase_b_relay.md for the design. In one line:
 *  maintain a table of {node_id → (peer_address, last_seen)}; when a
 *  packet arrives, look up its dst_node and forward there.
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

#define RELAY_DEFAULT_PORT 7400
#define RELAY_MAGIC        0x52454C59U   /* "RELY" (little-endian) */
#define RELAY_VERSION      1
#define RELAY_HEADER_LEN   12
#define MAX_PAYLOAD        1380
#define MAX_PKT            (RELAY_HEADER_LEN + MAX_PAYLOAD)
#define NODE_MAX           256
#define IDLE_TIMEOUT       300           /* seconds before an entry is evicted */

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
} NodeEntry;

static NodeEntry table[NODE_MAX];
static int verbose = 0;

/* --- header parsing ----------------------------------------------------- */

static int parse_header(const unsigned char *buf, int len,
                        unsigned *type, unsigned *src, unsigned *dst)
{
    if (len < RELAY_HEADER_LEN) return -1;
    uint32_t magic = (uint32_t)buf[0]
                   | ((uint32_t)buf[1] << 8)
                   | ((uint32_t)buf[2] << 16)
                   | ((uint32_t)buf[3] << 24);
    if (magic != RELAY_MAGIC)     return -1;
    if (buf[4] != RELAY_VERSION)  return -1;
    *type = buf[5];
    *src  = buf[6];
    *dst  = buf[7];
    return 0;
}

/* --- table maintenance -------------------------------------------------- */

static void update(int src, const struct sockaddr_in *from)
{
    if (src < 1 || src >= NODE_MAX) return;
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
        if (table[n].active && now - table[n].last_seen > IDLE_TIMEOUT) {
            table[n].active = 0;
            fprintf(stderr, "[relay] node %d evicted (idle)\n", n);
        }
    }
}

/* --- forwarding --------------------------------------------------------- */

static void forward(int sock, int dst_node,
                    const unsigned char *buf, int len, time_t now)
{
    if (dst_node < 1 || dst_node >= NODE_MAX) return;
    if (!table[dst_node].active) {
        if (verbose) fprintf(stderr, "[relay] no route to dst=%d\n", dst_node);
        return;
    }
    if (now - table[dst_node].last_seen > IDLE_TIMEOUT) {
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

int main(int argc, char **argv)
{
    int port = RELAY_DEFAULT_PORT;
    int opt;
    while ((opt = getopt(argc, argv, "p:vh")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'v': verbose = 1; break;
        case 'h':
        default:
            fprintf(stderr, "usage: %s [-p port] [-v]\n", argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    /* sigaction without SA_RESTART so recvfrom() actually returns
     * EINTR on SIGTERM/SIGINT — signal() on Linux glibc defaults to
     * SA_RESTART, which would auto-restart the syscall and the main
     * loop would never see `stop`. */
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

    fprintf(stderr, "[relay] listening on 0.0.0.0:%d (verbose=%d)\n",
            port, verbose);

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

        unsigned type, src, dst;
        if (parse_header(buf, (int)n, &type, &src, &dst) < 0) {
            if (verbose) fprintf(stderr, "[relay] drop: bad header (%zd B)\n", n);
            continue;
        }

        time_t now = time(NULL);
        evict_stale(now);

        if (verbose) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            fprintf(stderr, "[relay] rx type=%u src=%u dst=%u from %s:%u (%zd B)\n",
                    type, src, dst, ip, ntohs(from.sin_port), n);
        }

        switch (type) {
        case REL_REGISTER:
        case REL_KEEPALIVE:
            update((int)src, &from);
            break;

        case REL_DATA:
            update((int)src, &from);
            forward(sock, (int)dst, buf, (int)n, now);
            break;

        case REL_BROADCAST:
            update((int)src, &from);
            for (int n_id = 1; n_id < NODE_MAX; n_id++) {
                if (n_id == (int)src) continue;
                if (!table[n_id].active)    continue;
                forward(sock, n_id, buf, (int)n, now);
            }
            break;

        default:
            if (verbose) fprintf(stderr, "[relay] drop: unknown type %u\n", type);
        }
    }

    fprintf(stderr, "[relay] shutdown\n");
    close(sock);
    return 0;
}
