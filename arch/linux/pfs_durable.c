/*
 *  pfs_durable.c — p-fs P0 durable backend (hosted / Linux userspace).
 *
 *  G24, wave 12-E: "the library is still volatile". The P0 block store
 *  (arch/common/pfs_block.c) keeps content-addressed blocks in an
 *  in-memory table; if every node loses power at once, the swarm's whole
 *  memory is gone. This file gives that table a disk under it.
 *
 *  Layout (all under $PKERNEL_PFS_DIR, created on first use):
 *    <64-hex-block-id>   raw block bytes; filename IS the sha256 id, so
 *                        the store is self-verifying — on reload we
 *                        recompute sha256(content) and reject any file
 *                        whose bytes do not match its name (bit-rot /
 *                        tampering caught for free). Content-addressed =>
 *                        a write is idempotent: same id always same bytes.
 *    refs.tab            the P2 named-ref table (name -> head manifest id),
 *                        the one mutable thing p-fs has; written by
 *                        pfs_dag.c through pfs_dur_write/read.
 *
 *  Power-loss discipline: each file is written to "<name>.tmp", fsync'd,
 *  atomically rename()d over the final name, then the directory is
 *  fsync'd so the rename itself survives a crash. The atomic rename means
 *  a reader never sees a half-written block; a crash mid-write leaves at
 *  most a stale ".tmp" the next write overwrites. (Honest limit: we trust
 *  the filesystem's fsync — on a lying disk cache nothing here can help;
 *  documented in docs/architecture/20-architecture/p-fs.md.)
 *
 *  This is a HOSTED-only backend (arch/linux/, shared by the aarch64 and
 *  x86_64 Linux ports). Bare-metal targets never compile this file; their
 *  pfs_block.c durable calls compile out under !_TK_HOSTED_LIBC_, so the
 *  store stays memory-only there (no-op backend) and still builds.
 *
 *  arch/linux discipline (see net_relay.c): this TU is built with the
 *  hosted CFLAGS and does NOT include kernel.h (its stddef/stdint shadows
 *  clash with the system headers we need). Plain C types only; the one
 *  cross-layer contract is the 64-hex block-id name == sha256 width (32 B).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>	/* _mkdir (single-arg on Windows) */
#include <io.h>		/* _commit (fsync equivalent)     */
#else
#include <sys/syscall.h>	/* SYS_syncfs (bionic has it as a syscall number too) */
#endif

/* sha256 digest is 32 bytes -> 64 lowercase hex chars per block-id name.
 * Kept as a literal here (kernel.h / pfs_block.h would drag the T-Kernel
 * stddef shadow into this hosted TU); the value is pinned against
 * PFS_ID_LEN by a _Static_assert on the pfs_block.c side. */
#define PFS_DUR_HEX_LEN   64
#define PFS_DUR_BLOCK_MAX 4096          /* must match PFS_BLOCK_MAX */
#define PFS_DUR_PATH_MAX  1024

static char dur_dir[512];
static int  dur_state = -1;             /* -1 unknown, 0 disabled, 1 ready */

/* Resolve (once) the durable directory from $PKERNEL_PFS_DIR. Creates it
 * if absent. Returns the path, or NULL when persistence is disabled
 * (env unset/empty) — that path keeps the in-memory store backward
 * compatible: no env => exactly the old behaviour. */
const char *pfs_dur_dir(void)
{
    if (dur_state < 0) {
        const char *e = getenv("PKERNEL_PFS_DIR");
        if (e && e[0]) {
            size_t n = strlen(e);
            if (n >= sizeof dur_dir) n = sizeof dur_dir - 1;
            memcpy(dur_dir, e, n);
            dur_dir[n] = '\0';
#ifdef _WIN32
            (void)_mkdir(dur_dir);          /* single-arg on Windows; EEXIST fine */
#else
            (void)mkdir(dur_dir, 0700);     /* idempotent; EEXIST is fine */
#endif
            dur_state = 1;
        } else {
            dur_state = 0;
        }
    }
    return dur_state == 1 ? dur_dir : (const char *)0;
}

/* 1 if a durable directory is configured, else 0. */
int pfs_dur_active(void)
{
    return pfs_dur_dir() != (const char *)0;
}

/* DUR-SWALLOW cert hook: when set non-zero, the very next pfs_dur_write FAILS
 * (returns -1) WITHOUT touching the disk, then auto-clears. This drives the
 * REAL production write seam through its failure branch so pfs_self_test can
 * prove pfs_put returns non-OK AND the block survives eviction — no sim. */
static int dur_force_fail_once;
void pfs_dur_force_fail(int on) { dur_force_fail_once = on ? 1 : 0; }

/* 0-14 S1 (swarm-leave.md): after `leave` this process must not write the
 * weights back (the mind and the baby in RAM still hold the swarm's share).
 * Until the process ends, writes of these two names fail. */
static int dur_weights_frozen;
void pfs_dur_freeze_weights(void) { dur_weights_frozen = 1; }
static int dur_is_weights(const char *fname)
{
    return strcmp(fname, "mind.rw") == 0 || strcmp(fname, "ns1_student.bin") == 0;
}

/* B5 D4 (research/b5-m-round.md §4): the student's chunks (quiet blocks, tens of
 * thousands a round) are written WITHOUT the two fsyncs (tmp + rename only) and
 * made durable by ONE syncfs at the end of the publish / the fetch
 * (pfs_dur_sync). A chunk cut by a power loss reads back as "not there": every
 * read re-hashes it against its id (pfs_get). Every fsync this file does is
 * counted (pfs_dur_fsync_count, W13). Windows has no syncfs: lazy = durable. */
static unsigned long dur_fsync_n;
/* W13c (research/b5-m-round.md §7): two narrower counts, so other tasks'
 * durable writes do not enter them: the fsyncs of writes that came in through
 * the lazy door (pfs_dur_write_lazy: the student's chunks and their .org; 0 in
 * the default build, every chunk under -DB5W_NC_FSYNC_EACH), and the syncfs
 * calls that ran (pfs_dur_sync with something pending). */
static unsigned long dur_lazy_fsync_n, dur_syncfs_n;
#ifndef _WIN32
static int dur_lazy_pending;
#endif
unsigned long pfs_dur_fsync_count(void) { return dur_fsync_n; }
unsigned long pfs_dur_lazy_fsync_count(void) { return __atomic_load_n(&dur_lazy_fsync_n, __ATOMIC_RELAXED); }
unsigned long pfs_dur_syncfs_count(void) { return __atomic_load_n(&dur_syncfs_n, __ATOMIC_RELAXED); }
static int dur_fsync_lz(int fd, int lazy)
{
    if (lazy) __atomic_add_fetch(&dur_lazy_fsync_n, 1, __ATOMIC_RELAXED);
    dur_fsync_n++;
#ifdef _WIN32
    return _commit(fd);
#else
    return fsync(fd);
#endif
}
static int dur_fsync(int fd) { return dur_fsync_lz(fd, 0); }
/* sync: 1 = fsync'd as before, 0 = no fsyncs (made durable by pfs_dur_sync);
 * lazy: the write came through the lazy door (counted for W13c) */
static int dur_write(const char *fname, const void *data, unsigned len, int sync, int lazy);
int pfs_dur_write(const char *fname, const void *data, unsigned len)
{
    return dur_write(fname, data, len, 1, 0);
}
int pfs_dur_write_lazy(const char *fname, const void *data, unsigned len)
{
#if defined(_WIN32) || defined(B5W_NC_FSYNC_EACH)
    return dur_write(fname, data, len, 1, 1);   /* falsifier W13: every chunk fsync'd */
#else
    return dur_write(fname, data, len, 0, 1);
#endif
}
/* one syncfs for everything written lazily since the last call; 0 = done or
 * nothing pending */
int pfs_dur_sync(void)
{
#ifndef _WIN32
    const char *dir = pfs_dur_dir();
    /* the mark is cleared BEFORE the syncfs: a lazy write by another task
     * (pfs_repl_rx) during it sets the mark again, never lost */
    if (!dir || !__atomic_exchange_n(&dur_lazy_pending, 0, __ATOMIC_ACQ_REL)) return 0;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) { __atomic_store_n(&dur_lazy_pending, 1, __ATOMIC_RELEASE); return -1; }
    dur_fsync_n++;
    __atomic_add_fetch(&dur_syncfs_n, 1, __ATOMIC_RELAXED);
    int r = (int)syscall(SYS_syncfs, dfd);
    close(dfd);
    if (r != 0) __atomic_store_n(&dur_lazy_pending, 1, __ATOMIC_RELEASE);
    return r == 0 ? 0 : -1;
#else
    return 0;
#endif
}

/* Atomic write of `len` bytes to <dir>/<fname>, fsync'd unless `sync` is 0.
 * Returns 0 on success, -1 on any failure (or when persistence is disabled). */
static int dur_write(const char *fname, const void *data, unsigned len, int sync, int lazy)
{
    const char *dir = pfs_dur_dir();
    if (!dir) return -1;

    if (dur_force_fail_once) { dur_force_fail_once = 0; return -1; }
    if (dur_weights_frozen && dur_is_weights(fname)) return -1;   /* S1: left */

    char path[PFS_DUR_PATH_MAX], tmp[PFS_DUR_PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", dir, fname) >= (int)sizeof path)
        return -1;
    if (snprintf(tmp, sizeof tmp, "%s/%s.tmp", dir, fname) >= (int)sizeof tmp)
        return -1;

#ifdef _WIN32
    /* O_BINARY: Windows text mode would translate bytes; the store is binary. */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
#else
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
#endif
    if (fd < 0) return -1;

    const unsigned char *p = (const unsigned char *)data;
    unsigned off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, (size_t)(len - off));
        if (w <= 0) { close(fd); unlink(tmp); return -1; }
        off += (unsigned)w;
    }
    if (sync && dur_fsync_lz(fd, lazy) != 0) { close(fd); unlink(tmp); return -1; }
    close(fd);

    /* S1: a weights write that began before `leave` must not land after it
     * (the 22 MB baby blob takes seconds; seen in run_swarm_leave.sh L2) */
    if (dur_weights_frozen && dur_is_weights(fname)) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }

#ifndef _WIN32
    /* fsync the directory so the rename (the durability point) survives a
     * power loss, not just the file's own data blocks. Windows has no
     * directory-fsync fd; the rename is still atomic (P1 accepts the
     * weaker crash-durability). */
    if (sync) {
        int dfd = open(dir, O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) { (void)dur_fsync_lz(dfd, lazy); close(dfd); }
    } else {
        __atomic_store_n(&dur_lazy_pending, 1, __ATOMIC_RELEASE);
    }
#endif
    return 0;
}

/* Read up to maxlen bytes of <dir>/<fname> into buf. Returns the byte
 * count read, or -1 (missing / disabled / error). */
int pfs_dur_read(const char *fname, void *buf, unsigned maxlen)
{
    const char *dir = pfs_dur_dir();
    if (!dir) return -1;

    char path[PFS_DUR_PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", dir, fname) >= (int)sizeof path)
        return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    unsigned char *p = (unsigned char *)buf;
    unsigned off = 0;
    while (off < maxlen) {
        ssize_t r = read(fd, p + off, (size_t)(maxlen - off));
        if (r < 0) { close(fd); return -1; }
        if (r == 0) break;
        off += (unsigned)r;
    }
    close(fd);
    return (int)off;
}

/* 1 unless <dir>/<fname> is certainly absent (stat says ENOENT): any other failure
 * counts as "there" so a caller never replaces a file it merely could not read
 * (ark_rec.c's ark.key, audit-172). 0 when persistence is disabled. */
int pfs_dur_exists(const char *fname)
{
    const char *dir = pfs_dur_dir();
    if (!dir) return 0;
    char path[PFS_DUR_PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", dir, fname) >= (int)sizeof path)
        return 1;
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    return errno == ENOENT ? 0 : 1;
}

/* Remove <dir>/<fname> (pfs_drop: a superseded block must not come back at
 * the next restore). Returns 0 on success, -1 otherwise. */
int pfs_dur_remove(const char *fname)
{
    const char *dir = pfs_dur_dir();
    if (!dir) return -1;
    char path[PFS_DUR_PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", dir, fname) >= (int)sizeof path)
        return -1;
    return unlink(path) == 0 ? 0 : -1;
}

/* True if `s` is exactly PFS_DUR_HEX_LEN lowercase-hex chars — i.e. looks
 * like a block-id filename (skips ".", "..", "refs.tab", "*.tmp"). */
static int is_block_name(const char *s)
{
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return 0;
        if (n >= PFS_DUR_HEX_LEN) return 0;
    }
    return n == PFS_DUR_HEX_LEN;
}

static unsigned char dur_rdbuf[PFS_DUR_BLOCK_MAX];

/* Scan the durable directory and hand every block file (name + raw bytes)
 * to `cb`. Reading happens here (hosted readdir/IO); verification and
 * table insertion happen in the caller (pfs_block.c), which knows sha256.
 * Returns the number of block files visited, or -1 if disabled/unopenable. */
int pfs_dur_foreach(void (*cb)(const char *name, const void *data,
                               unsigned len, void *ctx),
                    void *ctx)
{
    const char *dir = pfs_dur_dir();
    if (!dir) return -1;

    DIR *d = opendir(dir);
    if (!d) return -1;

    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != (struct dirent *)0) {
        if (!is_block_name(de->d_name)) continue;
        int len = pfs_dur_read(de->d_name, dur_rdbuf, sizeof dur_rdbuf);
        if (len < 0) continue;
        if (cb) cb(de->d_name, dur_rdbuf, (unsigned)len, ctx);
        count++;
    }
    closedir(d);
    return count;
}

/* ---- ROADMAP 0-14 S1 (docs/architecture/30-module/swarm-leave.md) ----------
 * Each flat block's origin lives beside it in "<64 hex>.org": 2 bytes, little
 * endian (the hosted pk_node_t). The block file name stays the sha256 (the
 * store is still self-verifying); a block without its .org is "unknown"
 * (written before S1, or the .org was lost). */
static int dur_org_write(const char *hex, unsigned origin, int lazy)
{
    char name[PFS_DUR_HEX_LEN + 8];
    unsigned char b[2];
    if (snprintf(name, sizeof name, "%s.org", hex) >= (int)sizeof name) return -1;
    b[0] = (unsigned char)(origin & 0xFFu);
    b[1] = (unsigned char)((origin >> 8) & 0xFFu);
    return lazy ? pfs_dur_write_lazy(name, b, 2) : pfs_dur_write(name, b, 2);
}
int pfs_dur_org_write(const char *hex, unsigned origin) { return dur_org_write(hex, origin, 0); }
/* B5 D4: a chunk's .org goes with the chunk (made durable by pfs_dur_sync) */
int pfs_dur_org_write_lazy(const char *hex, unsigned origin) { return dur_org_write(hex, origin, 1); }

/* the origin recorded for block <hex>, or -1 when there is none */
int pfs_dur_org_read(const char *hex)
{
    char name[PFS_DUR_HEX_LEN + 8];
    unsigned char b[3];
    if (snprintf(name, sizeof name, "%s.org", hex) >= (int)sizeof name) return -1;
    if (pfs_dur_read(name, b, sizeof b) != 2) return -1;
    return (int)b[0] | ((int)b[1] << 8);
}

/* remove block <hex> and its .org (0 if the block file is gone afterwards) */
int pfs_dur_remove_block(const char *hex)
{
    char name[PFS_DUR_HEX_LEN + 8];
    (void)pfs_dur_remove(hex);
    if (snprintf(name, sizeof name, "%s.org", hex) < (int)sizeof name) (void)pfs_dur_remove(name);
    return pfs_dur_exists(hex) ? -1 : 0;
}

/* move block <hex> into <dir>/<sub>/ (kept, not deleted). 0 on success */
int pfs_dur_move_aside(const char *hex, const char *sub)
{
    const char *dir = pfs_dur_dir();
    char sd[PFS_DUR_PATH_MAX], from[PFS_DUR_PATH_MAX], to[PFS_DUR_PATH_MAX];
    if (!dir) return -1;
    if (snprintf(sd, sizeof sd, "%s/%s", dir, sub) >= (int)sizeof sd) return -1;
    if (snprintf(from, sizeof from, "%s/%s", dir, hex) >= (int)sizeof from) return -1;
    if (snprintf(to, sizeof to, "%s/%s/%s", dir, sub, hex) >= (int)sizeof to) return -1;
#ifdef _WIN32
    (void)_mkdir(sd);
#else
    (void)mkdir(sd, 0700);
#endif
    if (rename(from, to) != 0) return -1;
#ifndef _WIN32
    { int dfd = open(dir, O_RDONLY | O_DIRECTORY); if (dfd >= 0) { (void)dur_fsync(dfd); close(dfd); } }
#endif
    return 0;
}

/* hand every block file's name and recorded origin (-1: none) to cb; the
 * number visited, or -1 when persistence is off. cb may remove or move the
 * file it is given (the names are copied out before any callback runs). */
int pfs_dur_foreach_origin(void (*cb)(const char *hex, int origin, void *ctx), void *ctx)
{
    const char *dir = pfs_dur_dir();
    static char names[4096][PFS_DUR_HEX_LEN + 1];
    int n = 0, i;
    DIR *d;
    struct dirent *de;
    if (!dir) return -1;
    d = opendir(dir);
    if (!d) return -1;
    while ((de = readdir(d)) != (struct dirent *)0 && n < 4096) {
        if (!is_block_name(de->d_name)) continue;
        memcpy(names[n], de->d_name, PFS_DUR_HEX_LEN + 1);
        n++;
    }
    closedir(d);
    for (i = 0; i < n; i++) if (cb) cb(names[i], pfs_dur_org_read(names[i]), ctx);
    return n;
}
