/*
 *  lookup.c — decentralized lookup L0: stateless rendezvous hashing (HRW).
 *
 *  Spec: docs/architecture/decentralized-lookup.md §2.1, §3.1, §6 (L0).
 *
 *  responsible(k, r) = ALIVE nodes sorted by weight(n, k) descending,
 *  top r. weight(n, k) = sha256(key || node_id). No ring to maintain,
 *  no state to sync: "a stateless deterministic computation is a virtue
 *  in a world where membership views drift — it confines the source of
 *  drift to membership alone" (§2.1). The convenience wrappers read only
 *  the local egocentric views (region.h / dnode_table[]) by design.
 *
 *  --- Scoring construction (the cross-ABI contract) ------------------
 *  Full sha256 over (key[32] || node_id[1]), then the score is the
 *  digest's FIRST 8 BYTES interpreted big-endian as two U4 words:
 *      hi = d[0]<<24 | d[1]<<16 | d[2]<<8 | d[3]
 *      lo = d[4]<<24 | d[5]<<16 | d[6]<<8 | d[7]
 *  Compare (hi, lo) lexicographically, higher weight ranks first.
 *  Why not "use the whole digest"? 64 bits of sha256 prefix already
 *  makes ties a 2^-64 event per pair; two U4 words keep every operation
 *  fixed-width (no UD/long math, no endianness dependence — the bytes
 *  are picked out explicitly), which is what makes the ranking
 *  byte-stable across aarch64 / x86_64 / i686. Residual ties break to
 *  the LOWER node id so the order stays total.
 *
 *  Per the L0 note in the doc (net_relay.c lesson,
 *  feedback_hosted_relay_stack_overflow): the scratch here is small —
 *  LOOKUP_MAX_MEMBERS * sizeof(HRW_SCORE) = 64 * 12 = 768 bytes — and
 *  this is not a broadcast-RX hot path, so stack locals are fine.
 */

#include "lookup.h"
#include "region.h"          /* region_recompute/region_is_member         */
#include "drpc.h"            /* dnode_table[], DNODE_MAX, DNODE_ALIVE     */
#include "sha256.h"          /* relay/sha256.c — zero-dep, kernel-side    */

/* NO <string.h> here (repo rule — see pfs_block.c / kdds.c kd_memcpy):
 * on hosted-LP64 builds include/lib/libc/stddef.h clashes with the
 * kernel include chain's include/stddef.h. Local helper loops only. */

/* ------------------------------------------------------------------ */
/* ABI-stability guards                                                */
/* ------------------------------------------------------------------ */

_Static_assert(LOOKUP_KEY_LEN == SHA256_DIGEST_SIZE,
               "lookup key width must equal sha256 digest size");
_Static_assert(sizeof(U1) == 1, "U1 must be a single byte");
_Static_assert(sizeof(UB) == 1, "UB must be a single byte");
_Static_assert(sizeof(U4) == 4, "U4 must be exactly 32 bits (LP64 trap)");
_Static_assert(LOOKUP_MAX_MEMBERS >= DNODE_MAX,
               "HRW population cap must cover the whole dnode table");

/* One (weight, id) entry during ranking. Scores live in fixed-width
 * U4 pairs derived from digest bytes — never long-width arithmetic. */
typedef struct {
    U4 hi;     /* digest bytes 0..3, big-endian */
    U4 lo;     /* digest bytes 4..7, big-endian */
    UB id;     /* candidate node id             */
} HRW_SCORE;

_Static_assert(sizeof(HRW_SCORE) <= 12,
               "HRW_SCORE must stay small (stack-resident ranking array)");

/* ------------------------------------------------------------------ */
/* weight(n, k) = sha256(key || node_id), first 8 digest bytes         */
/* ------------------------------------------------------------------ */

static void hrw_weight(const U1 key[LOOKUP_KEY_LEN], UB node,
                       U4 *hi, U4 *lo)
{
    U1 buf[LOOKUP_KEY_LEN + 1];
    for (INT i = 0; i < LOOKUP_KEY_LEN; i++) buf[i] = key[i];
    buf[LOOKUP_KEY_LEN] = node;

    U1 d[SHA256_DIGEST_SIZE];
    sha256(buf, sizeof(buf), d);

    /* Explicit byte picks — endianness-independent by construction. */
    *hi = ((U4)d[0] << 24) | ((U4)d[1] << 16) | ((U4)d[2] << 8) | (U4)d[3];
    *lo = ((U4)d[4] << 24) | ((U4)d[5] << 16) | ((U4)d[6] << 8) | (U4)d[7];
}

/* a ranks before b? Higher (hi, lo) wins; tie -> lower node id. */
static INT hrw_before(const HRW_SCORE *a, const HRW_SCORE *b)
{
    if (a->hi != b->hi) return a->hi > b->hi;
    if (a->lo != b->lo) return a->lo > b->lo;
    return a->id < b->id;
}

/* ------------------------------------------------------------------ */
/* Core: pure responsible(k, r)                                        */
/* ------------------------------------------------------------------ */

INT lookup_responsible(const U1 key[LOOKUP_KEY_LEN],
                       const UB *members, INT n_members,
                       UB out[], INT r)
{
    if (key == NULL || members == NULL || out == NULL) return -1;
    if (n_members <= 0 || r <= 0)                      return -1;
    if (n_members > LOOKUP_MAX_MEMBERS)                return -1;

    HRW_SCORE sc[LOOKUP_MAX_MEMBERS];

    /* Score every candidate, inserting in rank order (n <= 64; an
     * insertion sort is simpler than anything cleverer here). */
    INT cnt = 0;
    for (INT i = 0; i < n_members; i++) {
        HRW_SCORE cur;
        cur.id = members[i];
        hrw_weight(key, cur.id, &cur.hi, &cur.lo);

        INT j = cnt;
        while (j > 0 && hrw_before(&cur, &sc[j - 1])) {
            sc[j] = sc[j - 1];
            j--;
        }
        sc[j] = cur;
        cnt++;
    }

    INT take = (r < cnt) ? r : cnt;
    for (INT i = 0; i < take; i++) out[i] = sc[i].id;
    return take;
}

/* ------------------------------------------------------------------ */
/* Convenience wrappers — egocentric local views (documented contract) */
/* ------------------------------------------------------------------ */

INT lookup_responsible_region(const U1 key[LOOKUP_KEY_LEN],
                              UB out[], INT r)
{
    UB members[DNODE_MAX];
    INT n = 0;

    region_recompute();
    for (UB i = 0; i < DNODE_MAX; i++) {
        if (region_is_member(i)) members[n++] = i;
    }
    if (n == 0) return 0;   /* no cluster yet (drpc_my_node == 0xFF) */
    return lookup_responsible(key, members, n, out, r);
}

INT lookup_responsible_alive(const U1 key[LOOKUP_KEY_LEN],
                             UB out[], INT r)
{
    UB members[DNODE_MAX];
    INT n = 0;

    for (UB i = 0; i < DNODE_MAX; i++) {
        if (dnode_table[i].state == DNODE_ALIVE) members[n++] = i;
    }
    if (n == 0) return 0;   /* no cluster yet */
    return lookup_responsible(key, members, n, out, r);
}

/* ------------------------------------------------------------------ */
/* self-test — determinism + cross-ABI vector + §3.1 overlap           */
/* ------------------------------------------------------------------ */

static void emit_dec(void (*emit)(const char *), INT v)
{
    char buf[12];
    INT i = 11;
    buf[i] = '\0';
    if (v == 0) { emit("0"); return; }
    if (v < 0)  { emit("-"); v = -v; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    emit(&buf[i]);
}

static void emit_ids(void (*emit)(const char *), const UB *ids, INT n)
{
    for (INT i = 0; i < n; i++) {
        if (i) emit(" ");
        emit_dec(emit, (INT)ids[i]);
    }
}

INT lookup_self_test(void (*emit)(const char *))
{
    INT fails = 0;

    /* sha256 must pass its FIPS KATs before we trust any weight. */
    if (sha256_self_test() != 0) {
        emit("[hrw] FAIL sha256 KAT\r\n");
        return 1;
    }

    UB members8[8];
    for (UB i = 0; i < 8; i++) members8[i] = i;

    /* The fixed test key: bytes 0x00..0x1f. Trivially reproducible on
     * any arch, any language — that is what a regression vector wants. */
    U1 key[LOOKUP_KEY_LEN];
    for (INT i = 0; i < LOOKUP_KEY_LEN; i++) key[i] = (U1)i;

    /* (1) determinism: same key + members twice -> identical top-r. */
    UB a[8], b[8];
    INT na = lookup_responsible(key, members8, 8, a, 3);
    INT nb = lookup_responsible(key, members8, 8, b, 3);
    INT same = (na == 3 && nb == 3);
    for (INT i = 0; same && i < 3; i++) same = (a[i] == b[i]);
    if (same) {
        emit("[hrw] ok  determinism: same input twice -> identical top-3\r\n");
    } else {
        emit("[hrw] FAIL determinism\r\n"); fails++;
    }

    /* (2) cross-ABI known vector: full ranking of {0..7} for the fixed
     * key. This exact line must match on aarch64 / x86_64 / i686. */
    UB full[8];
    INT nf = lookup_responsible(key, members8, 8, full, 8);
    if (nf != 8) {
        emit("[hrw] FAIL known-vector rank count\r\n"); fails++;
    } else {
        emit("[hrw] vector key=00..1f members={0..7} ranking: ");
        emit_ids(emit, full, 8);
        emit("  top-3: ");
        emit_ids(emit, full, 3);
        emit("\r\n");
    }

    /* (3) §3.1 overlap: two views differing by ONE node must keep their
     * top-2 sets intersecting. Removal case: view B = A minus node 7.
     * Addition case: view B' = A plus node 8. 16 keys each. */
    UB members7[7], members9[9];
    for (UB i = 0; i < 7; i++) members7[i] = i;
    for (UB i = 0; i < 9; i++) members9[i] = i;

    INT hit_rm = 0, hit_add = 0;
    const INT NKEYS = 16;
    for (INT k = 0; k < NKEYS; k++) {
        U1 kk[LOOKUP_KEY_LEN];
        for (INT i = 0; i < LOOKUP_KEY_LEN; i++)
            kk[i] = (U1)(i * 7 + k * 31 + 1);

        UB ta[2], tb[2];
        lookup_responsible(kk, members8, 8, ta, 2);

        lookup_responsible(kk, members7, 7, tb, 2);     /* one node removed */
        if (ta[0] == tb[0] || ta[0] == tb[1] ||
            ta[1] == tb[0] || ta[1] == tb[1]) hit_rm++;

        lookup_responsible(kk, members9, 9, tb, 2);     /* one node added */
        if (ta[0] == tb[0] || ta[0] == tb[1] ||
            ta[1] == tb[0] || ta[1] == tb[1]) hit_add++;
    }
    emit("[hrw] overlap (one node removed): top-2 sets intersect ");
    emit_dec(emit, hit_rm); emit("/"); emit_dec(emit, NKEYS); emit(" keys\r\n");
    emit("[hrw] overlap (one node added):   top-2 sets intersect ");
    emit_dec(emit, hit_add); emit("/"); emit_dec(emit, NKEYS); emit(" keys\r\n");
    if (hit_rm < NKEYS || hit_add < NKEYS) {
        /* With r=2 and a single-node delta the intersection is provably
         * non-empty (the survivor of A's top-2 can drop at most one
         * rank); anything below 16/16 means the ranking is unstable. */
        emit("[hrw] FAIL overlap property (sets must intersect)\r\n");
        fails++;
    } else {
        emit("[hrw] ok  one-node view drift keeps top-2 sets overlapping\r\n");
    }

    if (fails == 0) emit("[hrw] PASS (HRW responsible(k,r) deterministic + drift-tolerant)\r\n");
    else            emit("[hrw] FAIL\r\n");
    return fails;
}

/* ================================================================== */
/* L1 — resolution cache + read-k candidates                          */
/* (doc §2.3 "成功を覚える", §3.2 read-k, §6 L1 row)                  */
/* ================================================================== */

/*
 *  The cache is a LOCAL OBSERVATION table in the world-table style
 *  (world.c): each entry remembers "key k was actually FOUND on node n
 *  at local time t". It is never gossiped, never synced, never treated
 *  as truth — staleness (LOOKUP_CACHE_TTL_MS, same value as
 *  WORLD_STALE_MS) makes old observations decay back to the stateless
 *  HRW guess. A wrong entry costs one extra hop at L2; it can never
 *  make a key unfindable, because lookup_candidates_in() always appends
 *  the full HRW order after the (at most one) cached candidate.
 *
 *  LP64 note: seen_ms is U4 on purpose (feedback_lp64_typedef_trap) —
 *  freshness math is done in 32-bit unsigned wraparound arithmetic,
 *  identical on every ABI.
 */

typedef struct {
    U1 key[LOOKUP_KEY_LEN];  /* the resolved key                        */
    U4 seen_ms;              /* local tk_get_otm().lo at observation    */
    UB node;                 /* where the key was actually found        */
    UB valid;                /* 1 = slot in use                         */
} LOOKUP_CACHE_ENT;

_Static_assert(sizeof(U4) == 4, "U4 must be 32 bits (LP64 trap)");
_Static_assert(sizeof(LOOKUP_CACHE_ENT) == 40,
               "cache entry layout must be LP64-stable (32+4+1+1+2pad)");
_Static_assert(LOOKUP_CACHE_SIZE > 0 && LOOKUP_CACHE_SIZE <= 256,
               "cache must stay small and index-able by INT");

static LOOKUP_CACHE_ENT lk_cache[LOOKUP_CACHE_SIZE];

/* Local clock in ms (32-bit lo is plenty for age math; wraparound-safe
 * because freshness uses unsigned (now - seen) like world.c). */
static U4 lk_now_ms(void)
{
    SYSTIM t; tk_get_otm(&t);
    return (U4)t.lo;
}

static INT lk_key_eq(const U1 *a, const U1 *b)
{
    for (INT i = 0; i < LOOKUP_KEY_LEN; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* entry age in ms (unsigned wraparound math, ABI-uniform) */
static U4 lk_age(const LOOKUP_CACHE_ENT *e, U4 now)
{
    return (U4)(now - e->seen_ms);
}

static INT lk_fresh(const LOOKUP_CACHE_ENT *e, U4 now)
{
    return e->valid && lk_age(e, now) <= (U4)LOOKUP_CACHE_TTL_MS;
}

void lookup_note_found(const U1 key[LOOKUP_KEY_LEN], UB node)
{
    if (key == NULL) return;

    U4 now = lk_now_ms();

    /* 1) same key already cached -> refresh in place */
    for (INT i = 0; i < LOOKUP_CACHE_SIZE; i++) {
        if (lk_cache[i].valid && lk_key_eq(lk_cache[i].key, key)) {
            lk_cache[i].node    = node;
            lk_cache[i].seen_ms = now;
            return;
        }
    }

    /* 2) eviction: free slot > oldest expired slot > oldest live slot.
     * Deterministic: scan order breaks age ties to the lowest index. */
    INT victim = -1;
    U4  victim_age = 0;
    for (INT i = 0; i < LOOKUP_CACHE_SIZE; i++) {
        if (!lk_cache[i].valid) { victim = i; break; }   /* free wins  */
        U4 age = lk_age(&lk_cache[i], now);
        if (victim < 0 || age > victim_age) {
            victim = i; victim_age = age;                /* oldest     */
        }
    }

    for (INT i = 0; i < LOOKUP_KEY_LEN; i++) lk_cache[victim].key[i] = key[i];
    lk_cache[victim].node    = node;
    lk_cache[victim].seen_ms = now;
    lk_cache[victim].valid   = 1;
}

INT lookup_cache_get(const U1 key[LOOKUP_KEY_LEN], UB *node)
{
    if (key == NULL || node == NULL) return 0;

    U4 now = lk_now_ms();
    for (INT i = 0; i < LOOKUP_CACHE_SIZE; i++) {
        if (lk_cache[i].valid && lk_key_eq(lk_cache[i].key, key)) {
            if (!lk_fresh(&lk_cache[i], now)) {
                lk_cache[i].valid = 0;   /* expired: drop on touch */
                return 0;
            }
            *node = lk_cache[i].node;
            return 1;
        }
    }
    return 0;
}

void lookup_cache_clear(void)
{
    for (INT i = 0; i < LOOKUP_CACHE_SIZE; i++) lk_cache[i].valid = 0;
}

void lookup_cache_age_for_test(U4 ms)
{
    /* Pretend `ms` of wall time passed: push every observation that far
     * into the past. Unsigned wraparound keeps (now - seen) exact. */
    for (INT i = 0; i < LOOKUP_CACHE_SIZE; i++)
        if (lk_cache[i].valid) lk_cache[i].seen_ms -= ms;
}

/* ------------------------------------------------------------------ */
/* read-k candidates: cache hit first, then HRW order, deduped         */
/* ------------------------------------------------------------------ */

INT lookup_candidates_in(const U1 key[LOOKUP_KEY_LEN],
                         const UB *members, INT n_members,
                         UB out[], INT k)
{
    if (key == NULL || members == NULL || out == NULL) return -1;
    if (n_members <= 0 || k <= 0)                      return -1;
    if (n_members > LOOKUP_MAX_MEMBERS)                return -1;

    /* Full HRW ranking once; the cache only REORDERS within it. */
    UB rank[LOOKUP_MAX_MEMBERS];
    INT nr = lookup_responsible(key, members, n_members, rank, n_members);
    if (nr < 0) return nr;

    INT cnt = 0;

    /* Fresh local observation goes first — but only if the node is in
     * the supplied member view (a dead/foreign node must not shadow a
     * live HRW candidate). */
    UB hit;
    INT have_hit = 0;
    if (lookup_cache_get(key, &hit)) {
        for (INT i = 0; i < n_members; i++) {
            if (members[i] == hit) { have_hit = 1; break; }
        }
        if (have_hit && cnt < k) out[cnt++] = hit;
    }

    /* Then the HRW order, skipping the already-emitted cache hit. */
    for (INT i = 0; i < nr && cnt < k; i++) {
        if (have_hit && rank[i] == hit) continue;
        out[cnt++] = rank[i];
    }
    return cnt;
}

INT lookup_candidates(const U1 key[LOOKUP_KEY_LEN], UB out[], INT k)
{
    UB members[DNODE_MAX];
    INT n = 0;

    for (UB i = 0; i < DNODE_MAX; i++) {
        if (dnode_table[i].state == DNODE_ALIVE) members[n++] = i;
    }
    if (n == 0) return 0;   /* no cluster yet */
    return lookup_candidates_in(key, members, n, out, k);
}

/* ------------------------------------------------------------------ */
/* L1 self-test — cache semantics on top of the unchanged L0 ranking   */
/* ------------------------------------------------------------------ */

static INT ids_equal(const UB *a, const UB *b, INT n)
{
    for (INT i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

INT lookup_l1_self_test(void (*emit)(const char *))
{
    INT fails = 0;

    UB members8[8];
    for (UB i = 0; i < 8; i++) members8[i] = i;

    /* Same fixed key as the L0 vector (bytes 0x00..0x1f) so the HRW
     * baseline is the known cross-ABI ranking. */
    U1 key[LOOKUP_KEY_LEN];
    for (INT i = 0; i < LOOKUP_KEY_LEN; i++) key[i] = (U1)i;

    lookup_cache_clear();   /* re-runnable: start from a cold cache */

    UB hrw[8], cand[8], cand2[8];
    INT nh = lookup_responsible(key, members8, 8, hrw, 8);

    /* (1) cold cache: candidates must equal the pure HRW order. */
    INT nc = lookup_candidates_in(key, members8, 8, cand, 8);
    if (nh == 8 && nc == 8 && ids_equal(cand, hrw, 8)) {
        emit("[hrw-l1] ok  cold cache: candidates == HRW order\r\n");
    } else {
        emit("[hrw-l1] FAIL cold cache must fall through to HRW\r\n"); fails++;
    }

    /* (2) note a local observation on the HRW LAST-ranked node; a fresh
     * hit must short-circuit to candidates[0], rest stays HRW-ordered. */
    UB found_on = hrw[7];               /* worst HRW guess: node 5 */
    lookup_note_found(key, found_on);
    nc = lookup_candidates_in(key, members8, 8, cand, 8);
    {
        INT ok = (nc == 8 && cand[0] == found_on);
        /* remainder must be the HRW order with found_on removed */
        for (INT i = 0; ok && i < 7; i++) ok = (cand[i + 1] == hrw[i]);
        if (ok) {
            emit("[hrw-l1] ok  fresh hit: noted node short-circuits to candidates[0]\r\n");
        } else {
            emit("[hrw-l1] FAIL fresh-hit ordering\r\n"); fails++;
        }
    }

    /* (3) simulated aging past the TTL: the observation expires and
     * candidates fall back to the pure HRW order. */
    lookup_cache_age_for_test((U4)LOOKUP_CACHE_TTL_MS + 1u);
    nc = lookup_candidates_in(key, members8, 8, cand, 8);
    if (nc == 8 && ids_equal(cand, hrw, 8)) {
        emit("[hrw-l1] ok  aged out: stale observation decays back to HRW\r\n");
    } else {
        emit("[hrw-l1] FAIL stale entry must not steer candidates\r\n"); fails++;
    }

    /* (4) determinism: same cache state + members twice -> identical
     * lists, both with and without a fresh hit present. */
    lookup_note_found(key, found_on);
    nc      = lookup_candidates_in(key, members8, 8, cand,  8);
    INT nc2 = lookup_candidates_in(key, members8, 8, cand2, 8);
    if (nc == 8 && nc2 == 8 && ids_equal(cand, cand2, 8)) {
        emit("[hrw-l1] ok  determinism: identical candidate lists on repeat\r\n");
    } else {
        emit("[hrw-l1] FAIL determinism\r\n"); fails++;
    }

    lookup_cache_clear();   /* leave no test residue for real callers */

    if (fails == 0) emit("[hrw-l1] PASS (read-k candidates + resolution cache)\r\n");
    else            emit("[hrw-l1] FAIL\r\n");
    return fails;
}
