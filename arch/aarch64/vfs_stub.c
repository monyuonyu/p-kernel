/*
 *  vfs_stub.c (aarch64)
 *  Phase 2c step 2 stubs — AArch64 has no IDE/FAT32 stack yet.
 *
 *  The distributed-layer code (mem_store, sfs, chat, kloader_task, heal,
 *  dproc) was originally written against the x86 VFS+ELF loader. To get
 *  it linking on AArch64 we provide empty stubs:
 *
 *    - vfs_ready = FALSE so callers that guard with `if (vfs_ready)`
 *      skip their persistence/file-IO paths.
 *    - The function stubs return failure for callers that don't guard;
 *      they treat fd<0 / -1 as "operation skipped" and continue cleanly.
 *
 *  When AArch64 gains a real block device (SD card on RPi, virtio-blk on
 *  QEMU virt, etc.) this file is replaced by a proper VFS implementation
 *  in arch/aarch64/, mirroring arch/x86/vfs.c.
 */

#include "kernel.h"
#include "vfs.h"
#include "elf_loader.h"
#include "arkfs.h"
#include "ark_bdev.h"
#include "virtio_blk.h"

BOOL vfs_ready = FALSE;

/* ----------------------------------------------------------------- */
/* ARK backend (aarch64).                                            */
/*                                                                    */
/* AArch64 has no FAT32 stack, so the generic VFS file ops above stay */
/* stubs. But ARK — the log-structured survival FS — now has a real   */
/* block device on the QEMU virt machine (virtio-blk MMIO), so its    */
/* whole-file API is wired here, mirroring arch/x86/vfs.c's           */
/* VFS_BACKEND_ARK path but scoped to aarch64. `dev` is ignored:      */
/* there is one virtio-blk device, bound directly (no blk_ssy layer). */
/* ----------------------------------------------------------------- */
static VFS_BACKEND g_backend = VFS_BACKEND_FAT32;
static ARK_BDEV    g_ark_bd;

VFS_BACKEND vfs_active_backend(void) { return g_backend; }

INT vfs_ark_mount(const char *dev, BOOL do_format)
{
    (void)dev;
    if (virtio_blk_init() < 0) return -1;
    INT rc = ark_bdev_bind(&g_ark_bd);
    if (rc != ARK_OK) return rc;
    if (do_format) {
        rc = ark_format(&g_ark_bd);
        if (rc != ARK_OK) return rc;
    }
    rc = ark_mount(&g_ark_bd);
    if (rc != ARK_OK) return rc;
    g_backend = VFS_BACKEND_ARK;
    return 0;
}

INT vfs_ark_write_file(const char *path, const void *buf, UW len)
{
    if (g_backend != VFS_BACKEND_ARK) return -1;
    return ark_write_file(path, buf, (U4)len);
}

INT vfs_ark_read_file(const char *path, void *buf, UW max)
{
    if (g_backend != VFS_BACKEND_ARK) return -1;
    return ark_read_file(path, buf, (U4)max);
}

INT  vfs_init(void)                                    { return -1; }
INT  vfs_open(const char *p)                           { (void)p; return -1; }
INT  vfs_read(INT fd, void *b, UW n)                   { (void)fd; (void)b; (void)n; return -1; }
INT  vfs_seek(INT fd, UW o)                            { (void)fd; (void)o; return -1; }
UW   vfs_fsize(INT fd)                                 { (void)fd; return 0; }
void vfs_close(INT fd)                                 { (void)fd; }
INT  vfs_create(const char *p)                         { (void)p; return -1; }
INT  vfs_write(INT fd, const void *b, UW n)            { (void)fd; (void)b; (void)n; return -1; }
INT  vfs_unlink(const char *p)                         { (void)p; return -1; }
INT  vfs_mkdir(const char *p)                          { (void)p; return -1; }
INT  vfs_rename(const char *o, const char *n)          { (void)o; (void)n; return -1; }
INT  vfs_readdir(const char *p, VFS_DIRENT *o, INT m)  { (void)p; (void)o; (void)m; return -1; }
INT  vfs_stat_path(const char *p, UW *s, BOOL *d)      { (void)p; (void)s; (void)d; return -1; }
INT  vfs_dup(INT fd)                                   { (void)fd; return -1; }
INT  vfs_dup2(INT o, INT n)                            { (void)o; (void)n; return -1; }
void vfs_getcwd(char *b, INT n)                        { if (n > 0) b[0] = '\0'; }
INT  vfs_chdir(const char *p)                          { (void)p; return -1; }

ID   elf_exec(const char *path, const char *cmdline)
{
    (void)path; (void)cmdline;
    return -1;
}

/* Foreign-context user-process teardown (arch/common/dproc.c kill
 * path).  No ring-3 ELF processes on this target — nothing to free. */
void user_proc_teardown(ID tid) { (void)tid; }
