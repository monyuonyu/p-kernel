/*
 *  arch/windows/x86_64/net_win.c
 *
 *  Winsock bring-up and the small libc shims the native Windows port
 *  needs. POSIX-only TU (no T-Kernel headers): it only pulls in winsock2
 *  and the CRT.
 *
 *    win_net_startup()  — WSAStartup(2.2). Called once at boot
 *                         (main_win.c) before any socket() call.
 *    __errno_location() — the net TUs declare this (glibc-ism) and use it
 *                         as errno; map to mingw's _errno().
 *    dprintf(fd,fmt,..) — absent on mingw; used by net_dispatch.c and the
 *                         net_*.c diagnostics. Route fd 2 → stderr, else
 *                         stdout.
 */

#include <winsock2.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

void win_net_startup(void)
{
    static int done = 0;
    WSADATA wsa;
    if (done) return;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        done = 1;
    }
}

/* The net TUs do `extern int *__errno_location(void); #define errno
 * (*__errno_location())`. Provide the symbol via mingw's _errno(). */
int *__errno_location(void)
{
    return _errno();
}

int dprintf(int fd, const char *fmt, ...)
{
    FILE *f = (fd == 2) ? stderr : stdout;
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
    return n;
}
