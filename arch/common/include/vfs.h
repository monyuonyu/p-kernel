/*
 *  vfs.h (x86)
 *  Virtual File System — thin wrapper over FAT32
 *
 *  Currently: one mount point (FAT32 on IDE drive).
 *  Future: multiple backends (ramfs, devfs, …).
 */
#pragma once
#include "kernel.h"

#define VFS_MAX_NAME    256

typedef struct {
    char  name[VFS_MAX_NAME];
    UW    size;
    BOOL  is_dir;
} VFS_DIRENT;

/* Mount the root filesystem (IDE → FAT32).
 * Returns 0 on success. */
INT  vfs_init(void);

/* Open a file.  Returns fd ≥ 0 or negative on error. */
INT  vfs_open(const char *path);

/* Read up to `len` bytes.  Returns bytes read (0=EOF), negative=error. */
INT  vfs_read(INT fd, void *buf, UW len);

/* Seek to absolute byte position. */
INT  vfs_seek(INT fd, UW offset);

/* File size of open fd. */
UW   vfs_fsize(INT fd);

/* Close fd. */
void vfs_close(INT fd);

/* Create or truncate a file; returns writable fd or -1. */
INT  vfs_create(const char *path);

/* Write len bytes at current position of writable fd. */
INT  vfs_write(INT fd, const void *buf, UW len);

/* Delete a file. */
INT  vfs_unlink(const char *path);

/* Create a directory. */
INT  vfs_mkdir(const char *path);

/* Rename / move. */
INT  vfs_rename(const char *oldpath, const char *newpath);

/* List directory.  Returns entry count, negative on error. */
INT  vfs_readdir(const char *path, VFS_DIRENT *out, INT max);

/* Stat by path.  Returns 0 on success, -1 on error. */
INT  vfs_stat_path(const char *path, UW *size, BOOL *is_dir);

/* Duplicate an open fd (independent seek, read-only). */
INT  vfs_dup(INT fd);
/* Duplicate old_fd to new_fd (dup2 semantics). Closes new_fd first if open. */
INT  vfs_dup2(INT old_fd, INT new_fd);

/* Get/set current working directory. */
void vfs_getcwd(char *buf, INT len);
INT  vfs_chdir(const char *path);

extern BOOL vfs_ready;

/* ------------------------------------------------------------------ */
/* Second backend slot — p-fs content-addressed store (p-fs.md §3.1)   */
/* ------------------------------------------------------------------ */
/* The VFS today fronts a single FAT32 mount ("Currently: one mount
 * point" above). p-fs is the planned second backend that sits beside
 * FAT32: content-addressed, immutable blocks (block-id = sha256(bytes)).
 *
 * P0 (delivered): the block layer itself — see pfs_block.h
 * (pfs_put / pfs_get / pfs_has). It is reachable directly today; the
 * VFS_BACKEND enum below reserves the slot so a future increment can
 * route vfs_open/read/write to either backend by mount point without an
 * API change (p-fs.md §3.1 "vfs.h コメントの Future: multiple backends").
 *
 * TODO(P1+): dispatch table {fat32, pfs} selected per mount; for now the
 * two coexist as separate entry points (vfs_* vs pfs_*). */
typedef enum {
    VFS_BACKEND_FAT32 = 0,   /* current durable backend (arch/x86/fat32.c) */
    VFS_BACKEND_PFS   = 1,   /* p-fs content-addressed store (pfs_block.c) */
    VFS_BACKEND_ARK   = 2,   /* ARK log-structured survival FS (arkfs.c)   */
} VFS_BACKEND;

/* ------------------------------------------------------------------ */
/* ARK backend — selectable, durable, bare-metal mount (ARK-3).        */
/*                                                                      */
/* Until now ARK (arch/common/arkfs.c) was only reachable on hosted    */
/* Linux (behind _TK_HOSTED_LIBC_); the audit's 🔴3. These entry points */
/* make ARK a real VFS backend on bare metal: bind ARK's ARK_BDEV      */
/* vtable to a registered block device ("ide0") and mount it. FAT32     */
/* stays the default backend and is untouched — selecting ARK is        */
/* explicit (vfs_ark_mount), so existing FAT32/PFS behaviour is intact. */
/*                                                                      */
/* ARK is whole-file + content-addressed, not an fd-stream FS, so it is */
/* fronted by whole-file ops (vfs_ark_write_file/read_file) rather than */
/* the FAT32 fd verbs above — mirroring how p-fs coexists as its own    */
/* entry points (pfs_*). vfs_stat_path / vfs_readdir DO dispatch on the */
/* active backend once ARK is mounted.                                  */
/* ------------------------------------------------------------------ */

/* Mount ARK on block device `dev` (NULL => "ide0"). If `do_format` is
 * TRUE the device is freshly formatted first (destroys any content).
 * On success the active backend becomes VFS_BACKEND_ARK. Returns 0, or
 * negative (ARK_E_* / -1). */
INT  vfs_ark_mount(const char *dev, BOOL do_format);

/* Whole-file write/read against the mounted ARK backend. Each write is
 * one atomic, content-addressed, fsync'd commit. Return bytes / length
 * or negative. */
INT  vfs_ark_write_file(const char *path, const void *buf, UW len);
INT  vfs_ark_read_file(const char *path, void *buf, UW max);

/* Which backend the path-based VFS ops currently dispatch to. */
VFS_BACKEND vfs_active_backend(void);
