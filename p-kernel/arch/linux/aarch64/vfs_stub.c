/*
 *  arch/linux/aarch64/vfs_stub.c
 *  Same stubs as arch/aarch64/vfs_stub.c. The Linux port has no VFS
 *  yet; the distributed layer guards on vfs_ready=FALSE and skips
 *  persistence paths cleanly.
 */

#include "kernel.h"
#include "vfs.h"
#include "elf_loader.h"

BOOL vfs_ready = FALSE;

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
