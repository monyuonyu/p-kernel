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

extern BOOL vfs_ready;
