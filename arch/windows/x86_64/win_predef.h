/*
 *  arch/windows/x86_64/win_predef.h
 *
 *  Force-included (-include) into EVERY translation unit of the native
 *  Windows build. Deliberately contains ONLY plain C declarations and
 *  NO <winsock2.h>/<windows.h> — those pull in the Win32 type universe
 *  (BOOL/DWORD/…) which collides with the T-Kernel type universe used by
 *  the arch/common brain. So this header is safe to inject globally.
 *
 *  Its whole job: declare the handful of POSIX libc functions that
 *  mingw-w64 lacks but that portable (non-socket) TUs still call — chiefly
 *  dprintf(), which net_dispatch.c uses without including any socket
 *  header. Real definitions live in arch/windows/x86_64/net_win.c.
 *
 *  Socket/Winsock shims live separately in compat/win_net.h and reach
 *  only the POSIX socket TUs via -idirafter shim headers.
 */

#ifndef PKERNEL_WIN_PREDEF_H
#define PKERNEL_WIN_PREDEF_H

#ifdef _WIN32

/* dprintf(fd, fmt, ...) — absent on mingw. Defined in net_win.c. */
extern int dprintf(int fd, const char *fmt, ...);

/* Logical CPU count (replaces sysconf(_SC_NPROCESSORS_ONLN)). win_prim.c. */
extern int win_num_cpus(void);

#endif /* _WIN32 */

#endif /* PKERNEL_WIN_PREDEF_H */
