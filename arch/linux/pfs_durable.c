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
 *  documented in docs/architecture/p-fs.md.)
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
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

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
            (void)mkdir(dur_dir, 0700);     /* idempotent; EEXIST is fine */
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

/* Atomic, fsync'd write of `len` bytes to <dir>/<fname>. Returns 0 on
 * success, -1 on any failure (or when persistence is disabled). */
int pfs_dur_write(const char *fname, const void *data, unsigned len)
{
    const char *dir = pfs_dur_dir();
    if (!dir) return -1;

    if (dur_force_fail_once) { dur_force_fail_once = 0; return -1; }

    char path[PFS_DUR_PATH_MAX], tmp[PFS_DUR_PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", dir, fname) >= (int)sizeof path)
        return -1;
    if (snprintf(tmp, sizeof tmp, "%s/%s.tmp", dir, fname) >= (int)sizeof tmp)
        return -1;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;

    const unsigned char *p = (const unsigned char *)data;
    unsigned off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, (size_t)(len - off));
        if (w <= 0) { close(fd); unlink(tmp); return -1; }
        off += (unsigned)w;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return -1; }
    close(fd);

    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }

    /* fsync the directory so the rename (the durability point) survives a
     * power loss, not just the file's own data blocks. */
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { (void)fsync(dfd); close(dfd); }
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
