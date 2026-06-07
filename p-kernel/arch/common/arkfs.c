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
#define ARK_SB_MAGIC  "ARKLOG01"     /* 8 bytes */
#define ARK_REC_MAGIC "ARKR"         /* 4 bytes */
#define ARK_FMT_VERSION 1u

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

typedef struct { U1 id[ARK_ID_LEN]; U4 sec; U4 len; U1 used; } ark_idx;
static ark_idx   g_idx[ARK_MAX_INDEX];    /* block-id -> log location     */

/* static scratch — never on a task stack (arch/common rule) */
static U1 g_secbuf[ARK_SECTOR];
static U1 g_blkbuf[ARK_BLOCK_MAX];
static U1 g_payld [ARK_COMMIT_MAX];       /* generic payload buffer       */
static U1 g_cbuf  [ARK_COMMIT_MAX];       /* commit serialization buffer  */

/* ------------------------------------------------------------------ */
/* block index helpers                                                 */
/* ------------------------------------------------------------------ */

static INT idx_find(const U1 id[ARK_ID_LEN])
{
    for (U4 i = 0; i < ARK_MAX_INDEX; i++)
        if (g_idx[i].used && ark_memeq(g_idx[i].id, id, ARK_ID_LEN)) return (INT)i;
    return -1;
}

static INT idx_add(const U1 id[ARK_ID_LEN], U4 sec, U4 len)
{
    if (idx_find(id) >= 0) return ARK_OK;          /* dedup */
    for (U4 i = 0; i < ARK_MAX_INDEX; i++) {
        if (!g_idx[i].used) {
            ark_memcpy(g_idx[i].id, id, ARK_ID_LEN);
            g_idx[i].sec = sec; g_idx[i].len = len; g_idx[i].used = 1;
            return ARK_OK;
        }
    }
    return ARK_E_FULL;
}

static U4 idx_count(void)
{
    U4 c = 0;
    for (U4 i = 0; i < ARK_MAX_INDEX; i++) { if (g_idx[i].used) c++; }
    return c;
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
    if (g_head + need > g_bd->total_sectors) return ARK_E_FULL;

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
 * Read & fully validate the record whose header is at sector `sec` into
 * `payout` (capacity paymax). Returns sectors consumed (>0) if the record
 * is fully valid; 0 if the slot is empty / not a record (clean end of log);
 * ARK_E_CORRUPT if the header is a record but payload integrity fails.
 * *type_out / *seq_out / *len_out are filled when >0.
 */
static INT read_record(U4 sec, void *payout, U4 paymax,
                       U4 *type_out, U4 *seq_out, U4 *len_out,
                       U1 id_out[ARK_ID_LEN])
{
    if (sec >= g_bd->total_sectors) return 0;
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
    if (sec + 1 + psect > g_bd->total_sectors) return 0;

    /* gather payload */
    U1 *p = (U1 *)payout;
    for (U4 s = 0; s < psect; s++) {
        if (dev_read(sec + 1 + s, 1, g_secbuf) < 0) return 0;
        U4 off = s * ARK_SECTOR;
        U4 chunk = (h.len - off < ARK_SECTOR) ? (h.len - off) : ARK_SECTOR;
        ark_memcpy(p + off, g_secbuf, chunk);
    }

    if (ark_crc32(payout, h.len) != h.payload_crc)
        return ARK_E_CORRUPT;                                  /* torn / rot */

    /* content-address self-verify: id must equal sha256(payload) */
    U1 chk[ARK_ID_LEN];
    sha256(payout, (size_t)h.len, chk);
    if (!ark_memeq(chk, h.id, ARK_ID_LEN))
        return ARK_E_CORRUPT;

    if (type_out) *type_out = h.type;
    if (seq_out)  *seq_out  = h.seq;
    if (len_out)  *len_out  = h.len;
    if (id_out)   ark_memcpy(id_out, h.id, ARK_ID_LEN);
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

/* Append the current live table as a new COMMIT. Atomic visibility point. */
static INT commit_live(void)
{
    U4 len = serialize_live(g_cbuf);
    U1 id[ARK_ID_LEN];
    sha256(g_cbuf, (size_t)len, id);
    INT r = emit_record(ARK_REC_COMMIT, g_cbuf, len, id, 0);
    if (r == ARK_OK) dev_sync();           /* make the commit durable */
    return r;
}

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
    ark_memset(g_idx, 0, sizeof(g_idx));
    ark_memset(g_live, 0, sizeof(g_live));
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

    /* superblock */
    ark_memset(g_secbuf, 0, ARK_SECTOR);
    ark_super *sb = (ark_super *)g_secbuf;
    ark_memcpy(sb->magic, ARK_SB_MAGIC, 8);
    sb->version       = ARK_FMT_VERSION;
    sb->sector_size   = ARK_SECTOR;
    sb->log_start     = g_log_start;
    sb->total_sectors = bd->total_sectors;
    sb->epoch         = g_epoch;
    sb->crc           = ark_crc32(sb, (U4)((U1 *)&sb->crc - (U1 *)sb));
    if (dev_write(0, 1, g_secbuf) < 0) return ARK_E_IO;

    /* initial empty commit so a fresh image mounts cleanly */
    INT r = commit_live();
    if (r != ARK_OK) return r;
    dev_sync();
    return ARK_OK;
}

INT ark_mount(ARK_BDEV *bd)
{
    if (!bd || bd->sector_size != ARK_SECTOR) return ARK_E_INVAL;
    g_bd = bd;

    if (dev_read(0, 1, g_secbuf) < 0) return ARK_E_IO;
    ark_super *sb = (ark_super *)g_secbuf;
    if (!ark_memeq(sb->magic, ARK_SB_MAGIC, 8)) return ARK_E_CORRUPT;
    if (ark_crc32(sb, (U4)((U1 *)&sb->crc - (U1 *)sb)) != sb->crc)
        return ARK_E_CORRUPT;
    if (sb->version != ARK_FMT_VERSION || sb->sector_size != ARK_SECTOR)
        return ARK_E_INVAL;
    g_log_start = sb->log_start;
    g_epoch     = sb->epoch;
    reset_state();

    /* Replay the log. Track blocks since the last commit ("pending"); on a
     * valid commit they become permanent index entries and the live table is
     * adopted. A torn/garbage tail stops the scan and the pending blocks are
     * dropped (uncommitted -> rolled back). */
    static ark_idx pend[ARK_MAX_INDEX];
    U4 pend_n = 0;
    U4 sec = g_log_start;
    U4 head_after_commit = g_log_start;

    for (;;) {
        U4 type, seq, len; U1 id[ARK_ID_LEN];
        INT n = read_record(sec, g_payld, ARK_COMMIT_MAX, &type, &seq, &len, id);
        if (n <= 0) break;                       /* clean end or torn tail */

        if (type == ARK_REC_BLOCK) {
            if (pend_n < ARK_MAX_INDEX) {
                ark_memcpy(pend[pend_n].id, id, ARK_ID_LEN);
                pend[pend_n].sec = sec; pend[pend_n].len = len;
                pend[pend_n].used = 1; pend_n++;
            }
        } else if (type == ARK_REC_COMMIT) {
            g_live_n = parse_commit(g_payld, len, g_live, ARK_MAX_FILES);
            for (U4 i = 0; i < pend_n; i++)
                idx_add(pend[i].id, pend[i].sec, pend[i].len);
            pend_n = 0;
            g_seq = seq;
            head_after_commit = sec + (U4)n;
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
    return idx_add(id, start, len);
}

INT ark_block_get(const U1 id[ARK_ID_LEN], void *buf, U4 max)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    INT s = idx_find(id);
    if (s < 0) return ARK_E_NOTFOUND;

    U4 type, seq, len;
    INT n = read_record(g_idx[s].sec, g_payld, ARK_COMMIT_MAX,
                        &type, &seq, &len, 0);
    if (n == ARK_E_CORRUPT) return ARK_E_CORRUPT;  /* self-verify caught rot */
    if (n <= 0 || type != ARK_REC_BLOCK) return ARK_E_CORRUPT;

    U4 cpy = (len < max) ? len : max;
    if (buf && cpy) ark_memcpy(buf, g_payld, cpy);
    return (INT)len;
}

INT ark_block_has(const U1 id[ARK_ID_LEN]) { return idx_find(id) >= 0 ? 1 : 0; }
U4  ark_block_count(void) { return idx_count(); }

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

INT ark_read_file(const char *path, void *buf, U4 max)
{
    if (!g_bd) return ARK_E_NOTMOUNTED;
    INT e = live_find(path);
    if (e < 0 || g_live[e].is_dir) return ARK_E_NOTFOUND;

    U4 total = g_live[e].size;
    U4 done  = 0;
    for (U4 i = 0; i < g_live[e].nblk; i++) {
        INT bl = ark_block_get(g_live[e].blk[i], g_blkbuf, ARK_BLOCK_MAX);
        if (bl == ARK_E_CORRUPT) return ARK_E_CORRUPT;
        if (bl < 0) return ARK_E_NOTFOUND;
        U4 cpy = (U4)bl;
        if (done + cpy > max) cpy = (done < max) ? max - done : 0;
        if (buf && cpy) ark_memcpy((U1 *)buf + done, g_blkbuf, cpy);
        done += (U4)bl;
    }
    (void)total;
    return (INT)g_live[e].size;
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
        U4 type, seq, len;
        INT n = read_record(sec, g_payld, ARK_COMMIT_MAX, &type, &seq, &len, 0);
        if (n <= 0) break;
        if (type == ARK_REC_COMMIT) {
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
        U4 type, seq, len;
        INT n = read_record(sec, g_payld, ARK_COMMIT_MAX, &type, &seq, &len, 0);
        if (n <= 0) break;
        if (type == ARK_REC_COMMIT) {
            U4 ntb = parse_commit(g_payld, len, tbl, ARK_MAX_FILES);
            for (U4 i = 0; i < ntb; i++) {
                if (ark_streq(tbl[i].name, path) && tbl[i].version == version) {
                    U4 done = 0;
                    for (U4 b = 0; b < tbl[i].nblk; b++) {
                        INT bl = ark_block_get(tbl[i].blk[b], g_blkbuf, ARK_BLOCK_MAX);
                        if (bl == ARK_E_CORRUPT) return ARK_E_CORRUPT;
                        if (bl < 0) return ARK_E_NOTFOUND;
                        U4 cpy = (U4)bl;
                        if (done + cpy > max) cpy = (done < max) ? max - done : 0;
                        if (buf && cpy) ark_memcpy((U1 *)buf + done, g_blkbuf, cpy);
                        done += (U4)bl;
                    }
                    return (INT)tbl[i].size;
                }
            }
        }
        sec += (U4)n;
    }
    return ARK_E_NOTFOUND;
}

/* ------------------------------------------------------------------ */
/* self-test (RAM-backed) — CRUD + version + dedup + verify + crash    */
/* ------------------------------------------------------------------ */

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

    if (fails == 0) emit("[ark] PASS (content-address + versioned + crash-safe)\r\n");
    else            emit("[ark] FAIL\r\n");
    return fails;
}
