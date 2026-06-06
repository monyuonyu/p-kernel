/*
 *  lookup.h — decentralized lookup L0: stateless rendezvous hashing (HRW).
 *
 *  Spec: docs/architecture/decentralized-lookup.md §2.1, §6 (L0 row).
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

/* L0 self-test (shell `hrw`): determinism, the cross-ABI known vector
 * (full ranking of members {0..7} for a fixed key — must print the SAME
 * line on every arch), and the §3.1 overlap property (views differing
 * by one node keep intersecting top-2 sets). Prints PASS/FAIL via the
 * supplied puts-style callback. Returns 0 on PASS, non-zero on FAIL. */
INT lookup_self_test(void (*emit)(const char *));
