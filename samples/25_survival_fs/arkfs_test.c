/*
 *  arkfs_test.c — host harness for ARK (arch/common/arkfs.c).
 *
 *  Compiles the REAL arch/common/arkfs.c (ARK_HOST_TEST type shim) against a
 *  *file-backed* block device so crash consistency can be proven with a real,
 *  uncatchable SIGKILL between two separate processes:
 *
 *      proc1: ark_write_file(...) -- SIGKILL'd mid-commit (power loss)
 *      proc2: ark_mount(...) + ark_read_file(...) -- must see the last
 *             committed version, intact; the torn write is rolled back.
 *
 *  Fault injection lives in the block device (where "power loss" physically
 *  happens), driven by env vars:
 *      ARK_KILL_BEFORE=N   SIGKILL just before the N-th device write
 *                          (a write that never reached the platter).
 *      ARK_KILL_TORN=N     half-write the N-th sector, fsync, then SIGKILL
 *                          (a torn sector — the classic power-loss artifact).
 *
 *  Subcommands (one ark op per process, so the kill is real):
 *      format  <img> <nsectors>
 *      write   <img> <path> <string>
 *      read    <img> <path>
 *      version <img> <path>
 *      readv   <img> <path> <ver>
 *      history <img> <path>
 *      ls      <img> <dir>
 *      blocks  <img>                # stored distinct block count (dedup proof)
 *      corrupt <img> <byteoffset>   # flip one byte (rot injection)
 *      selftest                     # in-process RAM self-test
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#include "arkfs.h"

static void emit_stdout(const char *s) { fputs(s, stdout); fflush(stdout); }

/* ---- file-backed block device with fault injection ---- */
static int   g_fd = -1;
static U4    g_nsect;
static long  g_kill_before = -1;   /* 1-based write index, or -1 */
static long  g_kill_torn   = -1;
static long  g_wcnt;

static int fb_read(void *c, U4 lba, U4 n, void *buf)
{
    (void)c;
    if (lba + n > g_nsect) return -1;
    off_t off = (off_t)lba * ARK_SECTOR;
    ssize_t got = pread(g_fd, buf, (size_t)n * ARK_SECTOR, off);
    return (got == (ssize_t)((size_t)n * ARK_SECTOR)) ? 0 : -1;
}

static int fb_write(void *c, U4 lba, U4 n, const void *buf)
{
    (void)c;
    if (lba + n > g_nsect) return -1;
    g_wcnt++;

    if (g_kill_before == g_wcnt) {
        fsync(g_fd);
        fprintf(stderr, "[inject] SIGKILL before write #%ld (lba %u)\n",
                g_wcnt, lba);
        kill(getpid(), SIGKILL);
    }

    off_t off = (off_t)lba * ARK_SECTOR;
    if (g_kill_torn == g_wcnt) {
        /* A real torn sector: the first half lands, the second half is left
         * indeterminate. Model "indeterminate" as garbage (not zeros) so the
         * tear is genuinely destructive regardless of payload content. */
        size_t half = ARK_SECTOR / 2;
        unsigned char junk[ARK_SECTOR / 2];
        memset(junk, 0xA5, sizeof(junk));
        pwrite(g_fd, buf, half, off);
        pwrite(g_fd, junk, half, off + (off_t)half);
        fsync(g_fd);
        fprintf(stderr, "[inject] torn half-sector then SIGKILL at write #%ld "
                "(lba %u)\n", g_wcnt, lba);
        kill(getpid(), SIGKILL);
    }

    ssize_t put = pwrite(g_fd, buf, (size_t)n * ARK_SECTOR, off);
    return (put == (ssize_t)((size_t)n * ARK_SECTOR)) ? 0 : -1;
}

static int fb_sync(void *c) { (void)c; return fsync(g_fd); }

static ARK_BDEV g_bd;

static int open_img(const char *path, int create)
{
    int flags = O_RDWR | (create ? O_CREAT : 0);
    g_fd = open(path, flags, 0644);
    if (g_fd < 0) { perror("open"); return -1; }
    off_t sz = lseek(g_fd, 0, SEEK_END);
    g_nsect = (U4)(sz / ARK_SECTOR);
    const char *kb = getenv("ARK_KILL_BEFORE");
    const char *kt = getenv("ARK_KILL_TORN");
    g_kill_before = kb ? atol(kb) : -1;
    g_kill_torn   = kt ? atol(kt) : -1;
    g_wcnt = 0;
    g_bd.sector_size   = ARK_SECTOR;
    g_bd.total_sectors = g_nsect;
    g_bd.read  = fb_read;
    g_bd.write = fb_write;
    g_bd.sync  = fb_sync;
    g_bd.ctx   = NULL;
    return 0;
}

static U4 u4len(const char *s) { U4 n = 0; while (s[n]) n++; return n; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <cmd> ...\n", argv[0]); return 2; }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "selftest")) {
        return ark_self_test(emit_stdout);
    }

    if (!strcmp(cmd, "format")) {
        if (argc < 4) return 2;
        U4 nsect = (U4)atol(argv[3]);
        int fd = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("open"); return 1; }
        if (ftruncate(fd, (off_t)nsect * ARK_SECTOR) < 0) { perror("ftruncate"); return 1; }
        close(fd);
        if (open_img(argv[2], 0) < 0) return 1;
        int r = ark_format(&g_bd);
        printf("FORMAT: %d (%u sectors)\n", r, nsect);
        return r == ARK_OK ? 0 : 1;
    }

    if (!strcmp(cmd, "corrupt")) {
        if (argc < 4) return 2;
        long o = atol(argv[3]);
        int fd = open(argv[2], O_RDWR);
        if (fd < 0) { perror("open"); return 1; }
        unsigned char b;
        if (pread(fd, &b, 1, o) != 1) { perror("pread"); return 1; }
        b ^= 0xFF;
        if (pwrite(fd, &b, 1, o) != 1) { perror("pwrite"); return 1; }
        fsync(fd); close(fd);
        printf("CORRUPT: flipped byte at offset %ld\n", o);
        return 0;
    }

    /* all remaining commands mount an existing image */
    if (argc < 3) return 2;
    if (open_img(argv[2], 0) < 0) return 1;
    if (ark_mount(&g_bd) != ARK_OK) { printf("MOUNT-FAIL\n"); return 1; }

    if (!strcmp(cmd, "write")) {
        if (argc < 5) return 2;
        int r = ark_write_file(argv[3], argv[4], u4len(argv[4]));
        printf("WRITE: %d ver=%d\n", r, ark_version(argv[3]));
        return r == ARK_OK ? 0 : 1;
    }
    if (!strcmp(cmd, "read")) {
        if (argc < 4) return 2;
        static char buf[ARK_BLOCK_MAX * ARK_MAX_BLK + 1];
        int r = ark_read_file(argv[3], buf, sizeof(buf) - 1);
        if (r == ARK_E_CORRUPT) { printf("CORRUPT\n"); return 3; }
        if (r < 0) { printf("NOTFOUND\n"); return 1; }
        buf[r] = '\0';
        printf("READ: %s\n", buf);
        return 0;
    }
    if (!strcmp(cmd, "version")) {
        printf("VERSION: %d\n", ark_version(argv[3]));
        return 0;
    }
    if (!strcmp(cmd, "readv")) {
        if (argc < 5) return 2;
        static char buf[ARK_BLOCK_MAX * ARK_MAX_BLK + 1];
        int r = ark_read_version(argv[3], (U4)atol(argv[4]), buf, sizeof(buf) - 1);
        if (r == ARK_E_CORRUPT) { printf("CORRUPT\n"); return 3; }
        if (r < 0) { printf("NOTFOUND\n"); return 1; }
        buf[r] = '\0';
        printf("READV(%s): %s\n", argv[4], buf);
        return 0;
    }
    if (!strcmp(cmd, "history")) {
        ARK_HIST h[16];
        int n = ark_history(argv[3], h, 16);
        printf("HISTORY: %d versions\n", n);
        for (int i = 0; i < n; i++)
            printf("  v%u  commit_seq=%u  size=%u\n",
                   h[i].version, h[i].commit_seq, h[i].size);
        return 0;
    }
    if (!strcmp(cmd, "ls")) {
        ARK_DIRENT de[ARK_MAX_FILES];
        int n = ark_readdir(argv[3], de, ARK_MAX_FILES);
        printf("LS %s: %d entries\n", argv[3], n);
        for (int i = 0; i < n; i++)
            printf("  %s%s  size=%u v%u\n", de[i].name,
                   de[i].is_dir ? "/" : "", de[i].size, de[i].version);
        return 0;
    }
    if (!strcmp(cmd, "blocks")) {
        printf("BLOCKS: %u\n", ark_block_count());
        return 0;
    }

    fprintf(stderr, "unknown cmd %s\n", cmd);
    return 2;
}
