/*
 *  fat32.h (x86)
 *  FAT32 read-only filesystem
 */
#pragma once
#include "kernel.h"

#define FAT32_MAX_FD        8       /* max open file descriptors   */
#define FAT32_MAX_PATH      128
#define FAT32_MAX_NAME      256     /* LFN support                 */

typedef struct {
    char  name[FAT32_MAX_NAME];     /* UTF-8 filename              */
    UW    size;                     /* file size in bytes          */
    BOOL  is_dir;
} FAT32_DIRENT;

/* Mount FAT32 from the IDE drive.
 * Returns 0 on success, -1 on error. */
INT fat32_mount(void);

/* Open a file by absolute path (e.g. "/bin/hello.elf").
 * Returns fd ≥ 0 on success, negative on error. */
INT fat32_open(const char *path);

/* Read up to `len` bytes from an open fd.
 * Returns bytes read (0 = EOF), negative on error. */
INT fat32_read(INT fd, void *buf, UW len);

/* Seek to absolute byte offset. */
INT fat32_seek(INT fd, UW offset);

/* Return file size for open fd. */
UW  fat32_fsize(INT fd);

/* Close fd. */
void fat32_close(INT fd);

/* List directory entries.
 * `path` = "/" for root.
 * Stores up to `max` entries in `out[]`.
 * Returns entry count, negative on error. */
INT fat32_readdir(const char *path, FAT32_DIRENT *out, INT max);

extern BOOL fat32_mounted;
