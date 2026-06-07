/*
 *  fs_ssy.h (x86)
 *  Filesystem subsystem — ssid=3
 *
 *  Handles POSIX file I/O syscalls (SYS_OPEN, SYS_READ, SYS_WRITE,
 *  SYS_CLOSE, SYS_LSEEK, SYS_MKDIR, SYS_UNLINK, SYS_RENAME, SYS_READDIR).
 *  Tracks per-task FD ownership; auto-closes FDs on task exit (cleanupfn).
 */
#pragma once
#include "kernel.h"

/* T-Kernel subsystem ID for the filesystem */
#define FS_SSID  3

/* Service call packet — passed to knl_svc_ientry() */
typedef struct {
    W nr;    /* syscall number (SYS_OPEN, SYS_READ, …)   */
    W arg0;
    W arg1;
    W arg2;
} FS_SVC_PKT;

/* Initialise FD ownership table and register with tk_def_ssy(). */
void fs_ssy_init(void);

/* Direct call from syscall_dispatch (no knl_svc_ientry overhead). */
W fs_ssy_call(W nr, W arg0, W arg1, W arg2);
