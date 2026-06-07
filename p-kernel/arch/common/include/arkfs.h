/*
 *  arkfs.h — ARK: the filesystem that survives the flood.
 *
 *  Spec: docs/architecture/survival-fs.md
 *
 *  ARK is p-kernel's answer to "FAT32 has no place in a network that must
 *  not perish" (survival-network.md G24: the library is volatile memory).
 *  It is the LOCAL, on-device durable store — the single-node counterpart
 *  to p-fs (the distributed, gossip-replicated swarm memory). Both speak
 *  the SAME content address: a block-id is sha256(bytes), a fixed 32-byte
 *  value, byte-identical across aarch64 / x86_64 / i686 / Android. That
 *  shared id space is what lets ARK become p-fs's durable backend without
 *  any translation (survival-fs.md §7).
 *
 *  Three properties, one design:
 *    1. Content-addressed   block-id = sha256(block). Dedup + self-verify
 *                           come for free; reading re-hashes and detects rot.
 *    2. Log-structured / append-only. Existing bytes are NEVER overwritten.
 *                           A torn write (power loss mid-write) can only
 *                           damage the tail, never committed data.
 *    3. Atomic commit / versioned. A COMMIT record (a whole-FS checkpoint,
 *                           CRC + sha256 protected) is the single atomic
 *                           "the new version is now live" point. Old commits
 *                           stay in the log forever (the library that does
 *                           not perish). Crash recovery = replay the log to
 *                           the last fully-valid commit; the rest rolls back.
 *
 *  LP64 / ABI trap (memory: feedback_lp64_typedef_trap): every on-disk
 *  field is U1/U2/U4 (fixed width), NEVER a long-derived UW/W. The block-id
 *  is U1[ARK_ID_LEN]. _Static_assert in arkfs.c pins every struct size so
 *  an image written on one ABI mounts on another.
 *
 *  No <string.h> here (arch/common rule): tiny local mem loops in arkfs.c.
 */

#ifndef PKERNEL_ARKFS_H
#define PKERNEL_ARKFS_H

#ifdef ARK_HOST_TEST
/* Standalone host harness build (samples/25_survival_fs). Define the
 * fixed-width aliases EXACTLY as include/typedef.h does (plain C types,
 * no <stdint.h>) so the on-disk layout is bit-identical to the kernel
 * build, while keeping this header mixable with POSIX system headers. */
typedef unsigned char  U1;
typedef unsigned short U2;
typedef unsigned int   U4;
typedef unsigned int   UW;
typedef int            INT;
typedef unsigned int   BOOL;
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#else
#include "kernel.h"
#endif

/* ---- geometry / limits (prototype-bounded; see survival-fs.md §8) ---- */
#define ARK_SECTOR        512        /* on-disk sector / I/O quantum      */
#define ARK_ID_LEN        32         /* sha256 digest = block-id width    */
#define ARK_BLOCK_MAX     4096       /* max bytes per block (== PFS_BLOCK_MAX) */
#define ARK_NAME_MAX      64         /* full path length incl. NUL        */
#define ARK_MAX_FILES     32         /* entries in one committed snapshot  */
#define ARK_MAX_BLK       16         /* blocks per file -> 64 KiB max file */

/* In-memory block index (block-id -> log location).
 *
 * This is an open-addressed hash table backed by a static pool (arch/common is
 * freestanding — no malloc). It used to be a flat 256-entry array that capped
 * the WHOLE store at 256 blocks (~1 MiB) regardless of media size — the audit's
 * 🔴1. Now:
 *   - ARK_IDX_SLOTS is the static pool (a RAM budget; power of two for fast
 *     masking). It is the ONLY residual ceiling.
 *   - The EFFECTIVE capacity is sized from the device at mount:
 *         cap = min(ARK_IDX_SLOTS * load / 100, ARK_BDEV.total_sectors)
 *     so a small image is limited by its MEDIA (the store fills the disk, then
 *     emit_record returns ARK_E_FULL == ENOSPC), and only an image larger than
 *     the pool can hit the pool ceiling. #blocks <= #records <= total_sectors,
 *     hence total_sectors is a sound media-derived upper bound on the index.
 *
 * Tune ARK_IDX_SLOTS to trade BSS for capacity. 16384 slots * 40 B/slot ~=
 * 640 KiB and fully indexes any image up to ARK_IDX_LOAD% of the pool, e.g. a
 * 12288-block store (~12-48 MiB of content depending on block size). ARK-2/3
 * can later back the index on a real allocator to remove the pool ceiling. */
#define ARK_IDX_SLOTS     16384u     /* index hash slots (must be a power of 2) */
#define ARK_IDX_LOAD      75u        /* max load factor (%) — keep probes short */

/* Result codes (0 / >=0 = success, negative = error). */
#define ARK_OK            0
#define ARK_E_INVAL      (-1)
#define ARK_E_NODEV      (-2)
#define ARK_E_IO         (-3)
#define ARK_E_NOTFOUND   (-4)
#define ARK_E_TOOBIG     (-5)
#define ARK_E_FULL       (-6)
#define ARK_E_CORRUPT    (-7)        /* self-verify failed (block rot)    */
#define ARK_E_NOTMOUNTED (-8)

/* ------------------------------------------------------------------ */
/* ARK_BDEV — the block-device vtable ARK runs on.                      */
/*                                                                      */
/* This is the ONLY way arkfs.c touches storage: it contains NO host    */
/* libc and NO arch-specific I/O. The SAME arkfs.c logic runs on ANY    */
/* vtable, so ARK is media-agnostic. Implementations live OUTSIDE       */
/* arch/common:                                                         */
/*   - host file image  : arch/linux/pfs_ark.c (fd-backed; pread/       */
/*     pwrite/fsync) — used by the Linux ports and the samples.         */
/*   - bare metal (ARK-3): a thin blk_ssy / ide0 adapter — implement    */
/*     these four ops over the platform block driver and ARK mounts on  */
/*     real hardware with ZERO changes to arkfs.c.                      */
/*                                                                      */
/* Contract for an implementer (ARK-3):                                 */
/*   sector_size   MUST equal ARK_SECTOR (512); ark_mount/format reject */
/*                 anything else with ARK_E_INVAL.                      */
/*   total_sectors device size in 512-byte sectors. Sector 0 is the     */
/*                 superblock; sectors 1..total_sectors-1 are the log.   */
/*                 This is the media size the index now scales to. NOTE: */
/*                 U4 sectors => addressable media is capped at 2 TiB    */
/*                 (matches the on-disk U4 total_sectors in the super-   */
/*                 block); a wider address space would be a format bump. */
/*   read (ctx,lba,n,buf)  read n sectors at `lba` into buf. Return 0 on */
/*                 success, <0 on error. Must read the full n sectors    */
/*                 (handle partial/short transfers internally).         */
/*   write(ctx,lba,n,buf)  write n sectors. Same return convention.     */
/*   sync (ctx)   flush volatile caches so prior writes are durable —   */
/*                 the barrier behind ARK's commit/checkpoint atomicity. */
/*                 MAY be NULL (no-op) on media with no write-back cache;*/
/*                 then crash-durability degrades to the medium's own.   */
/*   ctx          opaque handle passed back to every op (fd, device id). */
/*                                                                      */
/* Deliberately NOT arch/x86's BLK_OPS, which lives outside common.     */
/* ------------------------------------------------------------------ */
typedef struct {
    U4    sector_size;                                   /* must be ARK_SECTOR */
    U4    total_sectors;                                 /* media size, sectors */
    INT (*read )(void *ctx, U4 lba, U4 n, void *buf);    /* 0 ok, <0 err */
    INT (*write)(void *ctx, U4 lba, U4 n, const void *buf);
    INT (*sync )(void *ctx);                             /* flush; may be NULL */
    void *ctx;                                           /* opaque device handle */
} ARK_BDEV;

/* Directory entry returned by ark_readdir (mirrors VFS_DIRENT shape). */
typedef struct {
    char name[ARK_NAME_MAX];
    U4   size;
    INT  is_dir;
    U4   version;
} ARK_DIRENT;

/* One row of a file's version history (ark_history). */
typedef struct {
    U4 version;
    U4 commit_seq;        /* the commit that minted this version */
    U4 size;
} ARK_HIST;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Lay down a fresh, empty ARK image on bd (writes the superblock and an
 * initial empty commit). Destroys any existing content. */
INT  ark_format(ARK_BDEV *bd);

/* Mount bd: validate the superblock, replay the log, and load the live
 * state from the last fully-valid commit. Anything after that commit
 * (a crash-torn tail) is discarded. Returns ARK_OK or negative. */
INT  ark_mount(ARK_BDEV *bd);

/* Forget the current mount (in-memory state only; the device is durable). */
void ark_unmount(void);

/* ------------------------------------------------------------------ */
/* Whole-file operations. Each write_file/mkdir/unlink is ONE atomic     */
/* commit: it either becomes the new version in full or not at all.       */
/* ------------------------------------------------------------------ */

/* Create or update a file. Content is chunked into content-addressed
 * blocks (dedup'd against the index), then a new commit makes it live.
 * A pre-existing path gets version+1; its old version survives in the log. */
INT  ark_write_file(const char *path, const void *buf, U4 len);

/* Read the current version of path into buf (up to max bytes). Returns the
 * file's full length, or negative. Each block is re-hashed on read; a
 * mismatch returns ARK_E_CORRUPT (self-verification). */
INT  ark_read_file(const char *path, void *buf, U4 max);

INT  ark_stat(const char *path, U4 *size, INT *is_dir, U4 *version);
INT  ark_mkdir(const char *path);
INT  ark_unlink(const char *path);     /* removes from live state (history kept) */
INT  ark_readdir(const char *path, ARK_DIRENT *out, INT max);

/* ------------------------------------------------------------------ */
/* Versioning — the library that does not perish                       */
/* ------------------------------------------------------------------ */

/* Current version number of path (1-based), or negative. */
INT  ark_version(const char *path);

/* List every historical version of path (oldest..newest) by scanning the
 * commit log. Returns the count written to out, or negative. */
INT  ark_history(const char *path, ARK_HIST *out, INT max);

/* Read a specific historical version's bytes (still in the log, immutable,
 * content-addressed). Returns length or negative. Proves old versions
 * survive even after the file was overwritten. */
INT  ark_read_version(const char *path, U4 version, void *buf, U4 max);

/* ------------------------------------------------------------------ */
/* p-fs-compatible raw block API (same 32-byte sha256 id space).        */
/* This is the seam by which ARK becomes p-fs's durable backend:         */
/* pfs_set_put_hook(ark_block_put-shim) persists; ark_block_get backs    */
/* pfs_get on a local miss. See survival-fs.md §7.                       */
/* ------------------------------------------------------------------ */
INT  ark_block_put(const void *buf, U4 len, U1 id_out[ARK_ID_LEN]);
INT  ark_block_get(const U1 id[ARK_ID_LEN], void *buf, U4 max);
INT  ark_block_has(const U1 id[ARK_ID_LEN]);
U4   ark_block_count(void);

/* Force a durable, fsync'd checkpoint: append a COMMIT that promotes every
 * block appended since the last commit into the permanent index. ark_block_put
 * alone only APPENDS a block record (uncommitted); without a following commit
 * a remount rolls it back (correct crash semantics). The p-fs durable backend
 * (arch/linux/pfs_ark.c) calls this after each raw block put so the block
 * survives a remount even though no file references it yet. A torn checkpoint
 * fails crc on replay and rolls the pending blocks back. Returns ARK_OK or
 * negative. See survival-fs.md §7. */
INT  ark_checkpoint(void);

/* GC / compaction: rewrite a fresh log keeping ONLY the latest version of each
 * LIVE block (the current commit's working set), reclaiming all superseded
 * versions and dead blocks so the append-only log stops growing without bound
 * (audit 🔴2 — the ENOSPC fuse). Crash-safe: the compacted log is built in free
 * space under a NEW epoch and made live by a single superblock switch, so a
 * power loss yields either the complete OLD library or the complete COMPACTED
 * one, never a mix. NOTE: historical versions (ark_read_version/ark_history) do
 * NOT survive a compaction — that is the deliberate cost of not dying by ENOSPC
 * (policy documented in arkfs.c). Returns ARK_OK (state reloaded), ARK_E_FULL
 * if the live set fits in no free region, or a negative error (old log intact). */
INT  ark_compact(void);

/* Self-test: format on a RAM bdev, then prove CRUD + versioning +
 * dedup + self-verify + crash-rollback (simulated power loss). Prints via
 * emit. Returns 0 on PASS, non-zero (= failure count) on FAIL. */
INT  ark_self_test(void (*emit)(const char *));

#endif /* PKERNEL_ARKFS_H */
