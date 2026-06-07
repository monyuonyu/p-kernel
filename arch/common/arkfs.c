/*
 *  arkfs.c — ARK: the log-structured, content-addressed, crash-safe
 *            local filesystem. The vessel that carries the library's
 *            memory through the power-loss flood.
 *
 *  Spec & rationale: docs/architecture/survival-fs.md
 *  Header contract:  arch/common/include/arkfs.h
 *
 *  On-disk shape (all sector-aligned; ARK_SECTOR = 512):
 *
 *    sector 0      : superblock  (magic, geometry, crc32)         immutable
 *    sector 1..    : the LOG  — an append-only stream of records:
 *                       REC_BLOCK   one content-addressed data block
 *                       REC_COMMIT  a whole-FS checkpoint (the live state)
 *
 *  Each record = 1 header sector + ceil(len/512) payload sectors. The header
 *  carries magic, type, seq, len, id=sha256(payload), payload_crc, hdr_crc.
 *
 *  Crash consistency (survival-fs.md §5):
 *    - Bytes are never overwritten; writes only APPEND past the log head.
 *    - A new file version becomes live ONLY when its REC_COMMIT lands with a
 *      valid hdr_crc AND payload_crc. A torn (half-written) commit fails crc
 *      and is rejected on replay -> the previous commit stands (rollback).
 *    - Mount replays the log, accepting records up to the last fully-valid
 *      commit; the crash-torn tail is discarded and reclaimed by the next
 *      append. => power loss can corrupt at most the in-flight write.
 *
 *  No <string.h> (arch/common rule): tiny local mem loops below.
 *  No glibc stdio / no large task-stack locals: all scratch is static.
 */

#include "arkfs.h"
#include "sha256.h"          /* relay/sha256.c — zero-dep, ABI-stable id */

/* ------------------------------------------------------------------ */
/* on-disk structures (packed, fixed-width, LP64-stable)               */
/* ------------------------------------------------------------------ */

#define ARK_SB_MAGIC0 'A'
/* Format v2 (wave 15, ARK-2 longevity): the superblock is now REPLICATED to a
 * second sector (the last sector of the media) so one rotted byte on sector 0
 * no longer makes the whole library unmountable. The last sector is therefore
 * reserved from the log. This is an on-disk format change, hence the magic +
 * version bump: a v1 image ("ARKLOG01") will not mount under v2 (clean reject,
 * not a mis-mount). The fuzzer reformats every run, so it stays valid. */
#define ARK_SB_MAGIC  "ARKLOG02"     /* 8 bytes */
#define ARK_REC_MAGIC "ARKR"         /* 4 bytes */
#define ARK_FMT_VERSION 2u

#define ARK_REC_BLOCK  1u
#define ARK_REC_COMMIT 2u

typedef struct __attribute__((packed)) {
    U1 magic[8];        /* "ARKLOG01" */
    U4 version;         /* format version */
    U4 sector_size;     /* ARK_SECTOR */
    U4 log_start;       /* first log sector (== 1) */
    U4 total_sectors;
    U4 epoch;           /* format generation — stamps every record this epoch */
    U4 crc;             /* crc32 over the preceding bytes */
} ark_super;

typedef struct __attribute__((packed)) {
    U1 magic[4];        /* "ARKR" */
    U4 type;            /* ARK_REC_BLOCK | ARK_REC_COMMIT */
    U4 seq;             /* monotonic record sequence within the epoch */
    U4 epoch;           /* must match the superblock's epoch to be valid */
    U4 len;             /* payload length in bytes */
    U1 id[ARK_ID_LEN];  /* sha256(payload) */
    U4 payload_crc;     /* crc32(payload) */
    U4 hdr_crc;         /* crc32(header bytes before this field) */
} ark_rec_hdr;

typedef struct __attribute__((packed)) {
    U4 commit_seq;
    U4 nent;
} ark_commit_hdr;

/* One directory entry, used both on disk (in a commit payload) and as the
 * in-memory live row. */
typedef struct __attribute__((packed)) {
    char name[ARK_NAME_MAX];
    U1   is_dir;
    U1   pad[3];
    U4   version;
    U4   size;
    U4   nblk;
    U1   blk[ARK_MAX_BLK][ARK_ID_LEN];
} ark_dent;

/* Commit payload upper bound, rounded up to a whole sector. */
#define ARK_COMMIT_MAX_RAW (sizeof(ark_commit_hdr) + ARK_MAX_FILES * sizeof(ark_dent))
#define ARK_SECT_UP(n)     (((n) + (ARK_SECTOR - 1)) / ARK_SECTOR * ARK_SECTOR)
#define ARK_COMMIT_MAX     ARK_SECT_UP(ARK_COMMIT_MAX_RAW)

/* ------------------------------------------------------------------ */
/* ABI guards — an image written on one arch must mount on another      */
/* ------------------------------------------------------------------ */
_Static_assert(sizeof(U1) == 1 && sizeof(U2) == 2 && sizeof(U4) == 4,
               "fixed-width on-disk types must be 1/2/4 bytes");
_Static_assert(ARK_ID_LEN == SHA256_DIGEST_SIZE,
               "block-id must equal sha256 digest size");
_Static_assert(sizeof(ark_super)  == 32, "superblock layout drift");
_Static_assert(sizeof(ark_rec_hdr) == 60, "record header layout drift");
_Static_assert(sizeof(ark_dent)   == ARK_NAME_MAX + 16 + ARK_MAX_BLK * ARK_ID_LEN,
               "dirent layout drift");
_Static_assert(ARK_COMMIT_MAX <= 32768, "commit payload exceeds scratch");
_Static_assert(ARK_BLOCK_MAX % ARK_SECTOR == 0, "block max must be sector-multiple");

/* ------------------------------------------------------------------ */
/* tiny local libc-free helpers                                        */
/* ------------------------------------------------------------------ */

static void ark_memcpy(void *d, const void *s, U4 n)
{ U1 *dd = (U1 *)d; const U1 *ss = (const U1 *)s; while (n--) *dd++ = *ss++; }

static void ark_memset(void *d, U1 v, U4 n)
{ U1 *dd = (U1 *)d; while (n--) *dd++ = v; }

static INT ark_memeq(const void *a, const void *b, U4 n)
{
    const U1 *p = (const U1 *)a, *q = (const U1 *)b;
    for (U4 i = 0; i < n; i++) { if (p[i] != q[i]) return 0; }
    return 1;
}

static U4 ark_strlen(const char *s) { U4 n = 0; while (s[n]) n++; return n; }

static INT ark_streq(const char *a, const char *b)
{ while (*a && *b && *a == *b) { a++; b++; } return *a == '\0' && *b == '\0'; }

static void ark_strncpy(char *d, const char *s, U4 max)
{
    U4 i;
    for (i = 0; i + 1 < max && s[i]; i++) d[i] = s[i];
    d[i] = '\0';
}

static U4 ark_crc32(const void *data, U4 len)
{
    const U1 *p = (const U1 *)data;
    U4 crc = 0xFFFFFFFFu;
    for (U4 i = 0; i < len; i++) {
        crc ^= p[i];
        for (INT k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88420u & (~(crc & 1u) + 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* mounted state (single mount — prototype, like fat32)                */
/* ------------------------------------------------------------------ */

static ARK_BDEV *g_bd;
static U4        g_log_start;
static U4        g_head;          /* next free log sector (append point) */
static U4        g_seq;           /* last record seq                     */
static U4        g_epoch;         /* this mount's format generation       */

static ark_dent  g_live[ARK_MAX_FILES];   /* live directory snapshot     */
static U4        g_live_n;

/* Open-addressed hash table: block-id -> log location. An entry is EMPTY iff
 * sec == 0 (sector 0 is the superblock, never a record location, so it is a
 * safe sentinel). No tombstones are needed: entries are only ever inserted at
 * runtime, and a remount rebuilds the whole table from scratch (reset_state +
 * replay), so there is no individual deletion to break a probe chain. */
typedef struct { U1 id[ARK_ID_LEN]; U4 sec; } ark_idx;   /* 40 bytes, 4-aligned */
static ark_idx   g_idx[ARK_IDX_SLOTS];    /* block-id -> log location     */
static U4        g_idx_n;                  /* live entry count             */
static U4        g_idx_cap;                /* media-derived insert limit   */

/* static scratch — never on a task stack (arch/common rule) */
static U1 g_secbuf[ARK_SECTOR];
static U1 g_blkbuf[ARK_BLOCK_MAX];
static U1 g_payld [ARK_COMMIT_MAX];       /* generic payload buffer       */
static U1 g_cbuf  [ARK_COMMIT_MAX];       /* commit serialization buffer  */

/* Upper bound on the append point (exclusive). 0 == use ark_usable_end().
 * Set transiently by ark_compact() so a front-region rewrite cannot overwrite
 * the still-live old log it is reading from. */
static U4 g_log_end_override;

/* De-dup scratch for ark_compact(): the set of unique live block-ids. One row
 * per possible live block reference (ARK_MAX_FILES * ARK_MAX_BLK). Static BSS,
 * never a task-stack local. */
static U1 g_compact_seen[ARK_MAX_FILES * ARK_MAX_BLK][ARK_ID_LEN];

/* ------------------------------------------------------------------ */
/* block index helpers                                                 */
/* ------------------------------------------------------------------ */

/* id is sha256 (uniformly distributed), so its low 32 bits are a good hash. */
static U4 idx_hash(const U1 id[ARK_ID_LEN])
{
    return (U4)id[0] | ((U4)id[1] << 8) | ((U4)id[2] << 16) | ((U4)id[3] << 24);
}

static INT idx_find(const U1 id[ARK_ID_LEN])
{
    const U4 mask = ARK_IDX_SLOTS - 1u;            /* SLOTS is a power of two */
    U4 h = idx_hash(id) & mask;
    for (U4 i = 0; i < ARK_IDX_SLOTS; i++) {
        U4 s = (h + i) & mask;
        if (g_idx[s].sec == 0) return -1;          /* empty -> not present  */
        if (ark_memeq(g_idx[s].id, id, ARK_ID_LEN)) return (INT)s;
    }
    return -1;                                     /* table full, absent    */
}

static INT idx_add(const U1 id[ARK_ID_LEN], U4 sec)
{
    const U4 mask = ARK_IDX_SLOTS - 1u;
    U4 h = idx_hash(id) & mask;
    for (U4 i = 0; i < ARK_IDX_SLOTS; i++) {
        U4 s = (h + i) & mask;
        if (g_idx[s].sec == 0) {                   /* first empty -> insert  */
            if (g_idx_n >= g_idx_cap) return ARK_E_FULL;   /* media/pool cap */
            ark_memcpy(g_idx[s].id, id, ARK_ID_LEN);
            g_idx[s].sec = sec;
            g_idx_n++;
            return ARK_OK;
        }
        if (ark_memeq(g_idx[s].id, id, ARK_ID_LEN)) return ARK_OK;   /* dedup */
    }
    return ARK_E_FULL;                             /* table full            */
}

static U4 idx_count(void) { return g_idx_n; }

/* Effective index capacity for a freshly-mounted device: bounded by the static
 * pool (load-factor-limited so probe chains stay short) AND by the media (a
 * block costs >= 1 sector, so #blocks <= total_sectors). The smaller wins, so
 * a small image is media-limited and the store fills the actual disk. */
static void idx_set_cap(U4 total_sectors)
{
    /* compile-time-safe: ARK_IDX_SLOTS*ARK_IDX_LOAD <= 2^31 by construction */
    U4 pool_cap = (ARK_IDX_SLOTS / 100u) * ARK_IDX_LOAD
                + ((ARK_IDX_SLOTS % 100u) * ARK_IDX_LOAD) / 100u;
    g_idx_cap = (total_sectors < pool_cap) ? total_sectors : pool_cap;
}

/* ------------------------------------------------------------------ */
/* low-level device I/O                                                */
/* ------------------------------------------------------------------ */

static INT dev_read(U4 lba, U4 n, void *buf)
{ if (!g_bd) return ARK_E_NODEV; return g_bd->read(g_bd->ctx, lba, n, buf); }

static INT dev_write(U4 lba, U4 n, const void *buf)
{ if (!g_bd) return ARK_E_NODEV; return g_bd->write(g_bd->ctx, lba, n, buf); }

static void dev_sync(void)
{ if (g_bd && g_bd->sync) g_bd->sync(g_bd->ctx); }

/* The log lives in [log_start, usable_end). The LAST sector is reserved for
 * the superblock replica (format v2), so the log never grows into it. */
static U4 ark_usable_end(void)
{ return g_bd->total_sectors - 1u; }

/*
 * Append a record. `payload` must point at a buffer holding `len` bytes
 * (the caller's; tail is zero-padded into a sector buffer here). On success
 * fills *out_start with the header sector and advances g_head. id is the
 * caller-supplied sha256(payload). Returns ARK_OK or negative.
 */
static INT emit_record(U4 type, const void *payload, U4 len,
                       const U1 id[ARK_ID_LEN], U4 *out_start)
{
    U4 psect = (len + ARK_SECTOR - 1) / ARK_SECTOR;
    U4 need  = 1u + psect;
    U4 lim   = g_log_end_override ? g_log_end_override : ark_usable_end();
    if (g_head + need > lim) return ARK_E_FULL;

    U4 start = g_head;

    /* header sector */
    ark_memset(g_secbuf, 0, ARK_SECTOR);
    ark_rec_hdr *h = (ark_rec_hdr *)g_secbuf;
    ark_memcpy(h->magic, ARK_REC_MAGIC, 4);
    h->type  = type;
    h->seq   = g_seq + 1;
    h->epoch = g_epoch;
    h->len   = len;
    ark_memcpy(h->id, id, ARK_ID_LEN);
    h->payload_crc = ark_crc32(payload, len);
    h->hdr_crc     = ark_crc32(h, (U4)((U1 *)&h->hdr_crc - (U1 *)h));
    if (dev_write(start, 1, g_secbuf) < 0) return ARK_E_IO;

    /* payload sectors (zero-pad the last) */
    const U1 *p = (const U1 *)payload;
    for (U4 s = 0; s < psect; s++) {
        U4 off = s * ARK_SECTOR;
        U4 chunk = (len - off < ARK_SECTOR) ? (len - off) : ARK_SECTOR;
        ark_memset(g_secbuf, 0, ARK_SECTOR);
        ark_memcpy(g_secbuf, p + off, chunk);
        if (dev_write(start + 1 + s, 1, g_secbuf) < 0) return ARK_E_IO;
    }

    g_seq++;
    g_head += need;
    if (out_start) *out_start = start;
    return ARK_OK;
}

/*
 * Read the record whose header is at sector `sec` into `payout` (capacity
 * paymax). HEADER validity and PAYLOAD validity are deliberately separated:
 *
 *   return value : sectors the record occupies (1 + payload sectors) when the
 *                  HEADER is valid (magic + hdr_crc + matching epoch); 0 when
 *                  there is no valid record header here (clean end of log /
 *                  torn header / stale epoch -> the scan must STOP).
 *   *payload_ok  : 1 if the payload's crc32 AND sha256(payload)==id both check
 *                  out; 0 if the payload is torn or rotted.
 *
 * This split is what lets recovery (a) advance past a header-valid record by
 * its declared length and (b) still flag a rotted committed block as CORRUPT
 * at read time, instead of letting one bad block truncate the whole log.
 * type/seq/len/id come straight from the (valid) header even when payload_ok=0.
 */
static INT read_record(U4 sec, void *payout, U4 paymax,
                       U4 *type_out, U4 *seq_out, U4 *len_out,
                       U1 id_out[ARK_ID_LEN], INT *payload_ok)
{
    if (payload_ok) *payload_ok = 0;
    if (sec >= ark_usable_end()) return 0;          /* never read the SB replica */
    if (dev_read(sec, 1, g_secbuf) < 0) return 0;
    if (!ark_memeq(((ark_rec_hdr *)g_secbuf)->magic, ARK_REC_MAGIC, 4))
        return 0;                                             /* not a record */

    /* Copy the header OUT of g_secbuf before the payload reads below clobber
     * it (g_secbuf is the shared sector scratch). */
    ark_rec_hdr h;
    ark_memcpy(&h, g_secbuf, sizeof(h));

    U4 want = ark_crc32(&h, (U4)((U1 *)&h.hdr_crc - (U1 *)&h));
    if (want != h.hdr_crc) return 0;                           /* torn header */
    if (h.epoch != g_epoch) return 0;                          /* stale epoch */
    if (h.len > paymax) return 0;                              /* unrepresentable */

    U4 psect = (h.len + ARK_SECTOR - 1) / ARK_SECTOR;
    if (sec + 1 + psect > ark_usable_end()) return 0;

    /* header is valid -> publish its fields and the record's footprint. */
    if (type_out) *type_out = h.type;
    if (seq_out)  *seq_out  = h.seq;
    if (len_out)  *len_out  = h.len;
    if (id_out)   ark_memcpy(id_out, h.id, ARK_ID_LEN);

    /* gather payload and verify it (crc + content-address self-check). */
    INT ok = 1;
    U1 *p = (U1 *)payout;
    for (U4 s = 0; s < psect; s++) {
        if (dev_read(sec + 1 + s, 1, g_secbuf) < 0) { ok = 0; break; }
        U4 off = s * ARK_SECTOR;
        U4 chunk = (h.len - off < ARK_SECTOR) ? (h.len - off) : ARK_SECTOR;
        ark_memcpy(p + off, g_secbuf, chunk);
    }
    if (ok && ark_crc32(payout, h.len) != h.payload_crc) ok = 0;
    if (ok) {
        U1 chk[ARK_ID_LEN];
        sha256(payout, (size_t)h.len, chk);
        if (!ark_memeq(chk, h.id, ARK_ID_LEN)) ok = 0;
    }
    if (payload_ok) *payload_ok = ok;
    return (INT)(1 + psect);
}

/* ------------------------------------------------------------------ */
/* commit (snapshot) serialization                                     */
/* ------------------------------------------------------------------ */

static U4 serialize_live(U1 *out)
{
    ark_commit_hdr *ch = (ark_commit_hdr *)out;
    ch->commit_seq = g_seq + 1;
    ch->nent       = g_live_n;
    U1 *p = out + sizeof(ark_commit_hdr);
    for (U4 i = 0; i < g_live_n; i++) {
        ark_memcpy(p, &g_live[i], sizeof(ark_dent));
        p += sizeof(ark_dent);
    }
    return (U4)(p - out);
}

/* Parse a commit payload into the provided table. Returns entry count. */
static U4 parse_commit(const U1 *pay, U4 len, ark_dent *tbl, U4 max)
{
    if (len < sizeof(ark_commit_hdr)) return 0;
    const ark_commit_hdr *ch = (const ark_commit_hdr *)pay;
    U4 n = ch->nent;
    if (n > max) n = max;
    const U1 *p = pay + sizeof(ark_commit_hdr);
    U4 avail = (len - sizeof(ark_commit_hdr)) / sizeof(ark_dent);
    if (n > avail) n = avail;
    for (U4 i = 0; i < n; i++) {
        ark_memcpy(&tbl[i], p, sizeof(ark_dent));
        p += sizeof(ark_dent);
    }
    return n;
}

/* Append the current live table as a new COMMIT. Atomic visibility point.
 *
 * The commit is the directory snapshot (every live file's name -> block-ids).
 * It is written TWICE, back-to-back, as rot-insurance: a single flipped byte in
 * one commit's payload only invalidates THAT copy, and mount / the read-path
 * fallback recover the name->block mapping from the surviving replica. Without
 * this, one rotted commit-payload byte erases a whole version's directory entry
 * even though its content blocks are intact on disk (the fuzzer's multi-fault
 * "no prior version recoverable" case). The FIRST copy is the atomic visibility
 * point — a torn first copy fails crc and rolls the version back exactly as
 * before; the SECOND copy is best-effort (its ENOSPC is non-fatal, the commit
 * still stands). Both copies are byte-identical (same payload, same id/crc;
 * only the header seq differs), so whichever self-verifies is authoritative. */
static INT commit_live_ex(INT replicate)
{
    U4 len = serialize_live(g_cbuf);
    U1 id[ARK_ID_LEN];
    sha256(g_cbuf, (size_t)len, id);
    INT r = emit_record(ARK_REC_COMMIT, g_cbuf, len, id, 0);
    if (r != ARK_OK) return r;
    if (replicate)
        emit_record(ARK_REC_COMMIT, g_cbuf, len, id, 0);   /* replica; ENOSPC ok */
    dev_sync();                            /* make the commit durable */
    return ARK_OK;
}

/* Data-bearing commits are replicated (rot-insurance, see above). The initial
 * EMPTY commit ark_format writes protects no directory entries, so it is left
 * single — which also keeps the first written file's first block at the same
 * on-disk sector as format v1 (a property the samples/25 harness relies on). */
static INT commit_live(void) { return commit_live_ex(1); }

/* ------------------------------------------------------------------ */
/* live-table lookup                                                   */
/* ------------------------------------------------------------------ */

static INT live_find(const char *path)
{
    for (U4 i = 0; i < g_live_n; i++)
        if (ark_streq(g_live[i].name, path)) return (INT)i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* mount / format                                                      */
/* ------------------------------------------------------------------ */

static void reset_state(void)
{
    g_head = g_log_start;
    g_seq  = 0;
    g_live_n = 0;
    g_idx_n  = 0;
    ark_memset(g_idx, 0, sizeof(g_idx));
    ark_memset(g_live, 0, sizeof(g_live));
}

/* Serialize a superblock for (epoch, log_start) into g_secbuf and write it to
 * `sec`. Used for BOTH the primary (sector 0) and the replica (last sector).
 * The two copies are byte-identical, so a torn primary is recovered from the
 * replica on mount (format v2; survival-fs.md §5 / audit 🟡5). */
static INT write_super(U4 sec, U4 epoch, U4 log_start)
{
    ark_memset(g_secbuf, 0, ARK_SECTOR);
    ark_super *sb = (ark_super *)g_secbuf;
    ark_memcpy(sb->magic, ARK_SB_MAGIC, 8);
    sb->version       = ARK_FMT_VERSION;
    sb->sector_size   = ARK_SECTOR;
    sb->log_start     = log_start;
    sb->total_sectors = g_bd->total_sectors;
    sb->epoch         = epoch;
    sb->crc           = ark_crc32(sb, (U4)((U1 *)&sb->crc - (U1 *)sb));
    return dev_write(sec, 1, g_secbuf);
}

/* Write BOTH superblock copies (primary @0, replica @last) and fsync. */
static INT write_super_both(U4 epoch, U4 log_start)
{
    if (write_super(0, epoch, log_start) < 0) return ARK_E_IO;
    if (write_super(g_bd->total_sectors - 1u, epoch, log_start) < 0)
        return ARK_E_IO;
    dev_sync();
    return ARK_OK;
}

/* Validate the superblock currently in g_secbuf. Returns 1 if it is a sound
 * ARK v2 superblock (magic + crc + version + sector size), else 0. */
static INT super_valid(void)
{
    ark_super *sb = (ark_super *)g_secbuf;
    if (!ark_memeq(sb->magic, ARK_SB_MAGIC, 8)) return 0;
    if (ark_crc32(sb, (U4)((U1 *)&sb->crc - (U1 *)sb)) != sb->crc) return 0;
    if (sb->version != ARK_FMT_VERSION || sb->sector_size != ARK_SECTOR) return 0;
    return 1;
}

INT ark_format(ARK_BDEV *bd)
{
    if (!bd || bd->sector_size != ARK_SECTOR) return ARK_E_INVAL;
    if (bd->total_sectors < 4) return ARK_E_INVAL;
    g_bd = bd;
    g_log_start = 1;

    /* Bump the epoch past any prior format so that stale-but-valid records
     * from a previous life of this device can never be replayed as ours
     * (crash-recovery correctness; survival-fs.md §5). */
    U4 prev_epoch = 0;
    if (dev_read(0, 1, g_secbuf) == 0) {
        ark_super *old = (ark_super *)g_secbuf;
        if (ark_memeq(old->magic, ARK_SB_MAGIC, 8) &&
            ark_crc32(old, (U4)((U1 *)&old->crc - (U1 *)old)) == old->crc)
            prev_epoch = old->epoch;
    }
    g_epoch = prev_epoch + 1;
    reset_state();
    idx_set_cap(bd->total_sectors);

    /* superblock — written to BOTH the primary (sector 0) and the replica
     * (last sector) so sector-0 rot is survivable (format v2). */
    if (write_super_both(g_epoch, g_log_start) != ARK_OK) return ARK_E_IO;

    /* initial empty commit so a fresh image mounts cleanly (single — nothing
     * to protect yet, and keeps the first file's block at the v1 layout). */
    INT r = commit_live_ex(0);
    if (r != ARK_OK) return r;
    dev_sync();
    return ARK_OK;
}

INT ark_mount(ARK_BDEV *bd)
{
    if (!bd || bd->sector_size != ARK_SECTOR) return ARK_E_INVAL;
    g_bd = bd;

    /* Load the superblock: try the primary (sector 0); on rot/tear fall back
     * to the replica (last sector). Either intact copy mounts the library
     * (format v2; audit 🟡5 — sector 0 is no longer a single point of loss). */
    INT have_sb = 0;
    if (dev_read(0, 1, g_secbuf) == 0 && super_valid()) have_sb = 1;
    if (!have_sb &&
        dev_read(bd->total_sectors - 1u, 1, g_secbuf) == 0 && super_valid())
        have_sb = 1;
    if (!have_sb) return ARK_E_CORRUPT;

    ark_super *sb = (ark_super *)g_secbuf;
    g_log_start = sb->log_start;
    g_epoch     = sb->epoch;
    reset_state();
    idx_set_cap(sb->total_sectors);

    /*
     * Two-pass replay with HEADER RESYNC (no bounded "pending" staging array —
     * that was itself a 256-entry cap on a commit-less run of block records).
     *
     * A torn/rotted record HEADER must NOT truncate the whole log into a silent
     * empty library (audit/oracle BUG: one flipped header byte -> ls=0). So a
     * sector that holds no valid record header (clean zero / torn header / a
     * stale-epoch leftover) does not STOP the scan — we advance by ONE sector
     * and RESYNC on the next valid "ARKR" header (hdr_crc + epoch verified, so
     * a false resync onto payload bytes is a ~2^-32 event). Append-only layout
     * means the only thing past the last real record is the crash-torn tail and
     * zero/garbage free space, which resync simply skips to the usable end.
     *
     * PASS 1 finds the END of the accepted region: the sector just past the
     * LAST fully-valid COMMIT (head_after_commit). Only a COMMIT whose payload
     * self-verifies (crc + sha) moves the accepted frontier; everything after
     * the last valid commit is rolled back.
     *
     * PASS 2 re-scans [log_start, head_after_commit), indexing every block
     * record (committed, so rot-tolerant: a block whose header survives but
     * payload rotted stays indexed and surfaces as CORRUPT on read; a block
     * whose header rotted is skipped by resync and surfaces as NOTFOUND) and
     * applying the last valid commit's table as g_live. A rotted block inside
     * the accepted region therefore degrades to the read-path fallback to the
     * newest intact prior version (ark_read_file), never to total loss.
     */
    U4 end = ark_usable_end();
    U4 sec = g_log_start;
    U4 head_after_commit = g_log_start;
    while (sec < end) {
        U4 type, seq, len; INT payok;
        INT n = read_record(sec, g_payld, ARK_COMMIT_MAX,
                            &type, &seq, &len, 0, &payok);
        if (n <= 0) { sec += 1u; continue; }     /* resync past a bad header */
        if (type == ARK_REC_COMMIT && payok)
            head_after_commit = sec + (U4)n;
        sec += (U4)n;
    }

    sec = g_log_start;
    while (sec < head_after_commit) {
        U4 type, seq, len; U1 id[ARK_ID_LEN]; INT payok;
        INT n = read_record(sec, g_payld, ARK_COMMIT_MAX,
                            &type, &seq, &len, id, &payok);
        if (n <= 0) { sec += 1u; continue; }     /* resync past a bad header */
        if (type == ARK_REC_BLOCK) {
            idx_add(id, sec);                    /* committed; rot-tolerant   */
        } else if (type == ARK_REC_COMMIT && payok) {
            g_live_n = parse_commit(g_payld, len, g_live, ARK_MAX_FILES);
            g_seq    = seq;                      /* last accepted commit wins */
        }
        sec += (U4)n;
    }

    g_head = head_after_commit;       /* reclaim any uncommitted tail */
    return ARK_OK;
}

void ark_unmount(void) { g_bd = 0; reset_state(); }

/* ------------------------------------------------------------------ */
/* raw block API (p-fs-compatible)                                     */
/* ------------------------------------------------------------------ */

INT ark_block_put(const void *buf, U4 len, U1 id_out[ARK_ID_LEN])
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    if (len > ARK_BLOCK_MAX) return ARK_E_TOOBIG;
    if (!buf && len) return ARK_E_INVAL;

    U1 id[ARK_ID_LEN];
    sha256(buf ? buf : (const void *)"", (size_t)len, id);
    if (id_out) ark_memcpy(id_out, id, ARK_ID_LEN);

    if (idx_find(id) >= 0) return ARK_OK;          /* dedup */

    /* copy into padded scratch and append */
    ark_memset(g_blkbuf, 0, ARK_BLOCK_MAX);
    if (len) ark_memcpy(g_blkbuf, buf, len);
    U4 start;
    INT r = emit_record(ARK_REC_BLOCK, g_blkbuf, len, id, &start);
    if (r != ARK_OK) return r;
    return idx_add(id, start);
}

INT ark_block_get(const U1 id[ARK_ID_LEN], void *buf, U4 max)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    INT s = idx_find(id);
    if (s < 0) return ARK_E_NOTFOUND;

    U4 type, seq, len; INT payok;
    INT n = read_record(g_idx[s].sec, g_payld, ARK_COMMIT_MAX,
                        &type, &seq, &len, 0, &payok);
    if (n <= 0 || type != ARK_REC_BLOCK) return ARK_E_CORRUPT;
    if (!payok) return ARK_E_CORRUPT;              /* self-verify caught rot */

    U4 cpy = (len < max) ? len : max;
    if (buf && cpy) ark_memcpy(buf, g_payld, cpy);
    return (INT)len;
}

INT ark_block_has(const U1 id[ARK_ID_LEN]) { return idx_find(id) >= 0 ? 1 : 0; }
U4  ark_block_count(void) { return idx_count(); }

/* Durable checkpoint: append a COMMIT (fsync'd by commit_live) that makes
 * every block appended since the last commit permanent on the next replay.
 * Used by the p-fs durable backend so a bare ark_block_put outlives a remount.
 * Atomicity is unchanged: a torn COMMIT fails crc and rolls the tail back. */
INT ark_checkpoint(void)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    return commit_live();
}

/* ------------------------------------------------------------------ */
/* GC / compaction — bound the ever-growing log                        */
/* ------------------------------------------------------------------ */

/*
 * POLICY (survival-fs.md §8 follow-up; audit 🔴2 — "keep-everything with no
 * capacity story is a fuse"):
 *
 *   ARK is append-only, so every put/mutation appends a full snapshot and the
 *   log only grows — eventually emit_record returns ARK_E_FULL (ENOSPC) and the
 *   store is write-dead. ark_compact() reclaims that space by rewriting a FRESH
 *   log that keeps ONLY the latest version of each LIVE block (the blocks the
 *   current commit references), discarding every superseded version and dead
 *   block. After compaction the live log occupies just the live working set.
 *
 *   Retention: this is "keep the current library, drop the history." Old file
 *   *versions* (ark_read_version / ark_history) do NOT survive a compaction —
 *   they are the cost of not dying by ENOSPC. A caller that must keep history
 *   should compact rarely / never; the default kernel path does not auto-compact.
 *
 * CRASH-SAFETY (last-good-or-new-complete, never half):
 *
 *   The compacted log is written into FREE space that never overlaps the live
 *   old records — the tail [g_head, usable_end) if it fits, else the dead front
 *   [1, log_start) reclaimed by a prior compaction — and is stamped with a NEW
 *   epoch (epoch fencing makes the half-written new log invisible to the old
 *   superblock). The superblock is the single atomic switch: only after the
 *   whole compacted log is fsync'd do we rewrite the superblock (primary +
 *   replica) to point at the new epoch/log_start. A crash BEFORE that switch
 *   leaves the OLD superblock and OLD log fully intact (mount replays the old
 *   epoch; the new-epoch records are fenced out). A crash DURING the switch is
 *   covered by the superblock replica (a torn primary falls back to the old
 *   replica). So a power loss at any instant yields either the complete old
 *   library or the complete compacted one — never a mix.
 *
 * Returns ARK_OK on success (state reloaded), ARK_E_FULL if the live set does
 * not fit in any free region, or a negative error (old log left intact).
 */
INT ark_compact(void)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;

    U4 end = ark_usable_end();

    /* ---- dry pass: collect unique live block-ids + total footprint ---- */
    U4 nseen = 0;
    U4 foot  = 0;                                  /* sectors the new log needs */
    for (U4 f = 0; f < g_live_n; f++) {
        for (U4 b = 0; b < g_live[f].nblk; b++) {
            const U1 *id = g_live[f].blk[b];
            INT dup = 0;
            for (U4 s = 0; s < nseen; s++)
                if (ark_memeq(g_compact_seen[s], id, ARK_ID_LEN)) { dup = 1; break; }
            if (dup) continue;
            if (nseen >= ARK_MAX_FILES * ARK_MAX_BLK) return ARK_E_FULL;

            INT s = idx_find(id);
            if (s < 0) return ARK_E_CORRUPT;       /* live block gone -> abort */
            U4 t, sq, ln; INT pk;
            INT n = read_record(g_idx[s].sec, g_payld, ARK_COMMIT_MAX,
                                &t, &sq, &ln, 0, &pk);
            if (n <= 0 || t != ARK_REC_BLOCK || !pk)
                return ARK_E_CORRUPT;              /* can't re-emit -> abort */

            ark_memcpy(g_compact_seen[nseen++], id, ARK_ID_LEN);
            foot += 1u + (ln + ARK_SECTOR - 1u) / ARK_SECTOR;
        }
    }
    /* the trailing commit's footprint */
    {
        U4 clen = (U4)sizeof(ark_commit_hdr) + g_live_n * (U4)sizeof(ark_dent);
        foot += 1u + (clen + ARK_SECTOR - 1u) / ARK_SECTOR;
    }

    /* ---- choose a non-overlapping free region for the new log ---- */
    U4 S;
    U4 limit;
    if (g_head + foot <= end) {                    /* free tail */
        S = g_head;
        limit = end;
    } else if (1u + foot <= g_log_start) {         /* dead front (reclaimed) */
        S = 1u;
        limit = g_log_start;                       /* never touch live old log */
    } else {
        return ARK_E_FULL;                         /* genuinely no room */
    }

    /* ---- write pass: re-emit live blocks then one commit, NEW epoch ---- */
    U4 save_epoch = g_epoch, save_head = g_head, save_seq = g_seq,
       save_log_start = g_log_start;
    U4 new_epoch = save_epoch + 1u;

    g_log_end_override = limit;
    g_head = S;
    g_seq  = 0;

    INT rc = ARK_OK;
    for (U4 s = 0; s < nseen; s++) {
        g_epoch = save_epoch;                      /* read old blocks */
        INT bl = ark_block_get(g_compact_seen[s], g_blkbuf, ARK_BLOCK_MAX);
        if (bl < 0) { rc = ARK_E_CORRUPT; break; }
        g_epoch = new_epoch;                        /* write new-epoch records */
        U4 st;
        INT r = emit_record(ARK_REC_BLOCK, g_blkbuf, (U4)bl,
                            g_compact_seen[s], &st);
        if (r != ARK_OK) { rc = r; break; }
    }
    if (rc == ARK_OK) {
        g_epoch = new_epoch;
        rc = commit_live();                        /* trailing commit + fsync */
    }

    if (rc != ARK_OK) {
        /* Abort: we only wrote into free space and never the superblock, so the
         * OLD log is intact. Restore in-memory state and bail. */
        g_log_end_override = 0;
        g_epoch = save_epoch; g_head = save_head;
        g_seq = save_seq; g_log_start = save_log_start;
        return rc;
    }

    /* ---- atomic switch: rewrite the superblock(s) to the new log ---- */
    g_log_end_override = 0;
    INT sr = write_super_both(new_epoch, S);
    if (sr != ARK_OK) {
        /* SB unchanged-or-torn: the old SB still points at the intact old log
         * (and the replica recovers a torn primary). Restore and report. */
        g_epoch = save_epoch; g_head = save_head;
        g_seq = save_seq; g_log_start = save_log_start;
        return sr;
    }

    /* New superblock is durable -> reload cleanly from it (rebuilds index). */
    return ark_mount(g_bd);
}

/* ------------------------------------------------------------------ */
/* file operations                                                     */
/* ------------------------------------------------------------------ */

INT ark_write_file(const char *path, const void *buf, U4 len)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    if (!path || ark_strlen(path) + 1 > ARK_NAME_MAX) return ARK_E_INVAL;
    U4 nblk = (len + ARK_BLOCK_MAX - 1) / ARK_BLOCK_MAX;
    if (len == 0) nblk = 0;
    if (nblk > ARK_MAX_BLK) return ARK_E_TOOBIG;

    /* 1. write content blocks (dedup'd). Uncommitted until the commit below. */
    U1 ids[ARK_MAX_BLK][ARK_ID_LEN];
    for (U4 i = 0; i < nblk; i++) {
        U4 off = i * ARK_BLOCK_MAX;
        U4 chunk = (len - off < ARK_BLOCK_MAX) ? (len - off) : ARK_BLOCK_MAX;
        INT r = ark_block_put((const U1 *)buf + off, chunk, ids[i]);
        if (r != ARK_OK) return r;
    }

    /* 2. build the new live snapshot (does not touch disk yet). */
    INT e = live_find(path);
    if (e < 0) {
        if (g_live_n >= ARK_MAX_FILES) return ARK_E_FULL;
        e = (INT)g_live_n++;
        ark_memset(&g_live[e], 0, sizeof(ark_dent));
        ark_strncpy(g_live[e].name, path, ARK_NAME_MAX);
        g_live[e].version = 1;
    } else {
        g_live[e].version += 1;            /* old version survives in the log */
    }
    g_live[e].is_dir = 0;
    g_live[e].size   = len;
    g_live[e].nblk   = nblk;
    for (U4 i = 0; i < nblk; i++)
        ark_memcpy(g_live[e].blk[i], ids[i], ARK_ID_LEN);

    /* 3. ATOMIC COMMIT: the new version is live iff this record lands valid. */
    return commit_live();
}

/* Read every block of a directory entry into buf (whole-file). Returns the
 * file size on success, or the FIRST block failure (ARK_E_CORRUPT for rot,
 * ARK_E_NOTFOUND for a missing/header-rotted block). A block is only copied
 * out after ark_block_get's crc+sha self-verify passes, so this NEVER serves
 * a block that fails self-verify — it fails the whole read instead. */
static INT read_dent_blocks(const ark_dent *d, void *buf, U4 max)
{
    U4 done = 0;
    for (U4 i = 0; i < d->nblk; i++) {
        INT bl = ark_block_get(d->blk[i], g_blkbuf, ARK_BLOCK_MAX);
        if (bl < 0) return bl;                   /* CORRUPT or NOTFOUND */
        U4 cpy = (U4)bl;
        if (done + cpy > max) cpy = (done < max) ? max - done : 0;
        if (buf && cpy) ark_memcpy((U1 *)buf + done, g_blkbuf, cpy);
        done += (U4)bl;
    }
    return (INT)d->size;
}

INT ark_read_file(const char *path, void *buf, U4 max)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    INT e = live_find(path);
    if (e < 0 || g_live[e].is_dir) return ARK_E_NOTFOUND;

    /* Try the CURRENT version first. */
    INT r = read_dent_blocks(&g_live[e], buf, max);
    if (r >= 0) return r;

    /* Current version is torn/rotted (a block failed self-verify or is gone).
     * AUTO-FALLBACK to the newest INTACT prior version instead of returning
     * CORRUPT/NOTFOUND (audit 🟡6 — "always last-good"). Old versions still
     * live in the append-only log; ark_read_version verifies every block, so
     * we never serve corrupt bytes — we serve the latest version that reads
     * back clean, or withhold if none does. */
    U4 cur = g_live[e].version;
    for (U4 v = cur; v > 1u; ) {
        v--;
        INT rr = ark_read_version(path, v, buf, max);
        if (rr >= 0) return rr;                  /* newest intact prior wins */
    }
    return r;                                    /* nothing recoverable */
}

INT ark_stat(const char *path, U4 *size, INT *is_dir, U4 *version)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    INT e = live_find(path);
    if (e < 0) return ARK_E_NOTFOUND;
    if (size)    *size    = g_live[e].size;
    if (is_dir)  *is_dir  = g_live[e].is_dir;
    if (version) *version = g_live[e].version;
    return ARK_OK;
}

INT ark_mkdir(const char *path)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    if (!path || ark_strlen(path) + 1 > ARK_NAME_MAX) return ARK_E_INVAL;
    if (live_find(path) >= 0) return ARK_E_INVAL;
    if (g_live_n >= ARK_MAX_FILES) return ARK_E_FULL;
    INT e = (INT)g_live_n++;
    ark_memset(&g_live[e], 0, sizeof(ark_dent));
    ark_strncpy(g_live[e].name, path, ARK_NAME_MAX);
    g_live[e].is_dir  = 1;
    g_live[e].version = 1;
    return commit_live();
}

INT ark_unlink(const char *path)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    INT e = live_find(path);
    if (e < 0) return ARK_E_NOTFOUND;
    /* remove from live table (history of past versions stays in the log) */
    for (U4 i = (U4)e; i + 1 < g_live_n; i++) g_live[i] = g_live[i + 1];
    g_live_n--;
    return commit_live();
}

/* readdir: entries whose name is an immediate child of `path` ("/" lists
 * top level). Prototype namespace is flat path-strings (survival-fs.md §6). */
INT ark_readdir(const char *path, ARK_DIRENT *out, INT max)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    U4 plen = ark_strlen(path);
    INT root = (plen == 0) || (plen == 1 && path[0] == '/');
    INT cnt = 0;
    for (U4 i = 0; i < g_live_n && cnt < max; i++) {
        const char *nm = g_live[i].name;
        const char *child;
        if (root) {
            child = (nm[0] == '/') ? nm + 1 : nm;
        } else {
            if (!ark_memeq(nm, path, plen)) continue;
            if (nm[plen] != '/') continue;
            child = nm + plen + 1;
        }
        if (child[0] == '\0') continue;
        /* only immediate children: no further '/' in the remainder */
        INT nested = 0;
        for (const char *c = child; *c; c++) if (*c == '/') { nested = 1; break; }
        if (nested) continue;
        ark_strncpy(out[cnt].name, child, ARK_NAME_MAX);
        out[cnt].size    = g_live[i].size;
        out[cnt].is_dir  = g_live[i].is_dir;
        out[cnt].version = g_live[i].version;
        cnt++;
    }
    return cnt;
}

/* ------------------------------------------------------------------ */
/* versioning — walk the commit log                                    */
/* ------------------------------------------------------------------ */

INT ark_version(const char *path)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    INT e = live_find(path);
    if (e < 0) return ARK_E_NOTFOUND;
    return (INT)g_live[e].version;
}

/*
 * Scan every commit oldest->newest. For each commit holding `path`, record a
 * row whenever that file's version number changes (a new version was minted).
 */
INT ark_history(const char *path, ARK_HIST *out, INT max)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    static ark_dent tbl[ARK_MAX_FILES];
    U4 sec = g_log_start;
    INT cnt = 0;
    U4 last_ver = 0;

    while (sec < g_head && cnt < max) {
        U4 type, seq, len; INT payok;
        INT n = read_record(sec, g_payld, ARK_COMMIT_MAX, &type, &seq, &len, 0, &payok);
        if (n <= 0) { sec += 1u; continue; }     /* resync past a bad header */
        if (type == ARK_REC_COMMIT && payok) {
            U4 ntb = parse_commit(g_payld, len, tbl, ARK_MAX_FILES);
            for (U4 i = 0; i < ntb; i++) {
                if (ark_streq(tbl[i].name, path) && tbl[i].version != last_ver) {
                    out[cnt].version    = tbl[i].version;
                    out[cnt].commit_seq = seq;
                    out[cnt].size       = tbl[i].size;
                    last_ver = tbl[i].version;
                    cnt++;
                    break;
                }
            }
        }
        sec += (U4)n;
    }
    return cnt;
}

/* Read the bytes of a specific historical version (blocks are immutable and
 * still in the log). Returns length or negative. */
INT ark_read_version(const char *path, U4 version, void *buf, U4 max)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    static ark_dent tbl[ARK_MAX_FILES];
    U4 sec = g_log_start;

    while (sec < g_head) {
        U4 type, seq, len; INT payok;
        INT n = read_record(sec, g_payld, ARK_COMMIT_MAX, &type, &seq, &len, 0, &payok);
        if (n <= 0) { sec += 1u; continue; }     /* resync past a bad header */
        if (type == ARK_REC_COMMIT && payok) {
            U4 ntb = parse_commit(g_payld, len, tbl, ARK_MAX_FILES);
            for (U4 i = 0; i < ntb; i++) {
                if (ark_streq(tbl[i].name, path) && tbl[i].version == version)
                    return read_dent_blocks(&tbl[i], buf, max);
            }
        }
        sec += (U4)n;
    }
    return ARK_E_NOTFOUND;
}

/* ------------------------------------------------------------------ */
/* self-test (RAM-backed) — CRUD + version + dedup + verify + crash    */
/* ------------------------------------------------------------------ */

/* live log span (sectors between log_start and the append head) — used by the
 * self-test to prove compaction actually shrinks the live log. */
U4 ark_dbg_livespan(void) { return g_head - g_log_start; }

#define ARK_TEST_SECTORS 256        /* 128 KiB RAM image (self-test only) */
static U1  art_ram[ARK_TEST_SECTORS * ARK_SECTOR];
static U4  art_cut;                 /* >0 => lose writes after this many */
static U4  art_wcnt;

static INT art_read(void *ctx, U4 lba, U4 n, void *buf)
{
    (void)ctx;
    if (lba + n > ARK_TEST_SECTORS) return -1;
    ark_memcpy(buf, art_ram + (U4)lba * ARK_SECTOR, n * ARK_SECTOR);
    return 0;
}
static INT art_write(void *ctx, U4 lba, U4 n, const void *buf)
{
    (void)ctx;
    if (lba + n > ARK_TEST_SECTORS) return -1;
    art_wcnt++;
    if (art_cut && art_wcnt > art_cut) return 0;   /* simulated power loss */
    ark_memcpy(art_ram + (U4)lba * ARK_SECTOR, buf, n * ARK_SECTOR);
    return 0;
}

INT ark_self_test(void (*emit)(const char *))
{
    INT fails = 0;
    ARK_BDEV bd = { ARK_SECTOR, ARK_TEST_SECTORS, art_read, art_write, 0, 0 };

    if (sha256_self_test() != 0) { emit("[ark] FAIL sha256 KAT\r\n"); return 1; }

    art_cut = 0; art_wcnt = 0;
    if (ark_format(&bd) != ARK_OK) { emit("[ark] FAIL format\r\n"); return 1; }
    if (ark_mount(&bd)  != ARK_OK) { emit("[ark] FAIL mount\r\n");  return 1; }

    /* --- CRUD --- */
    const char *v1 = "the library that does not perish (v1)";
    const char *v2 = "the library that does not perish -- now version two!";
    static char rd[256];

    if (ark_write_file("/note.txt", v1, (U4)ark_strlen(v1)) != ARK_OK) {
        emit("[ark] FAIL write v1\r\n"); fails++;
    }
    INT rl = ark_read_file("/note.txt", rd, sizeof(rd));
    if (rl != (INT)ark_strlen(v1) || !ark_memeq(rd, v1, (U4)rl)) {
        emit("[ark] FAIL read v1\r\n"); fails++;
    } else emit("[ark] ok  create + read-back\r\n");

    /* --- versioning: write v2, old v1 must survive --- */
    if (ark_write_file("/note.txt", v2, (U4)ark_strlen(v2)) != ARK_OK) {
        emit("[ark] FAIL write v2\r\n"); fails++;
    }
    if (ark_version("/note.txt") != 2) { emit("[ark] FAIL version != 2\r\n"); fails++; }
    rl = ark_read_file("/note.txt", rd, sizeof(rd));
    if (rl != (INT)ark_strlen(v2) || !ark_memeq(rd, v2, (U4)rl)) {
        emit("[ark] FAIL read v2\r\n"); fails++;
    }
    rl = ark_read_version("/note.txt", 1, rd, sizeof(rd));
    if (rl != (INT)ark_strlen(v1) || !ark_memeq(rd, v1, (U4)rl)) {
        emit("[ark] FAIL old version 1 not recoverable\r\n"); fails++;
    } else emit("[ark] ok  old version survives new write (library)\r\n");

    ARK_HIST hist[8];
    INT hn = ark_history("/note.txt", hist, 8);
    if (hn != 2) { emit("[ark] FAIL history count != 2\r\n"); fails++; }
    else emit("[ark] ok  history records both versions\r\n");

    /* --- dedup: same content twice does not grow the store --- */
    U4 bc0 = ark_block_count();
    U1 dupid[ARK_ID_LEN];
    ark_block_put("identical bytes", 15, dupid);
    ark_block_put("identical bytes", 15, dupid);
    if (ark_block_count() != bc0 + 1) { emit("[ark] FAIL dedup\r\n"); fails++; }
    else emit("[ark] ok  content-address dedup\r\n");

    /* --- readdir / mkdir --- */
    ark_mkdir("/dir");
    ark_write_file("/dir/a", "aaa", 3);
    ARK_DIRENT de[8];
    INT dn = ark_readdir("/dir", de, 8);
    if (dn != 1 || !ark_streq(de[0].name, "a")) { emit("[ark] FAIL readdir\r\n"); fails++; }
    else emit("[ark] ok  mkdir + readdir\r\n");

    /* --- self-verify: corrupt a stored block's bytes on the device, then
     *     read it back -> the re-hash must catch the rot (fsck-free). We
     *     store a uniquely-marked block, find its bytes in the RAM image,
     *     flip one, and read by id. */
    {
        static const char marker[] = "ZZ-arkfs-rot-marker-block-ZZ";
        U1 mid[ARK_ID_LEN];
        ark_block_put(marker, (U4)(sizeof(marker) - 1), mid);
        /* locate the marker bytes in the device image and corrupt one. */
        INT found = -1;
        for (U4 off = 0; off + (sizeof(marker) - 1) < sizeof(art_ram); off++) {
            if (ark_memeq(art_ram + off, marker, (U4)(sizeof(marker) - 1))) {
                found = (INT)off; break;
            }
        }
        if (found < 0) { emit("[ark] FAIL could not locate stored block\r\n"); fails++; }
        else {
            art_ram[found + 3] ^= 0xFF;             /* flip a payload byte */
            INT got = ark_block_get(mid, rd, sizeof(rd));
            if (got != ARK_E_CORRUPT) {
                emit("[ark] FAIL corruption not detected on read\r\n"); fails++;
            } else emit("[ark] ok  self-verify detects injected rot on read\r\n");
        }
    }

    /* --- crash rollback: torn commit must roll back to previous version --- */
    art_cut = 0; art_wcnt = 0;
    ark_format(&bd);
    ark_mount(&bd);
    ark_write_file("/x", "committed-A", 11);     /* durable v1 */
    /* now arm a power-loss: lose every write from here on, then attempt v2. */
    U4 mark = art_wcnt;
    art_cut = mark;                              /* writes after `mark` vanish */
    ark_write_file("/x", "lost-B-uncommitted", 18);
    art_cut = 0;
    /* remount as if power was lost during the v2 write */
    if (ark_mount(&bd) != ARK_OK) { emit("[ark] FAIL remount after crash\r\n"); fails++; }
    rl = ark_read_file("/x", rd, sizeof(rd));
    if (rl == 11 && ark_memeq(rd, "committed-A", 11) && ark_version("/x") == 1) {
        emit("[ark] ok  crash mid-write rolled back to last commit\r\n");
    } else {
        emit("[ark] FAIL crash rollback (state not clean)\r\n"); fails++;
    }

    /* --- compaction: log shrinks, current data survives, store still works --- */
    {
        art_cut = 0; art_wcnt = 0;
        ark_format(&bd);
        ark_mount(&bd);
        /* churn one file through many versions so the log accumulates dead
         * (superseded) blocks the compactor should reclaim. */
        for (INT i = 0; i < 20; i++) {
            char s[32]; INT j = 0;
            const char *pfx = "compact-churn-v";
            while (pfx[j]) { s[j] = pfx[j]; j++; }
            s[j++] = (char)('0' + (i / 10));
            s[j++] = (char)('0' + (i % 10));
            s[j]   = '\0';
            ark_write_file("/c.txt", s, (U4)j);
        }
        ark_write_file("/keep.txt", "stays-alive", 11);
        extern U4 ark_dbg_livespan(void);
        U4 span_before = ark_dbg_livespan();
        INT cr = ark_compact();
        U4 span_after = ark_dbg_livespan();
        static char cb[64];
        INT okc = (cr == ARK_OK);
        rl = ark_read_file("/c.txt", cb, sizeof(cb));
        if (!(rl == 17 && ark_memeq(cb, "compact-churn-v19", 17))) okc = 0;
        rl = ark_read_file("/keep.txt", cb, sizeof(cb));
        if (!(rl == 11 && ark_memeq(cb, "stays-alive", 11))) okc = 0;
        /* a put must still work after compaction (log not wedged) */
        if (ark_write_file("/after.txt", "post-compact", 12) != ARK_OK) okc = 0;
        rl = ark_read_file("/after.txt", cb, sizeof(cb));
        if (!(rl == 12 && ark_memeq(cb, "post-compact", 12))) okc = 0;
        if (span_after >= span_before) okc = 0;       /* the live log shrank */
        if (okc) emit("[ark] ok  compaction reclaims log, live data survives\r\n");
        else   { emit("[ark] FAIL compaction\r\n"); fails++; }
    }

    if (fails == 0) emit("[ark] PASS (content-address + versioned + crash-safe)\r\n");
    else            emit("[ark] FAIL\r\n");
    return fails;
}
