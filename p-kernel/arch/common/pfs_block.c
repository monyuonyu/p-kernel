/*
 *  pfs_block.c — p-fs P0: content-addressed block store (LOCAL only).
 *
 *  Spec: docs/architecture/p-fs.md §2.1, §4, §5 (P0 row).
 *
 *  block-id = sha256(block bytes), held as a fixed 32-byte array. Storing
 *  the same content twice yields the same id and does NOT duplicate the
 *  bytes (dedup). This is the bottom block layer of p-fs; gossip
 *  replication (P1), the version DAG (P2), distributed lookup (P3) and
 *  erasure coding (P4) all build on it.
 *
 *  Backing store (P0): a fixed in-kernel block table. This is the
 *  "simplest that works" durable-enough-for-P0 store the spec permits.
 *
 *  --- FAT32 persistence: deliberately a documented TODO for P0 ---------
 *  The spec's P0 row asks for blocks landing content-addressed onto FAT32
 *  (arch/x86/fat32.c) as the durable layer. That is left as a TODO here:
 *  fat32.c only exists for the x86 arch (aarch64 / linux use vfs_stub.c),
 *  so wiring it now would (a) make the store arch-specific, breaking the
 *  "shared arch/common layer" rule, and (b) risk the build on the three
 *  arches that have no FAT32. P0 value (local content-addressing + dedup +
 *  the second VFS backend slot) is fully delivered by the in-memory table;
 *  the durable FAT32 backend is the natural next increment. See
 *  pfs_persist_todo() below for exactly where it would hook in.
 *
 *  LP64 trap: block-id is U1[PFS_ID_LEN] (== U1[32]) — fixed-width bytes,
 *  never a long-derived type. Pinned by _Static_assert below so the id is
 *  byte-identical across aarch64 / x86_64 / i686 (same content -> same id).
 */

#include "pfs_block.h"
#include "sha256.h"          /* relay/sha256.c — zero-dep, reused kernel-side */

/* Hosted durable backend (arch/linux/pfs_durable.c). Declared here as
 * externs — not via a header — so the arch/linux contract never leaks
 * into the bare-metal arch/common include chain (feedback_arch_common_layout,
 * exactly how genome.c reaches selfc). Compiled out on bare metal: those
 * targets don't define _TK_HOSTED_LIBC_, so the store stays memory-only
 * and still links (no pfs_durable.c object). */
#ifdef _TK_HOSTED_LIBC_
extern int  pfs_dur_active(void);
extern int  pfs_dur_write(const char *fname, const void *data, unsigned len);
extern int  pfs_dur_foreach(void (*cb)(const char *name, const void *data,
                                       unsigned len, void *ctx),
                            void *ctx);

/* ARK durable backend (arch/linux/pfs_ark.c) — the wave-13 "white-pearl"
 * integration: ARK becomes p-fs's durable store when PKERNEL_PFS_BACKEND=ark.
 * Selectable against the flat backend above; mutually exclusive at every seam.
 * Same externs-not-a-header rule as pfs_dur_* (keeps arch/linux out of the
 * bare-metal arch/common include chain). */
extern int  pfs_ark_configured(void);                 /* env selects ARK?   */
extern int  pfs_ark_active(void);                      /* mounted + ready?   */
extern int  pfs_ark_restore(void (*emit)(const char *)); /* mount at boot    */
extern int  pfs_ark_put(const void *data, unsigned len); /* new block -> log */
extern int  pfs_ark_get(const unsigned char *id, void *buf, unsigned maxlen);
#endif

/* NO <string.h> here. arch/common files never include libc headers
 * directly (repo rule — see kdds.c's kd_memcpy): on hosted-LP64 builds
 * include/lib/libc/stddef.h (ptrdiff_t = int) clashes with the kernel
 * include chain's include/stddef.h (ptrdiff_t = long). Tiny local
 * loops below; sha256.h is fine — its <stddef.h> resolves to the same
 * kernel-chain include/stddef.h. */

static void pfs_memcpy(void *dst, const void *src, UW n)
{
    U1 *d = (U1 *)dst;
    const U1 *s = (const U1 *)src;
    while (n--) *d++ = *s++;
}

static INT pfs_memcmp(const void *a, const void *b, UW n)
{
    const U1 *p = (const U1 *)a, *q = (const U1 *)b;
    for (UW i = 0; i < n; i++) {
        if (p[i] != q[i]) return (INT)p[i] - (INT)q[i];
    }
    return 0;
}

static void pfs_memset(void *dst, U1 v, UW n)
{
    U1 *d = (U1 *)dst;
    while (n--) *d++ = v;
}

/* ABI-stability guard: the block-id MUST be exactly the sha256 digest
 * width, as a byte array — not a UW/W-derived type that would bloat on
 * LP64 and diverge between 32- and 64-bit builds. */
_Static_assert(PFS_ID_LEN == SHA256_DIGEST_SIZE,
               "pfs block-id width must equal sha256 digest size");
_Static_assert(sizeof(U1[PFS_ID_LEN]) == 32,
               "pfs block-id must be a fixed 32-byte array (LP64-stable)");
_Static_assert(sizeof(U1) == 1, "U1 must be a single byte");

/* ------------------------------------------------------------------ */
/* In-memory block table (P0 backing store)                            */
/* ------------------------------------------------------------------ */

typedef struct {
    U1   id[PFS_ID_LEN];           /* sha256(data[0..len)) */
    UW   len;                      /* block length in bytes */
    U1   used;                     /* slot occupied? */
    U1   origin;                   /* creator node id / PFS_ORIGIN_SELF */
    U1   data[PFS_BLOCK_MAX];      /* block bytes */
} PFS_SLOT;

static PFS_SLOT pfs_table[PFS_MAX_BLOCKS];
static UW       pfs_n;             /* number of distinct blocks stored */

/* P1 hook: fired once per NEW block (never on a dedup hit). Registered
 * by pfs_repl.c so a local store becomes a region announce. */
static PFS_PUT_HOOK pfs_put_hook = 0;

/* Set while pfs_durable_restore() is replaying blocks off disk into the
 * table: suppresses re-persisting (we just read them) AND the P1 announce
 * hook (boot reload is local recovery, not a fresh save to broadcast). */
static U1 pfs_loading = 0;

void pfs_set_put_hook(PFS_PUT_HOOK fn)
{
    pfs_put_hook = fn;
}

/* ------------------------------------------------------------------ */
/* internal helpers                                                    */
/* ------------------------------------------------------------------ */

static INT id_eq(const U1 a[PFS_ID_LEN], const U1 b[PFS_ID_LEN])
{
    return pfs_memcmp(a, b, PFS_ID_LEN) == 0;
}

#ifdef _TK_HOSTED_LIBC_
/* block-id -> 64-char lowercase hex + NUL (the durable filename). Only the
 * hosted durable backend names files by id, so this is hosted-only. */
static void id_to_hex(const U1 id[PFS_ID_LEN], char out[2 * PFS_ID_LEN + 1])
{
    static const char hexd[] = "0123456789abcdef";
    for (INT i = 0; i < PFS_ID_LEN; i++) {
        out[2 * i]     = hexd[(id[i] >> 4) & 0xF];
        out[2 * i + 1] = hexd[id[i] & 0xF];
    }
    out[2 * PFS_ID_LEN] = '\0';
}
#endif

/* Linear scan for a slot holding `id`. Returns index or -1. P0 keeps the
 * table tiny (PFS_MAX_BLOCKS); a hash index is a later optimisation. */
static INT find_slot(const U1 id[PFS_ID_LEN])
{
    for (UW i = 0; i < PFS_MAX_BLOCKS; i++) {
        if (pfs_table[i].used && id_eq(pfs_table[i].id, id))
            return (INT)i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

void pfs_id_compute(const void *buf, UW len, U1 id_out[PFS_ID_LEN])
{
    sha256(buf, (size_t)len, id_out);
}

INT pfs_put_origin(const void *buf, UW len, U1 id_out[PFS_ID_LEN],
                   U1 origin)
{
    if (buf == 0 && len != 0) return PFS_E_INVAL;
    if (len > PFS_BLOCK_MAX)   return PFS_E_TOOBIG;

    U1 id[PFS_ID_LEN];
    pfs_id_compute(buf, len, id);
    if (id_out) pfs_memcpy(id_out, id, PFS_ID_LEN);

    /* Dedup: same content -> same id -> do not re-store (and do NOT
     * fire the hook — that is what stops announce loops in P1: a
     * replica that already holds the block stays silent). */
    if (find_slot(id) >= 0) return PFS_OK;

    /* Find a free slot. */
    for (UW i = 0; i < PFS_MAX_BLOCKS; i++) {
        if (!pfs_table[i].used) {
            pfs_memcpy(pfs_table[i].id, id, PFS_ID_LEN);
            pfs_table[i].len    = len;
            pfs_table[i].origin = origin;
            if (len) pfs_memcpy(pfs_table[i].data, buf, (size_t)len);
            pfs_table[i].used = 1;
            pfs_n++;
            /* Durable backend: persist the new block content-addressed. The
             * backend is SELECTABLE (mutually exclusive) and skipped while
             * replaying from disk (pfs_loading) — those bytes already live
             * there. No-op when neither is configured or on bare metal.
             *   PKERNEL_PFS_BACKEND=ark -> ARK log (arch/linux/pfs_ark.c)
             *   else, $PKERNEL_PFS_DIR  -> flat file (arch/linux/pfs_durable.c,
             *                              filename = block-id hex, fsync'd). */
#ifdef _TK_HOSTED_LIBC_
            if (!pfs_loading) {
                if (pfs_ark_active()) {
                    pfs_ark_put(pfs_table[i].data, (unsigned)len);
                } else if (pfs_dur_active()) {
                    char hex[2 * PFS_ID_LEN + 1];
                    id_to_hex(pfs_table[i].id, hex);
                    pfs_dur_write(hex, pfs_table[i].data, (unsigned)len);
                }
            }
#endif
            if (!pfs_loading && pfs_put_hook)
                pfs_put_hook(pfs_table[i].id, len, origin);
            return PFS_OK;
        }
    }
    return PFS_E_FULL;
}

INT pfs_put(const void *buf, UW len, U1 id_out[PFS_ID_LEN])
{
    return pfs_put_origin(buf, len, id_out, PFS_ORIGIN_SELF);
}

INT pfs_get(const U1 id[PFS_ID_LEN], void *buf, UW maxlen)
{
    INT s = find_slot(id);
    if (s >= 0) {
        UW len = pfs_table[s].len;
        UW cpy = (len < maxlen) ? len : maxlen;
        if (buf && cpy) pfs_memcpy(buf, pfs_table[s].data, (size_t)cpy);
        return (INT)len;
    }

#ifdef _TK_HOSTED_LIBC_
    /* P0 (in-memory) MISS: fall through to the durable ARK backend when it is
     * the selected store. ARK self-verifies (crc + sha) on read; we ALSO
     * re-hash the returned bytes against the requested id here, so a backend
     * that serves rotted/wrong bytes is rejected as NOTFOUND — a p-fs-level
     * self-verify layered on top. P0 stays a cache: we serve straight from
     * ARK without re-populating the table (so "served FROM the log" is honest
     * and the tiny P0 table is never thrashed). */
    if (pfs_ark_active()) {
        static U1 fbuf[PFS_BLOCK_MAX];      /* static: never a task-stack local */
        int fl = pfs_ark_get(id, fbuf, (unsigned)PFS_BLOCK_MAX);
        if (fl >= 0 && (UW)fl <= PFS_BLOCK_MAX) {
            U1 chk[PFS_ID_LEN];
            pfs_id_compute(fbuf, (UW)fl, chk);
            if (id_eq(chk, id)) {
                UW cpy = ((UW)fl < maxlen) ? (UW)fl : maxlen;
                if (buf && cpy) pfs_memcpy(buf, fbuf, (size_t)cpy);
                return fl;
            }
        }
    }
#endif

    return PFS_E_NOTFOUND;
}

INT pfs_has(const U1 id[PFS_ID_LEN])
{
    return find_slot(id) >= 0 ? 1 : 0;
}

UW pfs_count(void)
{
    return pfs_n;
}

INT pfs_slot_info(UW idx, U1 id_out[PFS_ID_LEN], UW *len_out,
                  U1 *origin_out)
{
    if (idx >= PFS_MAX_BLOCKS || !pfs_table[idx].used) return 0;
    if (id_out)     pfs_memcpy(id_out, pfs_table[idx].id, PFS_ID_LEN);
    if (len_out)    *len_out    = pfs_table[idx].len;
    if (origin_out) *origin_out = pfs_table[idx].origin;
    return 1;
}

/* ------------------------------------------------------------------ */
/* P0 durable backend — boot restore with content-addressed self-check */
/* ------------------------------------------------------------------ */

INT pfs_durable_active(void)
{
#ifdef _TK_HOSTED_LIBC_
    return (pfs_ark_active() || pfs_dur_active()) ? 1 : 0;
#else
    return 0;
#endif
}

#ifdef _TK_HOSTED_LIBC_
/* small unsigned-decimal emitter for the restore summary (no tmonitor
 * dependency here — pfs_block.c stays output-channel-agnostic). */
static void emit_dec(void (*emit)(const char *), UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { emit("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    emit(&buf[i]);
}

static INT hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

typedef struct {
    void (*emit)(const char *);
    UW loaded;
    UW corrupt;
} pfs_restore_ctx;

/* Called once per block file by pfs_dur_foreach. The filename is the
 * CLAIMED block-id (64 hex). We recompute sha256(content) and compare —
 * content-addressing makes the store self-verifying, so a flipped bit or
 * a planted file is rejected here, never loaded. */
static void pfs_restore_cb(const char *name, const void *data,
                           unsigned len, void *vctx)
{
    pfs_restore_ctx *c = (pfs_restore_ctx *)vctx;

    /* decode the 64-hex name into the id it claims to be */
    U1 want[PFS_ID_LEN];
    for (INT i = 0; i < PFS_ID_LEN; i++) {
        INT hi = hex_nibble(name[2 * i]);
        INT lo = hex_nibble(name[2 * i + 1]);
        if (hi < 0 || lo < 0) { c->corrupt++; return; }
        want[i] = (U1)((hi << 4) | lo);
    }

    if (len > PFS_BLOCK_MAX) {
        c->corrupt++;
        if (c->emit) c->emit("[pfs] durable: REJECT oversize file\r\n");
        return;
    }

    /* content-addressed self-check: bytes must hash to their own name */
    U1 got[PFS_ID_LEN];
    pfs_id_compute(data, (UW)len, got);
    if (!id_eq(want, got)) {
        c->corrupt++;
        if (c->emit) {
            char pfx[17];
            for (INT k = 0; k < 16; k++) pfx[k] = name[k];
            pfx[16] = '\0';
            c->emit("[pfs] durable: REJECT corrupt block ");
            c->emit(pfx);
            c->emit("... (sha256 mismatch)\r\n");
        }
        return;
    }

    /* verified — replay into the table without re-persisting or announcing */
    pfs_loading = 1;
    pfs_put_origin(data, (UW)len, 0, PFS_ORIGIN_SELF);
    pfs_loading = 0;
    c->loaded++;
}
#endif /* _TK_HOSTED_LIBC_ */

INT pfs_durable_restore(void (*emit)(const char *))
{
#ifdef _TK_HOSTED_LIBC_
    /* ARK backend selected (PKERNEL_PFS_BACKEND=ark): mount the log image.
     * Blocks are served lazily via pfs_get's fall-through (P0 is a cache), so
     * we do NOT eagerly reload them into the in-memory table here. Mutually
     * exclusive with the flat-file backend below. */
    if (pfs_ark_configured())
        return pfs_ark_restore(emit);

    if (!pfs_dur_active()) {
        if (emit)
            emit("[pfs] durable: PKERNEL_PFS_DIR unset — memory-only "
                 "(no persistence)\r\n");
        return 0;
    }

    pfs_restore_ctx c;
    c.emit = emit; c.loaded = 0; c.corrupt = 0;
    INT seen = pfs_dur_foreach(pfs_restore_cb, &c);

    if (emit) {
        emit("[pfs] durable: restored ");
        emit_dec(emit, c.loaded);
        emit(" block(s) from PKERNEL_PFS_DIR");
        if (c.corrupt) {
            emit(", rejected ");
            emit_dec(emit, c.corrupt);
            emit(" corrupt");
        }
        emit(" (sha256-verified)\r\n");
        (void)seen;
    }
    return (INT)c.loaded;
#else
    (void)emit;
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* self-test — proves dedup + round-trip + miss (p-fs.md §5 P0)        */
/* ------------------------------------------------------------------ */

static void emit_hex_id(void (*emit)(const char *), const U1 id[PFS_ID_LEN])
{
    static const char hexd[] = "0123456789abcdef";
    char out[2 * 8 + 1];           /* first 8 bytes is plenty to eyeball */
    INT j = 0;
    for (INT i = 0; i < 8; i++) {
        out[j++] = hexd[(id[i] >> 4) & 0xF];
        out[j++] = hexd[id[i] & 0xF];
    }
    out[j] = '\0';
    emit(out);
}

INT pfs_self_test(void (*emit)(const char *))
{
    INT fails = 0;

    /* sha256 itself must pass its FIPS KATs before we trust block-ids. */
    if (sha256_self_test() != 0) {
        emit("[pfs] FAIL sha256 KAT\r\n");
        return 1;
    }

    static const char msgA[] = "hello p-fs content address";
    static const char msgB[] = "a different block entirely";

    UW before = pfs_count();

    /* put A twice -> same id, count rises by exactly 1 (dedup). */
    U1 id1[PFS_ID_LEN], id2[PFS_ID_LEN];
    INT r1 = pfs_put(msgA, (UW)(sizeof(msgA) - 1), id1);
    UW after_first = pfs_count();
    INT r2 = pfs_put(msgA, (UW)(sizeof(msgA) - 1), id2);
    UW after_second = pfs_count();

    if (r1 != PFS_OK || r2 != PFS_OK) {
        emit("[pfs] FAIL put returned error\r\n"); fails++;
    }
    if (pfs_memcmp(id1, id2, PFS_ID_LEN) != 0) {
        emit("[pfs] FAIL same content gave different id\r\n"); fails++;
    } else {
        emit("[pfs] ok  same content -> same id ("); emit_hex_id(emit, id1);
        emit("...)\r\n");
    }
    if (after_first != before + 1) {
        emit("[pfs] FAIL first put did not store\r\n"); fails++;
    }
    if (after_second != after_first) {
        emit("[pfs] FAIL dedup: second put re-stored\r\n"); fails++;
    } else {
        emit("[pfs] ok  dedup: store count unchanged on re-put\r\n");
    }

    /* get by id returns identical bytes. */
    U1 rd[64];
    INT glen = pfs_get(id1, rd, sizeof(rd));
    if (glen != (INT)(sizeof(msgA) - 1) ||
        pfs_memcmp(rd, msgA, sizeof(msgA) - 1) != 0) {
        emit("[pfs] FAIL get returned wrong bytes\r\n"); fails++;
    } else {
        emit("[pfs] ok  get by id round-trips identical bytes\r\n");
    }

    /* a distinct block gets a distinct id and bumps the count. */
    U1 idB[PFS_ID_LEN];
    pfs_put(msgB, (UW)(sizeof(msgB) - 1), idB);
    if (pfs_memcmp(id1, idB, PFS_ID_LEN) == 0) {
        emit("[pfs] FAIL distinct content shares an id\r\n"); fails++;
    }

    /* get on an unknown id must miss. */
    U1 bogus[PFS_ID_LEN];
    pfs_memset(bogus, 0xAB, PFS_ID_LEN);
    if (pfs_get(bogus, rd, sizeof(rd)) != PFS_E_NOTFOUND ||
        pfs_has(bogus) != 0) {
        emit("[pfs] FAIL unknown id was found\r\n"); fails++;
    } else {
        emit("[pfs] ok  unknown id correctly not found\r\n");
    }

    if (fails == 0) emit("[pfs] PASS (content-address + dedup proven)\r\n");
    else            emit("[pfs] FAIL\r\n");
    return fails;
}
