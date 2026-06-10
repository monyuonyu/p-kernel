/*
 *  galaxy_posix.c — POSIX transport for the galaxy observation window
 *  (docs/architecture/galaxy.md §3.2). The host-TCP listen socket that
 *  v1 needs and the in-kernel netstack cannot serve (§1).
 *
 *  Identical source compiled per-arch (arch/linux/x86_64 + arch/linux/
 *  aarch64), exactly like net_unix.c — promoted to arch/common/linux/
 *  when net_unix.c's duplication is cleaned up (net_dispatch.c:13-14).
 *
 *  Five C-ABI functions (NO T-Kernel types — the net_unix.c:19-21 rule;
 *  errno shim net_unix.c:34-37). All fds O_NONBLOCK. A client that stops
 *  reading is dropped on EWOULDBLOCK backlog overflow so a dead observer
 *  can never wedge the server task, let alone the organism (§3.2).
 *
 *  SECURITY (§3.5): the listen socket binds 127.0.0.1 ONLY, hard-coded
 *  htonl(0x7F000001) — there is NO code path that binds 0.0.0.0. Loopback
 *  only; the owner's own browser on the owner's own device.
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
#ifndef EAGAIN
#define EAGAIN       11
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK  EAGAIN
#endif
#ifndef TCP_NODELAY
#define TCP_NODELAY  1
#endif
#ifndef IPPROTO_TCP
#define IPPROTO_TCP  6
#endif

#define GALAXY_MAX_CLIENTS  4

static int listen_fd = -1;
static int client_fd[GALAXY_MAX_CLIENTS];

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* listen on 127.0.0.1:port, nonblocking. ret 0 on success, -1 on error. */
int galaxy_io_init(int port)
{
    for (int i = 0; i < GALAXY_MAX_CLIENTS; i++) client_fd[i] = -1;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;

    int one = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);   /* 127.0.0.1 — HARD-CODED */
    addr.sin_port        = htons((unsigned short)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(listen_fd); listen_fd = -1;
        return -1;
    }
    if (listen(listen_fd, GALAXY_MAX_CLIENTS + 2) < 0) {
        close(listen_fd); listen_fd = -1;
        return -1;
    }
    set_nonblock(listen_fd);

    dprintf(2, "[galaxy] listening on 127.0.0.1:%d\n", port);
    return 0;
}

/* accept one pending connection into a free slot. ret slot or -1. */
int galaxy_io_accept(void)
{
    if (listen_fd < 0) return -1;
    int slot = -1;
    for (int i = 0; i < GALAXY_MAX_CLIENTS; i++)
        if (client_fd[i] < 0) { slot = i; break; }

    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) return -1;                       /* EWOULDBLOCK = nothing  */

    if (slot < 0) { close(fd); return -1; }      /* all slots busy -> drop */

    set_nonblock(fd);
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    client_fd[slot] = fd;
    return slot;
}

/* nonblocking read. ret bytes (>0), 0 = nothing now, -1 = closed/error. */
int galaxy_io_read(int slot, void *buf, int max)
{
    if (slot < 0 || slot >= GALAXY_MAX_CLIENTS || client_fd[slot] < 0) return -1;
    ssize_t n = recv(client_fd[slot], buf, (size_t)max, MSG_DONTWAIT);
    if (n > 0) return (int)n;
    if (n == 0) return -1;                        /* peer closed            */
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

/* nonblocking write. ret bytes written (may be short), -1 = closed.
 * EWOULDBLOCK returns 0 (caller's small out-buffer absorbs / closes). */
int galaxy_io_write(int slot, const void *buf, int len)
{
    if (slot < 0 || slot >= GALAXY_MAX_CLIENTS || client_fd[slot] < 0) return -1;
    if (len <= 0) return 0;
    ssize_t n = send(client_fd[slot], buf, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

void galaxy_io_close(int slot)
{
    if (slot < 0 || slot >= GALAXY_MAX_CLIENTS) return;
    if (client_fd[slot] >= 0) { close(client_fd[slot]); client_fd[slot] = -1; }
}
