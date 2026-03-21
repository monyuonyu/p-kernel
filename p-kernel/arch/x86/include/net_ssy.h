/*
 *  net_ssy.h (x86)
 *  Network subsystem — ssid=1
 *
 *  Manages UDP socket slots + TCP connections.
 *  Registered via tk_def_ssy(NET_SSID, ...) in net_ssy_init().
 *  Invoked from syscall_dispatch() via knl_svc_ientry().
 *  Cleanup hook auto-closes sockets when the owning task exits.
 */

#pragma once
#include "kernel.h"

/* T-Kernel subsystem ID assigned to the network subsystem */
#define NET_SSID    1

/*
 * Packet passed through knl_svc_ientry() to the subsystem svchdr.
 * syscall_dispatch fills this from its (nr, arg0, arg1, arg2) args.
 */
typedef struct {
    W   nr;     /* syscall number (0x200–0x2FF) */
    W   arg0;
    W   arg1;
    W   arg2;
} NET_SVC_PKT;

/* Initialise state, allocate semaphores, register with tk_def_ssy() */
void net_ssy_init(void);
