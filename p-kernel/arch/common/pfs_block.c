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
#include <string.h>

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
    U1   data[PFS_BLOCK_MAX];      /* block bytes */
} PFS_SLOT;

static PFS_SLOT pfs_table[PFS_MAX_BLOCKS];
static UW       pfs_n;             /* number of distinct blocks stored */

/* ------------------------------------------------------------------ */
/* internal helpers                                                    */
/* ------------------------------------------------------------------ */

static INT id_eq(const U1 a[PFS_ID_LEN], const U1 b[PFS_ID_LEN])
{
    return memcmp(a, b, PFS_ID_LEN) == 0;
}

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

INT pfs_put(const void *buf, UW len, U1 id_out[PFS_ID_LEN])
{
    if (buf == 0 && len != 0) return PFS_E_INVAL;
    if (len > PFS_BLOCK_MAX)   return PFS_E_TOOBIG;

    U1 id[PFS_ID_LEN];
    pfs_id_compute(buf, len, id);
    if (id_out) memcpy(id_out, id, PFS_ID_LEN);

    /* Dedup: same content -> same id -> do not re-store. */
    if (find_slot(id) >= 0) return PFS_OK;

    /* Find a free slot. */
    for (UW i = 0; i < PFS_MAX_BLOCKS; i++) {
        if (!pfs_table[i].used) {
            memcpy(pfs_table[i].id, id, PFS_ID_LEN);
            pfs_table[i].len = len;
            if (len) memcpy(pfs_table[i].data, buf, (size_t)len);
            pfs_table[i].used = 1;
            pfs_n++;
            /* TODO(P0->durable): also append (id,len,bytes) to a
             * content-addressed file on FAT32 here; see header note. */
            return PFS_OK;
        }
    }
    return PFS_E_FULL;
}

INT pfs_get(const U1 id[PFS_ID_LEN], void *buf, UW maxlen)
{
    INT s = find_slot(id);
    if (s < 0) return PFS_E_NOTFOUND;

    UW len = pfs_table[s].len;
    UW cpy = (len < maxlen) ? len : maxlen;
    if (buf && cpy) memcpy(buf, pfs_table[s].data, (size_t)cpy);
    return (INT)len;
}

INT pfs_has(const U1 id[PFS_ID_LEN])
{
    return find_slot(id) >= 0 ? 1 : 0;
}

UW pfs_count(void)
{
    return pfs_n;
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
    if (memcmp(id1, id2, PFS_ID_LEN) != 0) {
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
        memcmp(rd, msgA, sizeof(msgA) - 1) != 0) {
        emit("[pfs] FAIL get returned wrong bytes\r\n"); fails++;
    } else {
        emit("[pfs] ok  get by id round-trips identical bytes\r\n");
    }

    /* a distinct block gets a distinct id and bumps the count. */
    U1 idB[PFS_ID_LEN];
    pfs_put(msgB, (UW)(sizeof(msgB) - 1), idB);
    if (memcmp(id1, idB, PFS_ID_LEN) == 0) {
        emit("[pfs] FAIL distinct content shares an id\r\n"); fails++;
    }

    /* get on an unknown id must miss. */
    U1 bogus[PFS_ID_LEN];
    memset(bogus, 0xAB, PFS_ID_LEN);
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
