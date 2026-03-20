/*
 *  vfs.c (x86)
 *  Virtual File System — delegates to FAT32
 */

#include "kernel.h"
#include "ide.h"
#include "fat32.h"
#include "vfs.h"
#include <tmonitor.h>

BOOL vfs_ready = FALSE;

INT vfs_init(void)
{
    if (ide_init() < 0) {
        tm_putstring((UB *)"[vfs]  IDE not found — filesystem unavailable\r\n");
        return -1;
    }
    if (fat32_mount() < 0) {
        tm_putstring((UB *)"[vfs]  FAT32 mount failed\r\n");
        return -1;
    }
    vfs_ready = TRUE;
    tm_putstring((UB *)"[vfs]  ready\r\n");
    return 0;
}

INT  vfs_open(const char *path)        { return fat32_open(path); }
INT  vfs_read(INT fd, void *b, UW l)   { return fat32_read(fd, b, l); }
INT  vfs_seek(INT fd, UW off)          { return fat32_seek(fd, off); }
UW   vfs_fsize(INT fd)                 { return fat32_fsize(fd); }
void vfs_close(INT fd)                 { fat32_close(fd); }

INT  vfs_readdir(const char *path, VFS_DIRENT *out, INT max)
{
    /* Reuse FAT32_DIRENT which has identical layout */
    return fat32_readdir(path, (FAT32_DIRENT *)out, max);
}
