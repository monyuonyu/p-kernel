/*
 *  lookup.h — decentralized lookup L0/L1: stateless rendezvous hashing
 *  (HRW) + a local resolution cache and read-k candidate lists.
 *
 *  Spec: docs/architecture/20-architecture/decentralized-lookup.md §2, §3.2, §6 (L0+L1).
 *
 *  L0 is the pure "responsible(k, r)" calculation: given a 32-byte key
 *  and a member view, rank the members by HRW weight and return the
 *  top-r node ids. NO ring, NO state, NO network, NO central index —
 *  every node computes the same answer from the same member view
 *  (the doc's core invariant). When views differ by one node, the
 *  top-r sets still intersect (§3.1) — that is what makes HRW the
 *  right primitive over an eventually-consistent membership.
 *
 *  Scoring (cross-ABI contract):
 *    weight(n, k) = sha256(key[32] || node_id[1])
 *  compared as the digest's first 8 bytes read big-endian into two U4
 *  words (hi = d[0..3], lo = d[4..7]), higher wins; ties (impossible in
 *  practice — they'd need a 64-bit sha256 prefix collision) break to the
 *  LOWER node id so the order stays total and deterministic.
 *
 *  LP64 / ABI trap (memory: feedback_lp64_typedef_trap): scores are
 *  derived ONLY from digest bytes via U1/U4 ops — never from long-width
 *  arithmetic — so the ranking is byte-identical across aarch64 /
 *  x86_64 / i686. _Static_asserts in lookup.c pin the widths.
 */

#pragma once
#include "kernel.h"

#define LOOKUP_KEY_LEN      32   /* key width == sha256 digest width      */
#define LOOKUP_MAX_MEMBERS  64   /* cap on one HRW population (>= DNODE_MAX) */

/* --- L1: resolution cache (doc §2.3, §6 L1 row) ----------------------
 * A small fixed table of LOCAL OBSERVATIONS: "key k was actually FOUND
 * on node n" (fed by callers via lookup_note_found() after a successful
 * fetch). World-table semantics, deliberately: an entry is a memory of
 * a past success, never a truth — it ages out (staleness like world.c's
 * WORLD_STALE_MS) and a wrong/stale entry merely costs one extra hop
 * before the HRW guess takes over again. No gossip, no sync, no central
 * anything: every node's cache holds only what that node itself saw. */
#define LOOKUP_CACHE_SIZE    16     /* entries (16 * 40 B = 640 B static) */
#define LOOKUP_CACHE_TTL_MS  9000   /* freshness window = WORLD_STALE_MS  */

/* Rank `members[0..n_members)` (distinct node ids) by HRW weight for
 * `key` and write the top-r ids into out[] in descending-weight order.
 * Pure function: reads no globals, touches no network.
 * Returns the number of ids written (= min(r, n_members)), or a
 * negative value on bad arguments. */
INT lookup_responsible(const U1 key[LOOKUP_KEY_LEN],
                       const UB *members, INT n_members,
                       UB out[], INT r);

/* Convenience: members = current region view (region.h, egocentric
 * local view — that is the documented contract). Returns count or <0. */
INT lookup_responsible_region(const U1 key[LOOKUP_KEY_LEN],
                              UB out[], INT r);

/* Convenience: members = all DNODE_ALIVE nodes in dnode_table[]
 * (egocentric local view; includes self). Returns count or <0. */
INT lookup_responsible_alive(const U1 key[LOOKUP_KEY_LEN],
                             UB out[], INT r);

/* ------------------------------------------------------------------ */
/* L1 — resolution cache + read-k candidates (doc §2.3, §3.2, §6 L1)   */
/* ------------------------------------------------------------------ */

/* Record a locally observed resolution: "key was actually FOUND on
 * `node`". Callers (p-fs / gating / world, at L2) feed this after every
 * successful fetch. Refreshes an existing entry for the same key;
 * otherwise evicts (in order of preference) a free slot, the oldest
 * expired slot, or the oldest live slot. Purely local state. */
void lookup_note_found(const U1 key[LOOKUP_KEY_LEN], UB node);

/* Fresh-cache probe: if `key` has a non-expired entry, write its node
 * to *node and return 1; else return 0. Never falls back to HRW —
 * that composition is lookup_candidates()'s job. */
INT  lookup_cache_get(const U1 key[LOOKUP_KEY_LEN], UB *node);

/* Drop every cache entry (tests; callers may use it on net reset). */
void lookup_cache_clear(void);

/* TEST-ONLY: age every entry by `ms` (simulated clock advance) so the
 * self-test can exercise expiry without sleeping LOOKUP_CACHE_TTL_MS. */
void lookup_cache_age_for_test(U4 ms);

/* read-k candidate list (the API gating/p-fs will call at L2): merged,
 * deduped, deterministic order —
 *   [0]   fresh cache hit for `key`, IF its node is in members[]
 *         (a locally observed fact short-circuits the HRW guess);
 *   [...] HRW responsible order over members[], skipping the cache
 *         hit if already emitted.
 * Pure in (cache state, members): no network, no global truth — the
 * cache reorders candidates only via locally observed facts, so the
 * underlying HRW ranking stays byte-identical across ABIs.
 * Returns the number of ids written (<= k), or <0 on bad arguments. */
INT lookup_candidates_in(const U1 key[LOOKUP_KEY_LEN],
                         const UB *members, INT n_members,
                         UB out[], INT k);

/* Convenience: members = all DNODE_ALIVE nodes in dnode_table[]
 * (egocentric local view). Returns count (0 if no cluster) or <0. */
INT lookup_candidates(const U1 key[LOOKUP_KEY_LEN], UB out[], INT k);

/* L0 self-test (shell `hrw`): determinism, the cross-ABI known vector
 * (full ranking of members {0..7} for a fixed key — must print the SAME
 * line on every arch), and the §3.1 overlap property (views differing
 * by one node keep intersecting top-2 sets). Prints PASS/FAIL via the
 * supplied puts-style callback. Returns 0 on PASS, non-zero on FAIL. */
INT lookup_self_test(void (*emit)(const char *));

/* L1 self-test (shell `hrw`, runs after the L0 test): ① cache miss ->
 * candidates == HRW order; ② fresh hit -> candidates[0] == noted node;
 * ③ simulated aging -> falls back to HRW; ④ determinism. Uses only the
 * pure _in variant + the test-only ager, so it passes on a single node
 * with no cluster. Returns 0 on PASS, non-zero on FAIL. */
INT lookup_l1_self_test(void (*emit)(const char *));
