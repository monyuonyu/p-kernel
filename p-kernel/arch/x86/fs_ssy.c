/*
 *  fs_ssy.c (x86)
 *  Filesystem subsystem — ssid=3
 *
 *  Responsibilities:
 *    - Handle POSIX file-I/O syscalls routed via knl_svc_ientry()
 *    - Track per-task VFS fd ownership
 *    - Auto-close open fds when a task exits (cleanupfn)
 *
 *  Syscall routing (from syscall.c):
 *    SYS_READ  (fd≥3), SYS_WRITE (fd≥3),
 *    SYS_OPEN, SYS_CLOSE, SYS_LSEEK,
 *    SYS_MKDIR, SYS_UNLINK, SYS_RENAME, SYS_READDIR
 *    → FS_SVC_PKT via knl_svc_ientry(&pk, (FN)((nr<<8)|FS_SSID))
 */

#include "kernel.h"
#include "task.h"
#include "fs_ssy.h"
#include "vfs.h"
#include "p_syscall.h"
#include <syscall.h>    /* tk_def_ssy */
#include <tmonitor.h>

/* ------------------------------------------------------------------ */
/* FD ownership table                                                   */
/* Maps VFS fd (0..FS_MAX_FD-1) → owning task ID.                     */
/* ------------------------------------------------------------------ */
#define FS_MAX_FD  8    /* must match FAT32_MAX_FD */

typedef struct {
    BOOL  in_use;
    ID    owner_tid;
} FS_FD_SLOT;

static FS_FD_SLOT fs_fds[FS_MAX_FD];

/* readdir scratch buffer (avoids a large stack allocation) */
#define READDIR_BUF_MAX  32
static VFS_DIRENT readdir_kbuf[READDIR_BUF_MAX];

/* ------------------------------------------------------------------ */
/* FD helpers                                                           */
/* ------------------------------------------------------------------ */
#define POSIX_FD_OFFSET  3
#define IS_STD_FD(fd)    ((fd) == 0 || (fd) == 1 || (fd) == 2)
#define TO_VFS_FD(fd)    ((fd) - POSIX_FD_OFFSET)
#define TO_POSIX_FD(vfd) ((vfd) + POSIX_FD_OFFSET)

static void fd_record(INT vfd, ID tid)
{
    if (vfd >= 0 && vfd < FS_MAX_FD) {
        fs_fds[vfd].in_use    = TRUE;
        fs_fds[vfd].owner_tid = tid;
    }
}

static void fd_release(INT vfd)
{
    if (vfd >= 0 && vfd < FS_MAX_FD)
        fs_fds[vfd].in_use = FALSE;
}

/* ------------------------------------------------------------------ */
/* cleanupfn — called by knl_ssy_cleanup() when a task exits          */
/* ------------------------------------------------------------------ */
static void fs_cleanupfn(ID tskid)
{
    for (INT i = 0; i < FS_MAX_FD; i++) {
        if (fs_fds[i].in_use && fs_fds[i].owner_tid == tskid) {
            vfs_close(i);
            fs_fds[i].in_use = FALSE;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                             */
/* ------------------------------------------------------------------ */
static W fs_dispatch(W nr, W arg0, W arg1, W arg2)
{
    switch (nr) {

    case SYS_WRITE: {
        /* arg0=fd, arg1=buf_ptr, arg2=len  (fd is POSIX, already ≥3) */
        const char *buf = (const char *)(UW)arg1;
        W len = arg2;
        if (len < 0 || len > 65536) return -1;
        if (!vfs_ready) return -1;
        return vfs_write(TO_VFS_FD(arg0), buf, (UW)len);
    }

    case SYS_READ: {
        /* arg0=fd, arg1=buf_ptr, arg2=len */
        void *buf = (void *)(UW)arg1;
        W len = arg2;
        if (len < 0 || len > 65536) return -1;
        if (!vfs_ready) return -1;
        return vfs_read(TO_VFS_FD(arg0), buf, (UW)len);
    }

    case SYS_OPEN: {
        /* arg0=path_ptr, arg1=flags (O_RDONLY=0, O_WRONLY=1, O_CREAT=0x40) */
        const char *path = (const char *)(UW)arg0;
        W flags = arg1;
        if (!vfs_ready) return -1;
        INT vfd = (flags & 0x0041) ? vfs_create(path) : vfs_open(path);
        if (vfd < 0) return -1;
        fd_record(vfd, knl_ctxtsk->tskid);
        return TO_POSIX_FD(vfd);
    }

    case SYS_CLOSE: {
        /* arg0=fd */
        if (IS_STD_FD(arg0)) return 0;
        if (!vfs_ready) return -1;
        INT vfd = TO_VFS_FD(arg0);
        vfs_close(vfd);
        fd_release(vfd);
        return 0;
    }

    case SYS_LSEEK: {
        /* arg0=fd, arg1=offset, arg2=whence */
        if (IS_STD_FD(arg0)) return -1;
        if (!vfs_ready) return -1;
        UW off = (UW)arg1;
        if (arg2 == 2)  /* SEEK_END */
            off = vfs_fsize(TO_VFS_FD(arg0));
        return vfs_seek(TO_VFS_FD(arg0), off);
    }

    case SYS_MKDIR: {
        const char *path = (const char *)(UW)arg0;
        if (!vfs_ready) return -1;
        return vfs_mkdir(path);
    }

    case SYS_UNLINK: {
        const char *path = (const char *)(UW)arg0;
        if (!vfs_ready) return -1;
        return vfs_unlink(path);
    }

    case SYS_RENAME: {
        const char *old = (const char *)(UW)arg0;
        const char *nw  = (const char *)(UW)arg1;
        if (!vfs_ready) return -1;
        return vfs_rename(old, nw);
    }

    case SYS_READDIR: {
        /* arg0=path_ptr, arg1=PK_DIRENT*, arg2=max_entries */
        const char *path = (const char *)(UW)arg0;
        PK_DIRENT  *out  = (PK_DIRENT  *)(UW)arg1;
        INT max = (INT)arg2;
        if (!path || !out || max <= 0) return -1;
        if (!vfs_ready) return -1;
        if (max > READDIR_BUF_MAX) max = READDIR_BUF_MAX;

        INT n = vfs_readdir(path, readdir_kbuf, max);
        if (n < 0) return n;

        for (INT i = 0; i < n; i++) {
            INT j;
            for (j = 0; j < PK_DIRENT_NAMELEN - 1 && readdir_kbuf[i].name[j]; j++)
                out[i].name[j] = readdir_kbuf[i].name[j];
            out[i].name[j] = '\0';
            out[i].size    = readdir_kbuf[i].size;
            out[i].is_dir  = readdir_kbuf[i].is_dir ? 1 : 0;
        }
        return n;
    }

    default:
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* svchdr — T-Kernel subsystem service call handler                   */
/* ------------------------------------------------------------------ */
static INT fs_svchdr(void *pk_para, FN fncd)
{
    (void)fncd;
    FS_SVC_PKT *pk = (FS_SVC_PKT *)pk_para;
    return (INT)fs_dispatch(pk->nr, pk->arg0, pk->arg1, pk->arg2);
}

/* Public entry used directly by syscall_dispatch (avoids knl_svc_ientry overhead). */
W fs_ssy_call(W nr, W arg0, W arg1, W arg2)
{
    return (W)fs_dispatch(nr, arg0, arg1, arg2);
}

/* ------------------------------------------------------------------ */
/* fs_ssy_init                                                          */
/* ------------------------------------------------------------------ */
void fs_ssy_init(void)
{
    for (INT i = 0; i < FS_MAX_FD; i++) fs_fds[i].in_use = FALSE;

    T_DSSY dssy;
    dssy.ssyatr    = TA_NULL;
    dssy.ssypri    = 3;
    dssy.svchdr    = (FP)fs_svchdr;
    dssy.breakfn   = NULL;
    dssy.startupfn = NULL;
    dssy.cleanupfn = (FP)fs_cleanupfn;
    dssy.eventfn   = NULL;
    dssy.resblksz  = 0;

    ER er = tk_def_ssy(FS_SSID, &dssy);
    if (er == E_OK)
        tm_putstring((UB *)"[fs_ssy]  registered (ssid=3)\r\n");
    else
        tm_putstring((UB *)"[fs_ssy]  tk_def_ssy FAILED\r\n");
}
