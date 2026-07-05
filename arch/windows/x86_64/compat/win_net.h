/*
 *  arch/windows/x86_64/compat/win_net.h
 *
 *  Winsock2 compatibility umbrella for the native Windows port. Included
 *  ONLY through the -idirafter shim headers (compat/sys/socket.h,
 *  netinet/in.h, arpa/inet.h, netdb.h, poll.h) — so it reaches only the
 *  POSIX-only network TUs (net_*.c, galaxy_posix.c), never the T-Kernel-
 *  typed arch/common brain.
 *
 *  It maps the BSD-sockets surface those files use onto Winsock2:
 *    - socket/bind/sendto/recvfrom/recv/send/connect/select/setsockopt/
 *      getsockopt/htons/htonl/getaddrinfo/inet_pton/inet_ntop  → native
 *    - close(sock)          → closesocket   (SAFE here: these TUs only
 *                             ever close sockets, never file descriptors)
 *    - fcntl(fd,F_SETFL,O_NONBLOCK) → ioctlsocket(FIONBIO)
 *    - poll                 → WSAPoll        (struct pollfd == WSAPOLLFD)
 *    - MSG_DONTWAIT/MSG_NOSIGNAL → 0 (nonblocking is set on the socket)
 *
 *  KNOWN P1 COMPROMISES (full fidelity is P2):
 *    - The net TUs store socket handles in `int`. On Win64 SOCKET is 64-bit
 *      (UINT_PTR); the truncation works in practice because Winsock handle
 *      values are small, and INVALID_SOCKET truncates to -1 so the `< 0`
 *      failure checks still hold. Correct SOCKET typing is P2.
 *    - Socket errors go to WSAGetLastError(), not errno, so strerror(errno)
 *      text after a socket failure may be inaccurate (cosmetic).
 */

#ifndef PKERNEL_WIN_NET_H
#define PKERNEL_WIN_NET_H

/* winsock2.h MUST precede windows.h; include it first and directly. */
#include <winsock2.h>
#include <ws2tcpip.h>

/* Pull the CRT's own close()/read()/write() declarations NOW, before the
 * close→closesocket macro below exists. These headers are include-guarded,
 * so later <unistd.h>/<io.h> includes in the socket TUs are no-ops and the
 * macro never corrupts a `int close(int)` declaration. */
#include <io.h>
#include <unistd.h>

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Linux privileged "force" socket-buffer options don't exist on Winsock.
 * Map them to the ordinary SO_*BUF (setsockopt still succeeds). The real
 * receive-buffer lever on the fleet is the host rmem_max anyway; here it's
 * a best-effort request. */
#ifndef SO_RCVBUFFORCE
#define SO_RCVBUFFORCE SO_RCVBUF
#endif
#ifndef SO_SNDBUFFORCE
#define SO_SNDBUFFORCE SO_SNDBUF
#endif

/* poll → WSAPoll. winsock2.h already provides `struct pollfd` (as
 * WSAPOLLFD) and POLLIN/POLLOUT/POLLERR/POLLHUP. */
#ifndef POLLIN
#define POLLIN  0x0100
#endif
#define poll(fds, nfds, timeout) WSAPoll((fds), (nfds), (timeout))

/* fcntl / O_NONBLOCK emulation for sockets via ioctlsocket(FIONBIO). The
 * net TUs call exactly fcntl(fd,F_GETFL,0) then fcntl(fd,F_SETFL,fl|O_NONBLOCK). */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif

static __inline int win_sock_fcntl(int fd, int cmd, long arg)
{
    if (cmd == F_SETFL) {
        u_long nb = (arg & O_NONBLOCK) ? 1u : 0u;
        ioctlsocket((SOCKET)fd, FIONBIO, &nb);
        return 0;
    }
    return 0;   /* F_GETFL: report no flags set */
}
#define fcntl(fd, cmd, arg) win_sock_fcntl((int)(fd), (int)(cmd), (long)(arg))

/* close(sock) → closesocket. Safe: these TUs never close file fds. */
#define close(fd) closesocket((SOCKET)(fd))

#endif /* PKERNEL_WIN_NET_H */
