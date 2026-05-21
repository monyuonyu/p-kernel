/*
 *  arch/linux/aarch64/net_unix.c
 *
 *  POSIX-only backend for the rtl8139 driver shim. Two ./p-kernel
 *  processes on the same host swap Ethernet frames via UDP loopback.
 *
 *  Why UDP loopback rather than AF_UNIX?
 *    - TUN/TAP requires CAP_NET_ADMIN; not available in Termux proot.
 *    - AF_UNIX abstract namespace and path-based sockets are both
 *      intercepted/translated by proot in ways that block cross-
 *      process delivery. UDP on 127.0.0.1 traverses the host kernel
 *      directly and works.
 *
 *  Node identity comes from the PKERNEL_NODE_ID environment variable.
 *  Each node listens on 127.0.0.1:(BASE_PORT + node_id) and sendto's
 *  every other slot in the same range — a naïve software switch
 *  that's fine for a 2..8-node mesh on a single machine.
 *
 *  This TU is POSIX-only; T-Kernel headers are NOT included to keep
 *  libc / sys headers clean. The matching T-Kernel-typed shim lives
 *  in rtl8139.c and forward-declares these by their C ABI.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* T-Kernel placeholder <errno.h> shadows system; declare what we need. */
extern int *__errno_location(void) __attribute__((__const__));
#define errno (*__errno_location())
extern char *strerror(int);

#define MAX_NODES   8
#define FRAME_MAX   1514
#define BASE_PORT   29000   /* 29001..29008 are our wire ports */

static int sock_fd = -1;
static int my_node_id = 1;
static struct sockaddr_in peer_addrs[MAX_NODES + 1];

int arch_linux_net_init(void)
{
    const char *env = getenv("PKERNEL_NODE_ID");
    my_node_id = env ? atoi(env) : 1;
    if (my_node_id < 1 || my_node_id > MAX_NODES) my_node_id = 1;

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) return -1;

    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);    /* 127.0.0.1 */
    addr.sin_port        = htons((unsigned short)(BASE_PORT + my_node_id));
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock_fd); sock_fd = -1;
        return -1;
    }

    for (int i = 1; i <= MAX_NODES; i++) {
        memset(&peer_addrs[i], 0, sizeof(peer_addrs[i]));
        peer_addrs[i].sin_family      = AF_INET;
        peer_addrs[i].sin_addr.s_addr = htonl(0x7F000001);
        peer_addrs[i].sin_port        = htons((unsigned short)(BASE_PORT + i));
    }

    dprintf(2, "[net_unix] node %d listening on 127.0.0.1:%d\n",
            my_node_id, BASE_PORT + my_node_id);
    return my_node_id;
}

int arch_linux_net_send(const void *frame, int len)
{
    if (sock_fd < 0 || len <= 0 || len > FRAME_MAX) return -1;
    for (int i = 1; i <= MAX_NODES; i++) {
        if (i == my_node_id) continue;
        (void)sendto(sock_fd, frame, (size_t)len,
                     MSG_DONTWAIT | MSG_NOSIGNAL,
                     (struct sockaddr *)&peer_addrs[i],
                     sizeof(peer_addrs[i]));
        /* Peers that aren't listening yet just produce ECONNREFUSED;
         * silently ignored — the next gossip round will retry. */
    }
    return len;
}

int arch_linux_net_recv(void *buf, int maxlen)
{
    if (sock_fd < 0) return 0;
    ssize_t n = recv(sock_fd, buf, (size_t)maxlen, MSG_DONTWAIT);
    if (n <= 0) return 0;
    return (int)n;
}

int arch_linux_net_node_id(void) { return my_node_id; }
