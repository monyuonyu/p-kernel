/*
 *  placement.h — SS-5: deterministic expert placement map (NOCENTRAL).
 *
 *  Spec: docs/architecture/special-structure-mind.md §6 (consensus_stance)
 *  + §8 item 6.
 *
 *  The genuinely new question SS-5 answers is "which node holds which
 *  expert?" — WITHOUT a vote, a registrar, a leader, or any new gossip.
 *  The NOCENTRAL-faithful answer (doc §6) is the SAME deterministic-from-
 *  membership trick region.c and HRW lookup already use: place expert e
 *  on the node that wins a RENDEZVOUS HASH (HRW) of (expert-id, alive-
 *  member-set). Every node computes the IDENTICAL map locally from its
 *  own SWIM view (dnode_table[]) — no broadcast, no quorum, no leader —
 *  and the map RE-DERIVES automatically on membership change: a dead
 *  node's experts re-home deterministically to the next HRW winner while
 *  every OTHER expert's owner is UNCHANGED (HRW minimal disruption).
 *
 *  Mechanism (REUSE, do NOT reimplement): the placement is a thin shim
 *  over lookup_responsible() (arch/common/lookup.c) — the EXISTING HRW
 *  primitive. weight(n,k)=sha256(key||node_id), first 8 digest bytes,
 *  higher wins, tie -> lower id. sha256-based => byte-identical ranking
 *  across aarch64 / x86_64 / i686 (the lookup.h cross-ABI contract).
 *
 *  The expert KEY is a stable 32-byte vector derived ONLY from the
 *  expert id (st_expert_key below) so the map is reproducible on any
 *  node, any arch, any language — like lookup.c's fixed test vector.
 *
 *  LP64 / no-float / no-VLA: integer + sha256 only (HRW is sha256-based);
 *  member sets are bounded by LOOKUP_MAX_MEMBERS (>= DNODE_MAX). No long-
 *  width arithmetic feeds the ranking; no stack array is sized by a
 *  runtime dim.
 *
 *  SCOPE (HONEST): this is the placement MAP + cert ONLY. Remote-expert
 *  EXECUTION (firing an expert on its owner node over the mesh) is SS-6
 *  and is DEFERRED. On a single node the alive set is {self}, so every
 *  expert maps to the one node — the map is the FOUNDATION for SS-6, not
 *  cross-node firing itself.
 */
#pragma once
#include "lookup.h"          /* LOOKUP_KEY_LEN, LOOKUP_MAX_MEMBERS, HRW   */

/* Replica fan-out ceiling for the top-r owner list (SS-6 fallback). Kept
 * small + bounded; the primary owner is index 0. */
#define ST_PLACE_RMAX   4

/* Derive the stable 32-byte HRW key for `expert_id`. Pure function of the
 * id (no globals): the SAME expert id yields the SAME key on every node /
 * arch, so the resulting placement map is reproducible by construction.
 * Writes LOOKUP_KEY_LEN bytes into out_key[]. */
void st_expert_key(UB expert_id, U1 out_key[LOOKUP_KEY_LEN]);

/* PURE core (testable with an explicit member set): rank the given alive
 * `members[0..n_members)` by HRW weight for expert `expert_id` and return
 * the WINNER (the owner). Returns the owner node id (>=0) or a negative
 * value on bad arguments / empty member set. No globals, no network. */
INT st_expert_owner_in(UB expert_id, const UB *members, INT n_members);

/* PURE core: write up to `r` top owners (replica order, primary at [0])
 * for `expert_id` over `members[]` into out[]. Returns the count written
 * (= min(r, n_members)) or <0 on bad arguments. SS-6 will use index>=1
 * for safe fallback; SS-5 only needs index 0. */
INT st_expert_owners_in(UB expert_id, const UB *members, INT n_members,
                        UB out[], INT r);

/* LIVE wrappers — the alive SWIM view (dnode_table[], egocentric local
 * view incl. self). These are what the kernel/student calls; the cert
 * drives the _in cores with synthetic member sets. */

/* Owner of `expert_id` over the current DNODE_ALIVE set. Returns the node
 * id, or a negative value if there is no cluster yet (drpc_my_node 0xFF). */
INT st_expert_owner(UB expert_id);

/* TRUE iff this node (drpc_my_node) owns `expert_id` on the current alive
 * set. On a single node every expert is local. FALSE if no cluster yet. */
BOOL st_expert_is_local(UB expert_id);

/* Top-r owner list over the current alive set (SS-6 replica fallback).
 * Returns count written or <0. */
INT st_expert_owners(UB expert_id, UB out[], INT r);

/* SS-5 self-test (shell `place`): drives the PURE cores with synthetic
 * member sets and asserts —
 *   [place-deterministic] every "node" (self-id) derives the IDENTICAL
 *                         owner map from the SAME membership;
 *   [place-rehome]        kill the owner of one expert -> it re-homes to
 *                         the next HRW winner AND every OTHER expert's
 *                         owner is UNCHANGED (minimal disruption); a
 *                         modulo-N placement would reshuffle -> the cert
 *                         distinguishes (falsifiable);
 *   [place-balance]       experts spread across a uniform member set
 *                         (reported, not over-claimed).
 * Prints PASS/FAIL via the supplied puts-style callback. Returns 0 on
 * PASS, non-zero on FAIL. */
INT st_placement_self_test(void (*emit)(const char *));
