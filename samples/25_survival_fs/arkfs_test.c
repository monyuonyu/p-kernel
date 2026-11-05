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
 *  ROADMAP 0-8 (allpoints.sh — every power-cut point of every operation):
 *      writefill <img> <path> <nbytes> <char>   # deterministic multi-block file
 *      unlink  <img> <path>
 *      mkdir   <img> <path>
 *      checkpoint <img>
 *      compact <img>
 *      dump    <img> <path>...      # one line per path: version, size, sha256
 *      compactrf <img> <text> <N> [hdr|rot]   # compact with a read error on <text>'s block
 *  ARK_COUNT=1 prints "[count] writes=<N>" on stderr at exit (the number of
 *  device writes the one operation made = the cut points 1..N).
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
#include "sha256.h"

static void emit_stdout(const char *s) { fputs(s, stdout); fflush(stdout); }

/* test-only seam in arkfs.c (as samples/37_ark_merkle/merkle_test.c) */
extern U4 ark_dbg_id_sector(const U1 id[ARK_ID_LEN]);

/* ---- file-backed block device with fault injection ---- */
static int   g_fd = -1;
static U4    g_nsect;
static long  g_kill_before = -1;   /* 1-based write index, or -1 */
static long  g_kill_torn   = -1;
static long  g_wcnt;

/* compactrf: reads touching g_rf_sec fail from the g_rf_from-th such read on,
 * only while armed (around ark_compact) */
static int   g_rf_arm = 0, g_rf_from = 1, g_rf_hits = 0;
static U4    g_rf_sec = 0;
/* compactrf mode "rot": no error, but the N-th read alone (only that one)
 * returns the sector with one byte flipped (a transient bad read, audit-134) */
static int   g_rf_rot = 0;

static int fb_read(void *c, U4 lba, U4 n, void *buf)
{
    (void)c;
    if (lba + n > g_nsect) return -1;
    int hit = g_rf_arm && g_rf_sec >= lba && g_rf_sec < lba + n;
    if (hit) g_rf_hits++;
    if (hit && !g_rf_rot && g_rf_hits >= g_rf_from)
        return -1;
    off_t off = (off_t)lba * ARK_SECTOR;
    ssize_t got = pread(g_fd, buf, (size_t)n * ARK_SECTOR, off);
    if (got != (ssize_t)((size_t)n * ARK_SECTOR)) return -1;
    if (hit && g_rf_rot && g_rf_hits == g_rf_from)
        ((U1 *)buf)[(size_t)(g_rf_sec - lba) * ARK_SECTOR + 7] ^= 0x5A;
    return 0;
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

static void print_count(void) { fprintf(stderr, "[count] writes=%ld\n", g_wcnt); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <cmd> ...\n", argv[0]); return 2; }
    if (getenv("ARK_COUNT")) atexit(print_count);
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
    if (!strcmp(cmd, "writefill")) {
        if (argc < 6) return 2;
        static char fill[ARK_BLOCK_MAX * ARK_MAX_BLK];
        long n = atol(argv[4]);
        if (n < 0 || n > (long)sizeof(fill)) return 2;
        for (long i = 0; i < n; i++) fill[i] = (char)(argv[5][0] + (i % 23));
        int r = ark_write_file(argv[3], fill, (U4)n);
        printf("WRITE: %d ver=%d\n", r, ark_version(argv[3]));
        return r == ARK_OK ? 0 : 1;
    }
    if (!strcmp(cmd, "unlink")) {
        if (argc < 4) return 2;
        int r = ark_unlink(argv[3]);
        printf("UNLINK: %d\n", r);
        return r == ARK_OK ? 0 : 1;
    }
    if (!strcmp(cmd, "mkdir")) {
        if (argc < 4) return 2;
        int r = ark_mkdir(argv[3]);
        printf("MKDIR: %d\n", r);
        return r == ARK_OK ? 0 : 1;
    }
    /* bput <img> <text>: a raw p-fs block the way arch/linux/pfs_ark.c pfs_ark_put
     * stores one (ark_block_put + ark_checkpoint, no file names it); prints the id.
     * bget <img> <id hex>: reads it back. (e) in run.sh: such a block must survive
     * ark_compact (the ark entry's records are p-fs blocks, ark-entry.md §2.3). */
    if (!strcmp(cmd, "bput")) {
        if (argc < 4) return 2;
        U1 id[ARK_ID_LEN];
        int r = ark_block_put(argv[3], u4len(argv[3]), id);
        int c = (r == ARK_OK) ? ark_checkpoint() : r;
        printf("BPUT: %d %d ", r, c);
        for (int i = 0; i < ARK_ID_LEN; i++) printf("%02x", id[i]);
        printf("\n");
        return (r == ARK_OK && c == ARK_OK) ? 0 : 1;
    }
    if (!strcmp(cmd, "bget")) {
        if (argc < 4 || strlen(argv[3]) != 2 * ARK_ID_LEN) return 2;
        U1 id[ARK_ID_LEN];
        for (int i = 0; i < ARK_ID_LEN; i++) {
            unsigned v; if (sscanf(argv[3] + 2 * i, "%2x", &v) != 1) return 2;
            id[i] = (U1)v;
        }
        static char buf[ARK_BLOCK_MAX + 1];
        int r = ark_block_get(id, buf, ARK_BLOCK_MAX);
        if (r == ARK_E_CORRUPT) { printf("CORRUPT\n"); return 3; }
        if (r < 0) { printf("NOTFOUND %d\n", r); return 1; }
        buf[r] = '\0';
        printf("BGET: %s\n", buf);
        return 0;
    }
    /* compactrf <img> <text> <N>: ark_compact while every read of the payload
     * sector of the block holding <text> fails from the N-th such read on (a
     * read error mid-compaction). The device is healed afterwards. Optional
     * 5th word: "hdr" = the block's header sector instead of the payload;
     * "rot" = no error, the N-th read alone returns a flipped byte (audit-134). */
    if (!strcmp(cmd, "compactrf")) {
        if (argc < 5) return 2;
        U1 id[ARK_ID_LEN];
        sha256_ctx sc; sha256_init(&sc); sha256_update(&sc, argv[3], u4len(argv[3])); sha256_final(&sc, id);
        g_rf_sec = ark_dbg_id_sector(id);
        if (g_rf_sec == 0) { printf("COMPACTRF: no such block\n"); return 2; }
        if (argc > 5 && !strcmp(argv[5], "hdr")) g_rf_sec -= 1u;   /* the header sector */
        g_rf_rot = argc > 5 && !strcmp(argv[5], "rot");
        g_rf_from = atoi(argv[4]); g_rf_hits = 0; g_rf_arm = 1;
        int r = ark_compact();
        g_rf_arm = 0;
        printf("COMPACTRF: %d\n", r);
        return 0;
    }
    if (!strcmp(cmd, "checkpoint")) {
        int r = ark_checkpoint();
        printf("CHECKPOINT: %d\n", r);
        return r == ARK_OK ? 0 : 1;
    }
    if (!strcmp(cmd, "compact")) {
        int r = ark_compact();
        printf("COMPACT: %d\n", r);
        return r == ARK_OK ? 0 : 1;
    }
    if (!strcmp(cmd, "dump")) {
        static char buf[ARK_BLOCK_MAX * ARK_MAX_BLK + 1];
        for (int a = 3; a < argc; a++) {
            U4 size = 0, ver = 0; INT is_dir = 0;
            if (ark_stat(argv[a], &size, &is_dir, &ver) < 0) {
                printf("%s absent\n", argv[a]);
                continue;
            }
            if (is_dir) { printf("%s dir v%u\n", argv[a], ver); continue; }
            int r = ark_read_file(argv[a], buf, sizeof(buf) - 1);
            if (r < 0) { printf("%s v%u size=%u READ-FAIL(%d)\n", argv[a], ver, size, r); continue; }
            unsigned char h[32];
            sha256(buf, (size_t)r, h);
            printf("%s v%u size=%u got=%d sha=", argv[a], ver, size, r);
            for (int i = 0; i < 8; i++) printf("%02x", h[i]);
            printf("\n");
        }
        return 0;
    }

    fprintf(stderr, "unknown cmd %s\n", cmd);
    return 2;
}
