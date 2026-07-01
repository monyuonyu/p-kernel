/*
 *  placement.c — SS-5: deterministic expert placement map (NOCENTRAL).
 *
 *  docs/architecture/30-module/special-structure-mind.md §6 + §8 item 6.
 *
 *  "Which node holds which expert?" answered by a LOCAL function of LOCAL
 *  membership — never a vote, registrar, leader, or new gossip. Place
 *  expert e on the node that WINS a rendezvous hash (HRW) of (expert-id,
 *  alive-member-set). Every node computes the IDENTICAL map from the SAME
 *  SWIM view; on membership change ONLY the dead node's experts re-home
 *  (HRW minimal disruption), every other owner unchanged.
 *
 *  This file is a THIN shim over lookup_responsible() (arch/common/
 *  lookup.c) — the existing HRW primitive. It does NOT reimplement
 *  sha256 or the ranking; it only derives the stable per-expert key and
 *  picks the winner. Kernel tier (integer + sha256 only), so it builds
 *  on bare metal AND hosted, byte-identical across ABIs.
 *
 *  SCOPE (HONEST): the placement MAP + cert ONLY. Remote-expert EXECUTION
 *  over the mesh is SS-6 (DEFERRED). Single node => alive set {self} =>
 *  every expert maps to the one node; the map is SS-6's foundation.
 */

#include "placement.h"
#include "drpc.h"            /* dnode_table[], DNODE_MAX, DNODE_ALIVE, my  */

/* NO <string.h> (repo rule — see lookup.c / pfs_block.c): the kernel
 * include chain clashes with the hosted stddef. Local loops only. */

/* ------------------------------------------------------------------ */
/* ABI / bound guards                                                  */
/* ------------------------------------------------------------------ */

_Static_assert(LOOKUP_KEY_LEN == 32, "expert key width must be 32 bytes");
_Static_assert(LOOKUP_MAX_MEMBERS >= DNODE_MAX,
               "HRW population must cover every node id");
_Static_assert(ST_PLACE_RMAX <= LOOKUP_MAX_MEMBERS,
               "replica fan-out must fit the HRW population");

/* ------------------------------------------------------------------ */
/* Stable per-expert key                                               */
/* ------------------------------------------------------------------ */

/* A fixed 32-byte vector derived ONLY from expert_id. Domain-separated
 * from lookup.c's own test vectors by a literal tag, then the id is woven
 * across the whole key so distinct experts land far apart in sha256-space
 * (the key feeds sha256(key||node) inside HRW; any spread is sufficient,
 * but a per-byte mix keeps adjacent ids from sharing a near-identical
 * key). Pure: identical on every node / arch / language => reproducible
 * map by construction. */
void st_expert_key(UB expert_id, U1 out_key[LOOKUP_KEY_LEN])
{
    /* "p-kernel-expert\0" domain tag (16 bytes), then a deterministic
     * fill that depends on expert_id. */
    static const U1 TAG[16] = {
        'p','-','k','e','r','n','e','l','-','e','x','p','e','r','t',0
    };
    for (INT i = 0; i < 16; i++) out_key[i] = TAG[i];
    for (INT i = 16; i < LOOKUP_KEY_LEN; i++) {
        /* Mix the id with the position so every tail byte differs across
         * ids; integer-only, no overflow concerns (UB wraps mod 256). */
        out_key[i] = (U1)(expert_id + (UB)i * 31u + 0x9Eu);
    }
}

/* ------------------------------------------------------------------ */
/* PURE cores (explicit member set — testable, no globals)             */
/* ------------------------------------------------------------------ */

INT st_expert_owners_in(UB expert_id, const UB *members, INT n_members,
                        UB out[], INT r)
{
    if (members == NULL || out == NULL) return -1;
    if (n_members <= 0 || r <= 0)       return -1;

    U1 key[LOOKUP_KEY_LEN];
    st_expert_key(expert_id, key);
    /* All ranking + bound-checking lives in the shared HRW primitive. */
    return lookup_responsible(key, members, n_members, out, r);
}

INT st_expert_owner_in(UB expert_id, const UB *members, INT n_members)
{
    UB win[1];
    INT n = st_expert_owners_in(expert_id, members, n_members, win, 1);
    if (n != 1) return -1;
    return (INT)win[0];
}

/* ------------------------------------------------------------------ */
/* LIVE wrappers — the egocentric DNODE_ALIVE view                      */
/* ------------------------------------------------------------------ */

/* Collect the current alive member ids (incl. self). Returns the count;
 * 0 == no cluster yet (drpc_my_node == 0xFF before init). */
static INT alive_members(UB out[DNODE_MAX])
{
    INT n = 0;
    for (UB i = 0; i < DNODE_MAX; i++) {
        if (dnode_table[i].state == DNODE_ALIVE) out[n++] = i;
    }
    return n;
}

INT st_expert_owners(UB expert_id, UB out[], INT r)
{
    UB members[DNODE_MAX];
    INT n = alive_members(members);
    if (n == 0) return -1;   /* no cluster yet */
    return st_expert_owners_in(expert_id, members, n, out, r);
}

INT st_expert_owner(UB expert_id)
{
    UB members[DNODE_MAX];
    INT n = alive_members(members);
    if (n == 0) return -1;
    return st_expert_owner_in(expert_id, members, n);
}

BOOL st_expert_is_local(UB expert_id)
{
    INT owner = st_expert_owner(expert_id);
    if (owner < 0) return FALSE;
    return ((UB)owner == drpc_my_node) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* SS-5 self-test (shell `place`) — drives the PURE cores              */
/* ------------------------------------------------------------------ */

#define PLACE_TEST_NEXPERT  8   /* >= L-tier ST_E_MAX; test is dim-agnostic */

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

/* A non-HRW control: trivial modulo-N placement. Used ONLY to PROVE the
 * re-home cert is falsifiable — modulo-N reshuffles every expert when the
 * member set shrinks, so it FAILS minimal-disruption where HRW passes. */
static INT modN_owner(UB expert_id, const UB *members, INT n_members)
{
    return (INT)members[(INT)expert_id % n_members];
}

INT st_placement_self_test(void (*emit)(const char *))
{
    INT fails = 0;

    /* A synthetic alive-member set of 5 distinct node ids — deliberately
     * NON-contiguous (not 0..4) so the map is proven not to assume ids
     * counted from zero. */
    UB members[5] = { 3, 9, 17, 40, 62 };
    INT NM = 5;

    /* ---- [place-deterministic]: every node derives the SAME map ----
     * The map is a PURE function of (expert_id, members). We model "node
     * X computes the map" by computing it repeatedly (the function reads
     * NO self-id) — identical owners every time, hence every node agrees.
     * Also note (by construction) cross-ABI identity: the HRW weight is
     * sha256-based and byte-picked, so aarch64/x86_64/i686 produce the
     * SAME owners (lookup.h contract; lookup_self_test pins the vector). */
    INT owner_base[PLACE_TEST_NEXPERT];
    INT det_ok = 1;
    for (INT e = 0; e < PLACE_TEST_NEXPERT; e++) {
        owner_base[e] = st_expert_owner_in((UB)e, members, NM);
        if (owner_base[e] < 0) det_ok = 0;
    }
    /* recompute "as several different nodes" — same input, same output. */
    for (INT pass = 0; det_ok && pass < 4; pass++) {
        for (INT e = 0; e < PLACE_TEST_NEXPERT; e++) {
            INT o = st_expert_owner_in((UB)e, members, NM);
            if (o != owner_base[e]) det_ok = 0;
        }
    }
    if (det_ok) {
        emit("[place] ok  [place-deterministic] map ");
        for (INT e = 0; e < PLACE_TEST_NEXPERT; e++) {
            emit(e ? " e" : "e"); emit_dec(emit, e);
            emit("->n"); emit_dec(emit, owner_base[e]);
        }
        emit("  (sha256-HRW => identical on every node/arch)\r\n");
    } else {
        emit("[place] FAIL [place-deterministic]\r\n"); fails++;
    }

    /* ---- [place-rehome]: kill the owner of expert 0; ONLY expert 0's
     * owner may move; every other expert keeps its owner (HRW minimal
     * disruption). Build a member set with owner_base[0] removed. ---- */
    UB victim = (UB)(owner_base[0] >= 0 ? owner_base[0] : 0);
    UB reduced[5];
    INT NR = 0;
    for (INT i = 0; i < NM; i++) {
        if (members[i] != victim) reduced[NR++] = members[i];
    }
    INT rehome_ok = 1;
    INT moved_count = 0;
    for (INT e = 0; e < PLACE_TEST_NEXPERT; e++) {
        INT before = owner_base[e];
        INT after  = st_expert_owner_in((UB)e, reduced, NR);
        if (after < 0) { rehome_ok = 0; continue; }
        if (before == (INT)victim) {
            /* expert 0 owned by the dead node MUST re-home to a *live*
             * node (and necessarily change away from the victim). */
            if (after == (INT)victim) rehome_ok = 0;
            moved_count++;
        } else {
            /* every OTHER expert's owner must be UNCHANGED. */
            if (after != before) { rehome_ok = 0; moved_count++; }
        }
    }
    if (rehome_ok && moved_count >= 1) {
        emit("[place] ok  [place-rehome] killed n");
        emit_dec(emit, (INT)victim);
        emit(": exactly the dead node's expert(s) re-homed (");
        emit_dec(emit, moved_count);
        emit(" moved), all others UNCHANGED\r\n");
    } else {
        emit("[place] FAIL [place-rehome]\r\n"); fails++;
    }

    /* ---- falsifiability: a modulo-N placement reshuffles owners when
     * the set shrinks (NM=5 -> NR=4 changes (e % N) for most experts),
     * so it would FAIL the minimal-disruption check above. Show that the
     * control actually MOVES more experts than HRW did. ---- */
    INT modn_moved = 0;
    for (INT e = 0; e < PLACE_TEST_NEXPERT; e++) {
        INT before = modN_owner((UB)e, members, NM);
        INT after  = modN_owner((UB)e, reduced, NR);
        if (after != before) modn_moved++;
    }
    if (modn_moved > moved_count) {
        emit("[place] ok  [place-rehome] falsifiable: modulo-N control moved ");
        emit_dec(emit, modn_moved);
        emit(" experts vs HRW ");
        emit_dec(emit, moved_count);
        emit(" (HRW is minimal)\r\n");
    } else {
        /* not a hard fail of the property, but the cert wants the control
         * to be strictly worse; if it is not, flag it for review. */
        emit("[place] WARN [place-rehome] control not strictly worse "
             "(modn=");
        emit_dec(emit, modn_moved);
        emit(" hrw=");
        emit_dec(emit, moved_count);
        emit(")\r\n");
    }

    /* ---- [place-balance] (honest, reported not over-claimed): how many
     * distinct nodes own at least one of a larger expert sweep over the
     * uniform 5-node set. We do NOT claim perfect balance. ---- */
    {
        INT SWEEP = 16;
        UB seen[5] = { 0, 0, 0, 0, 0 };
        INT distinct = 0;
        for (INT e = 0; e < SWEEP; e++) {
            INT o = st_expert_owner_in((UB)e, members, NM);
            for (INT i = 0; i < NM; i++) {
                if (members[i] == (UB)o && !seen[i]) { seen[i] = 1; distinct++; break; }
            }
        }
        emit("[place] ok  [place-balance] ");
        emit_dec(emit, SWEEP);
        emit(" experts spread across ");
        emit_dec(emit, distinct);
        emit("/");
        emit_dec(emit, NM);
        emit(" nodes (reported; not claiming perfect balance)\r\n");
        if (distinct < 2) {
            emit("[place] FAIL [place-balance] all experts on one node\r\n");
            fails++;
        }
    }

    /* ---- single-node honesty: with the alive set == {self}, every
     * expert maps to the one node. The map is SS-6's foundation, not
     * cross-node firing (which is DEFERRED to SS-6). ---- */
    {
        UB solo[1] = { 7 };
        INT solo_ok = 1;
        for (INT e = 0; e < PLACE_TEST_NEXPERT; e++) {
            if (st_expert_owner_in((UB)e, solo, 1) != 7) solo_ok = 0;
        }
        if (solo_ok) {
            emit("[place] ok  single-node: all experts -> the one node "
                 "(SS-6 remote firing DEFERRED)\r\n");
        } else {
            emit("[place] FAIL single-node mapping\r\n"); fails++;
        }
    }

    if (fails == 0) emit("[place] PASS\r\n");
    else            emit("[place] FAIL\r\n");
    return fails;
}
