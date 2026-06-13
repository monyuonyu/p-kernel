/*
 *  pfs_ark.c — ARK as p-fs's durable backend (hosted / Linux userspace).
 *
 *  The white-pearl integration (wave 13). Two wave-12 pieces existed but
 *  were not connected:
 *    - the p-fs P0 block store (arch/common/pfs_block.c): content-addressed,
 *      in-memory, with a flat-file durable backend (pfs_durable.c).
 *    - ARK (arch/common/arkfs.c): an Append-only / Replayable / Keep-
 *      everything log-structured filesystem, content-addressed in the SAME
 *      32-byte sha256 id space, atomic-commit + crc/sha self-verifying.
 *
 *  This file makes ARK *be* p-fs's durable store, so the local filesystem
 *  and the distributed content store are ONE thing (survival-fs.md §7):
 *
 *    put : a NEW p-fs block -> ark_block_put() + ark_checkpoint() (fsync'd).
 *          The checkpoint is what makes the bare block survive a remount.
 *    get : a P0 (in-memory) MISS falls through to ark_block_get(), whose
 *          bytes pfs_block.c re-hashes against the requested id before
 *          serving (a second, p-fs-level self-verify on top of ARK's own
 *          crc+sha check).
 *
 *  SELECTABLE BACKEND (so 23_durable keeps passing AND ARK is exercised):
 *      PKERNEL_PFS_BACKEND=ark   -> this ARK backend (image = $PKERNEL_ARK_IMG)
 *      (unset / anything else)   -> the flat-file backend (pfs_durable.c),
 *                                   driven by $PKERNEL_PFS_DIR as before.
 *  pfs_block.c routes puts/gets/restore to exactly one of the two; they are
 *  mutually exclusive at every seam.
 *
 *  Integration shape mirrors pfs_durable.c: a HOSTED-only TU (arch/linux/,
 *  shared by the aarch64- and x86_64-linux ports), reached from pfs_block.c
 *  via plain externs (never a header — keeps the arch/linux contract out of
 *  the bare-metal arch/common include chain). Bare-metal targets never build
 *  this file; their pfs_block.c ARK calls compile out under !_TK_HOSTED_LIBC_,
 *  so the store stays memory-only there and still links.
 *
 *  arch/linux discipline (see net_relay.c / pfs_durable.c): this TU is built
 *  with the hosted CFLAGS and must mix arkfs.h with POSIX system headers, so
 *  it defines ARK_HOST_TEST — that makes arkfs.h use the plain-C fixed-width
 *  typedefs (U1/U4/INT...) declared EXACTLY as include/typedef.h does, instead
 *  of pulling kernel.h (whose stddef/stdint shadows clash with <stdio.h> et
 *  al.). The on-disk layout is bit-identical to the kernel build of arkfs.c,
 *  and U4/INT/void* are ABI-identical across the two TUs, so the calls into
 *  arkfs.o (built without ARK_HOST_TEST) link and run correctly.
 */

#define ARK_HOST_TEST    /* plain-C fixed-width types; mixable with POSIX */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "arkfs.h"

#define ARK_DEFAULT_SECTORS 8192u    /* 4 MiB image when first created */

static int      ark_fd    = -1;
static int      ark_state = -1;      /* -1 unknown, 0 inactive, 1 mounted/ready */
static int      ark_cfg   = -1;      /* -1 unknown, 0 no, 1 backend == "ark" */
static U4       ark_total = 0;
static ARK_BDEV ark_bd;

/* ------------------------------------------------------------------ */
/* file-backed block device (sector quantum = ARK_SECTOR = 512)         */
/* read/write are partial-IO safe; sync = fsync (the durability point).  */
/* ------------------------------------------------------------------ */

static INT ark_dev_read(void *ctx, U4 lba, U4 n, void *buf)
{
    (void)ctx;
    off_t  off = (off_t)lba * ARK_SECTOR;
    size_t len = (size_t)n * ARK_SECTOR, done = 0;
    unsigned char *p = (unsigned char *)buf;
    while (done < len) {
        ssize_t r = pread(ark_fd, p + done, len - done, off + (off_t)done);
        if (r < 0) return ARK_E_IO;
        if (r == 0) { memset(p + done, 0, len - done); break; }  /* sparse tail */
        done += (size_t)r;
    }
    return ARK_OK;
}

static INT ark_dev_write(void *ctx, U4 lba, U4 n, const void *buf)
{
    (void)ctx;
    off_t  off = (off_t)lba * ARK_SECTOR;
    size_t len = (size_t)n * ARK_SECTOR, done = 0;
    const unsigned char *p = (const unsigned char *)buf;
    while (done < len) {
        ssize_t w = pwrite(ark_fd, p + done, len - done, off + (off_t)done);
        if (w <= 0) return ARK_E_IO;
        done += (size_t)w;
    }
    return ARK_OK;
}

static INT ark_dev_sync(void *ctx)
{
    (void)ctx;
    return (fsync(ark_fd) == 0) ? ARK_OK : ARK_E_IO;
}

/* ------------------------------------------------------------------ */
/* backend selection + lifecycle                                       */
/* ------------------------------------------------------------------ */

/* 1 if PKERNEL_PFS_BACKEND selects ARK. Decided once. pfs_block.c uses this
 * (not pfs_ark_active) to ROUTE restore to ARK, before the mount happens. */
int pfs_ark_configured(void)
{
    if (ark_cfg < 0) {
        const char *be = getenv("PKERNEL_PFS_BACKEND");
        ark_cfg = (be && strcmp(be, "ark") == 0) ? 1 : 0;
    }
    return ark_cfg;
}

/* 1 only once the ARK image is mounted and ready to serve puts/gets. */
int pfs_ark_active(void)
{
    return ark_state == 1;
}

/* Mount (or first-time format) the ARK image named by $PKERNEL_ARK_IMG and
 * arm the backend. Called from pfs_block.c::pfs_durable_restore at boot when
 * the ARK backend is selected. Blocks are served LAZILY through pfs_get's
 * fall-through (P0 stays a cache), so nothing is eagerly loaded here. Prints
 * a one-line summary via emit. Returns the block count, or 0 when disabled. */
int pfs_ark_restore(void (*emit)(const char *))
{
    if (!pfs_ark_configured()) return 0;

    const char *path = getenv("PKERNEL_ARK_IMG");
    if (!path || !path[0]) {
        if (emit)
            emit("[pfs] durable(ark): PKERNEL_ARK_IMG unset — ARK backend "
                 "disabled (memory-only)\r\n");
        ark_state = 0;
        return 0;
    }

    struct stat st;
    int existed = (stat(path, &st) == 0 && st.st_size >= (off_t)(4 * ARK_SECTOR));

    ark_fd = open(path, O_RDWR | O_CREAT, 0600);
    if (ark_fd < 0) {
        if (emit) emit("[pfs] durable(ark): cannot open image — disabled\r\n");
        ark_state = 0;
        return 0;
    }

    if (existed) {
        ark_total = (U4)(st.st_size / ARK_SECTOR);
    } else {
        const char *se = getenv("PKERNEL_ARK_SECTORS");
        U4 n = ARK_DEFAULT_SECTORS;
        if (se && se[0]) { U4 v = (U4)strtoul(se, 0, 10); if (v >= 4) n = v; }
        ark_total = n;
        if (ftruncate(ark_fd, (off_t)n * ARK_SECTOR) != 0) {
            if (emit) emit("[pfs] durable(ark): cannot size image — disabled\r\n");
            close(ark_fd); ark_fd = -1; ark_state = 0;
            return 0;
        }
    }

    ark_bd.sector_size   = ARK_SECTOR;
    ark_bd.total_sectors = ark_total;
    ark_bd.read  = ark_dev_read;
    ark_bd.write = ark_dev_write;
    ark_bd.sync  = ark_dev_sync;
    ark_bd.ctx   = 0;

    INT r;
    if (existed) {
        r = ark_mount(&ark_bd);
        if (r != ARK_OK) {
            if (emit)
                emit("[pfs] durable(ark): image present but superblock "
                     "invalid — NOT mounted\r\n");
            close(ark_fd); ark_fd = -1; ark_state = 0;
            return 0;
        }
    } else {
        r = ark_format(&ark_bd);
        if (r == ARK_OK) r = ark_mount(&ark_bd);
        if (r != ARK_OK) {
            if (emit) emit("[pfs] durable(ark): format failed — disabled\r\n");
            close(ark_fd); ark_fd = -1; ark_state = 0;
            return 0;
        }
    }

    ark_state = 1;
    U4 nb = ark_block_count();
    if (emit) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "[pfs] durable(ark): %s image '%s' — %u block(s) in log "
                 "(crash-safe, sha256/crc self-verified)\r\n",
                 existed ? "mounted" : "formatted", path, (unsigned)nb);
        emit(msg);
    }
    return (int)nb;
}

/* ------------------------------------------------------------------ */
/* the put / get seam (called inline by pfs_block.c)                    */
/* ------------------------------------------------------------------ */

/* DUR-SWALLOW cert hook (ARK backend): when set non-zero the NEXT pfs_ark_put
 * FAILS (returns -1) WITHOUT mutating the log, then auto-clears. Mirrors
 * pfs_dur_force_fail so the cert exercises the eviction-skip on the one
 * backend that actually evicts (ARK has the pfs_get fall-through). */
static int ark_force_fail_once;
void pfs_ark_force_fail(int on) { ark_force_fail_once = on ? 1 : 0; }

/* Persist a NEW p-fs block into the ARK log: append + durable checkpoint.
 * ark_block_put dedups against the on-disk index for free. Returns 0 / -1. */
int pfs_ark_put(const void *data, unsigned len)
{
    if (ark_state != 1) return -1;
    if (ark_force_fail_once) { ark_force_fail_once = 0; return -1; }
    U1 id[ARK_ID_LEN];
    if (ark_block_put(data, (U4)len, id) != ARK_OK) return -1;
    /* Promote the appended block into the permanent index with a durable,
     * fsync'd commit so it survives a remount even though no file references
     * it yet. A torn checkpoint fails crc on replay and rolls the block back
     * (crash-safety preserved through the p-fs path). */
    if (ark_checkpoint() != ARK_OK) return -1;
    return 0;
}

/* Serve a p-fs get that missed the in-memory P0 store from the ARK log.
 * Returns the block length (>= 0) or -1 on miss/corruption. ARK self-verifies
 * (crc + sha) internally; pfs_block.c re-hashes the bytes too before serving. */
int pfs_ark_get(const unsigned char *id, void *buf, unsigned maxlen)
{
    if (ark_state != 1) return -1;
    INT r = ark_block_get(id, buf, (U4)maxlen);
    if (r < 0) return -1;            /* ARK_E_NOTFOUND or ARK_E_CORRUPT */
    return (int)r;
}
