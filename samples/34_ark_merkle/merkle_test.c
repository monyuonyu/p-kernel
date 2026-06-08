/*
 *  merkle_test.c — host harness for ARK's Merkle directory tree
 *                  (arch/common/arkfs.c, format v3).
 *
 *  Compiles the REAL arch/common/arkfs.c (ARK_HOST_TEST type shim) against a
 *  *file-backed* block device, so the Merkle dir-tree properties can be proven
 *  cross-process — including a real, uncatchable SIGKILL between two processes
 *  for the crash-rollback case:
 *
 *      proc1: ark_mtree_put(...) -- SIGKILL'd mid-commit (power loss)
 *      proc2: ark_mount(...) + ark_mtree_root/get(...) -- must see the prior
 *             committed root, intact; the torn dir update is rolled back.
 *
 *  Fault injection lives in the block device (the same ARK_KILL_BEFORE /
 *  ARK_KILL_TORN env-var mechanism samples/25 uses).
 *
 *  Subcommands (one ark op per process, so a kill is a real power loss):
 *      mformat <img> <nsectors>
 *      mput    <img> <path> <string>     # insert/replace a file in the tree
 *      mget    <img> <path>              # read a file back (self-verified)
 *      mroot   <img>                     # print the Merkle root id (hex)
 *      mls     <img> <dir>              # list a directory node's children
 *      mtamper <img>                     # flip a byte of the ROOT node on disk
 *
 *  Exit codes: 0 ok, 1 not-found/error, 3 ARK_E_CORRUPT (rot/tamper detected).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include "arkfs.h"

/* test-only seam in arkfs.c (see ark_dbg_id_sector). */
extern U4 ark_dbg_id_sector(const U1 id[ARK_ID_LEN]);

/* ---- file-backed block device with fault injection (mirrors samples/25) ---- */
static int   g_fd = -1;
static U4    g_nsect;
static long  g_kill_before = -1;
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
        fprintf(stderr, "[inject] SIGKILL before write #%ld (lba %u)\n", g_wcnt, lba);
        kill(getpid(), SIGKILL);
    }

    off_t off = (off_t)lba * ARK_SECTOR;
    if (g_kill_torn == g_wcnt) {
        size_t half = ARK_SECTOR / 2;
        unsigned char junk[ARK_SECTOR / 2];
        memset(junk, 0xA5, sizeof(junk));
        ssize_t w1 = pwrite(g_fd, buf, half, off);
        ssize_t w2 = pwrite(g_fd, junk, half, off + (off_t)half);
        (void)w1; (void)w2;
        fsync(g_fd);
        fprintf(stderr, "[inject] torn half-sector then SIGKILL at write #%ld (lba %u)\n",
                g_wcnt, lba);
        kill(getpid(), SIGKILL);
    }

    ssize_t put = pwrite(g_fd, buf, (size_t)n * ARK_SECTOR, off);
    return (put == (ssize_t)((size_t)n * ARK_SECTOR)) ? 0 : -1;
}

static int fb_sync(void *c) { (void)c; return fsync(g_fd); }

static ARK_BDEV g_bd;

static int open_img(const char *path)
{
    g_fd = open(path, O_RDWR);
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

static void print_hex(const char *pfx, const U1 *id)
{
    printf("%s", pfx);
    for (int i = 0; i < ARK_ID_LEN; i++) printf("%02x", id[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <cmd> <img> ...\n", argv[0]); return 2; }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "mformat")) {
        if (argc < 4) return 2;
        U4 nsect = (U4)atol(argv[3]);
        int fd = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("open"); return 1; }
        if (ftruncate(fd, (off_t)nsect * ARK_SECTOR) < 0) { perror("ftruncate"); return 1; }
        close(fd);
        if (open_img(argv[2]) < 0) return 1;
        int r = ark_format(&g_bd);
        printf("MFORMAT: %d (%u sectors)\n", r, nsect);
        return r == ARK_OK ? 0 : 1;
    }

    if (open_img(argv[2]) < 0) return 1;
    if (ark_mount(&g_bd) != ARK_OK) { printf("MOUNT-FAIL\n"); return 1; }

    if (!strcmp(cmd, "mput")) {
        if (argc < 5) return 2;
        int r = ark_mtree_put(argv[3], argv[4], u4len(argv[4]));
        printf("MPUT %s: %d\n", argv[3], r);
        return r == ARK_OK ? 0 : 1;
    }
    if (!strcmp(cmd, "mget")) {
        if (argc < 4) return 2;
        static char buf[ARK_BLOCK_MAX + 1];
        int r = ark_mtree_get(argv[3], buf, sizeof(buf) - 1);
        if (r == ARK_E_CORRUPT) { printf("CORRUPT\n"); return 3; }
        if (r < 0) { printf("NOTFOUND\n"); return 1; }
        buf[r] = '\0';
        printf("MGET: %s\n", buf);
        return 0;
    }
    if (!strcmp(cmd, "mroot")) {
        U1 root[ARK_ID_LEN];
        ark_mtree_root(root);
        print_hex("MROOT: ", root);
        return 0;
    }
    if (!strcmp(cmd, "mls")) {
        const char *dir = (argc >= 4) ? argv[3] : "/";
        ARK_DIRENT de[64];
        int n = ark_mtree_list(dir, de, 64);
        if (n < 0) { printf("MLS-ERR: %d\n", n); return 1; }
        printf("MLS %s: %d entries\n", dir, n);
        for (int i = 0; i < n; i++)
            printf("  %s%s size=%u\n", de[i].name, de[i].is_dir ? "/" : "", de[i].size);
        return 0;
    }
    if (!strcmp(cmd, "mtamper")) {
        /* flip a byte of the ROOT dir node's payload, on disk, then exit. */
        U1 root[ARK_ID_LEN];
        ark_mtree_root(root);
        U4 sec = ark_dbg_id_sector(root);
        if (sec == 0) { printf("ROOT-NOT-INDEXED\n"); return 1; }
        off_t off = (off_t)sec * ARK_SECTOR + 16;
        unsigned char b;
        if (pread(g_fd, &b, 1, off) != 1) { perror("pread"); return 1; }
        b ^= 0xFF;
        if (pwrite(g_fd, &b, 1, off) != 1) { perror("pwrite"); return 1; }
        fsync(g_fd);
        printf("MTAMPER: flipped root-node byte at sector %u\n", sec);
        return 0;
    }

    fprintf(stderr, "unknown cmd %s\n", cmd);
    return 2;
}
