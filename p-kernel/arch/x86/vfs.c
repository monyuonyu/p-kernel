/*
 *  vfs.c (x86)
 *  Virtual File System — delegates to FAT32
 */

#include "kernel.h"
#include "ide.h"
#include "blk_ssy.h"
#include "fat32.h"
#include "vfs.h"
#include <tmonitor.h>

BOOL vfs_ready = FALSE;

/* ----------------------------------------------------------------- */
/* Current working directory                                         */
/* ----------------------------------------------------------------- */

static char vfs_cwd[FAT32_MAX_PATH] = "/";

/*
 * Resolve a user-supplied path:
 *   - Absolute paths (start with '/') pass through unchanged.
 *   - Relative paths are prepended with vfs_cwd.
 * Result is written to out_buf (at least FAT32_MAX_PATH bytes).
 */
static void vfs_resolve(const char *path, char *out_buf)
{
    if (path[0] == '/') {
        /* absolute — copy as-is */
        INT i = 0;
        while (path[i] && i < FAT32_MAX_PATH - 1) { out_buf[i] = path[i]; i++; }
        out_buf[i] = '\0';
        return;
    }
    /* relative — prepend cwd */
    INT ci = 0;
    while (vfs_cwd[ci] && ci < FAT32_MAX_PATH - 2) { out_buf[ci] = vfs_cwd[ci]; ci++; }
    /* ensure trailing slash */
    if (ci > 0 && out_buf[ci - 1] != '/') out_buf[ci++] = '/';
    INT pi = 0;
    while (path[pi] && ci < FAT32_MAX_PATH - 1) { out_buf[ci++] = path[pi++]; }
    out_buf[ci] = '\0';
}

INT vfs_init(void)
{
    /* Probe IDE; on success it self-registers as "ide0" in blk_ssy */
    if (ide_init() < 0) {
        tm_putstring((UB *)"[vfs]  IDE not found — filesystem unavailable\r\n");
        return -1;
    }

    /* Hand the "ide0" ops to FAT32 */
    const BLK_OPS *blk = blk_ssy_lookup("ide0");
    if (!blk) {
        tm_putstring((UB *)"[vfs]  blk_ssy: ide0 not registered\r\n");
        return -1;
    }
    fat32_set_blkdev(blk);

    if (fat32_mount() < 0) {
        tm_putstring((UB *)"[vfs]  FAT32 mount failed\r\n");
        return -1;
    }
    vfs_ready = TRUE;
    tm_putstring((UB *)"[vfs]  ready\r\n");
    return 0;
}

INT  vfs_open(const char *path)
{
    char buf[FAT32_MAX_PATH]; vfs_resolve(path, buf);
    return fat32_open(buf);
}
INT  vfs_read(INT fd, void *b, UW l)       { return fat32_read(fd, b, l); }
INT  vfs_seek(INT fd, UW off)              { return fat32_seek(fd, off); }
UW   vfs_fsize(INT fd)                     { return fat32_fsize(fd); }
void vfs_close(INT fd)                     { fat32_close(fd); }
INT  vfs_dup(INT fd)                       { return fat32_dup(fd); }
INT  vfs_create(const char *path)
{
    char buf[FAT32_MAX_PATH]; vfs_resolve(path, buf);
    return fat32_create_fd(buf);
}
INT  vfs_write(INT fd, const void *b, UW l){ return fat32_write(fd, b, l); }
INT  vfs_unlink(const char *path)
{
    char buf[FAT32_MAX_PATH]; vfs_resolve(path, buf);
    return fat32_unlink(buf);
}
INT  vfs_mkdir(const char *path)
{
    char buf[FAT32_MAX_PATH]; vfs_resolve(path, buf);
    return fat32_mkdir(buf);
}
INT  vfs_rename(const char *o, const char *n)
{
    char bo[FAT32_MAX_PATH], bn[FAT32_MAX_PATH];
    vfs_resolve(o, bo); vfs_resolve(n, bn);
    return fat32_rename(bo, bn);
}

INT  vfs_readdir(const char *path, VFS_DIRENT *out, INT max)
{
    char buf[FAT32_MAX_PATH]; vfs_resolve(path, buf);
    return fat32_readdir(buf, (FAT32_DIRENT *)out, max);
}

/* stat by path */
INT  vfs_stat_path(const char *path, UW *size, BOOL *is_dir)
{
    char buf[FAT32_MAX_PATH]; vfs_resolve(path, buf);
    return fat32_stat_path(buf, size, is_dir);
}

/* current working directory */
void vfs_getcwd(char *buf, INT len)
{
    INT i = 0;
    while (vfs_cwd[i] && i < len - 1) { buf[i] = vfs_cwd[i]; i++; }
    buf[i] = '\0';
}

INT  vfs_chdir(const char *path)
{
    char buf[FAT32_MAX_PATH]; vfs_resolve(path, buf);
    /* Verify the path is a directory */
    UW size; BOOL is_dir;
    if (fat32_stat_path(buf, &size, &is_dir) < 0) return -1;
    if (!is_dir) return -1;
    INT i = 0;
    while (buf[i] && i < FAT32_MAX_PATH - 1) { vfs_cwd[i] = buf[i]; i++; }
    vfs_cwd[i] = '\0';
    return 0;
}
