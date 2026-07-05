/*
 *  dmoe_bank.h — DMOE-A: the distributed-MoE expert BANK (the growth organ).
 *
 *  Design: scratchpad/distributed_moe_design.md (§1 the bank picture, §2 the
 *  ownerless gating protocol, §3 the real capacity-growth mechanism, §4 the
 *  honest degrade ladder, §7 the [dmoe-*] cert). Companion of scale-wall §6.3
 *  ("replicate the small, shard the big") and survival-network §7 ("distributed
 *  gating = R3").
 *
 *  WHAT THIS IS. SS-6 shares WORK: every node holds every expert; a remote
 *  timeout recomputes LOCALLY, so adding a node adds FLOPs, never capacity. The
 *  BANK inverts exactly that one clause. A bank expert's FFN blocks are SHARDED
 *  (resident only on the expert's HRW owners, placement.c's st_expert_owners),
 *  while its router row is REPLICATED (small: one [D] row per layer, so every
 *  node's gate can SCORE it). An expert you can ROUTE TO but do NOT HOLD, whose
 *  fallback is honest WIDTH degradation (never local recompute), is what turns
 *  "more nodes" into "more distinct experts the fleet can route to".
 *
 *  THE FLOOR IS UNTOUCHED. When the bank is empty every node is exactly today's
 *  student (the [dmoe-bank-empty-identity] gate). The bank is a NEW hosted TU
 *  bolted alongside the arena, never arena surgery — that is what keeps the
 *  m-identical hash family and every existing cert green, and keeps this whole
 *  TU OUT of the bare-metal link (bare metal links student_stub.o; llm/ is
 *  hosted-tier, so the crown .text is structurally unaffected).
 *
 *  CROWN-NEUTRAL. This file, student.c, ss6_live.c are all hosted-tier. It
 *  REUSES placement.c's st_expert_owners_in read-only (does NOT modify it), adds
 *  ZERO K-DDS topics (the manifest is a p-fs object; the fire plane is raw UDP),
 *  and touches no bare-metal TU.
 *
 *  ONE MATH. The per-expert SwiGLU (dmoe_expert_forward_ref) is the SAME
 *  statements as student.c's st_expert_forward_ref (-O1 -ffp-contract=off), so a
 *  remote bank expert's [D] output is bit-identical to a resident one across
 *  aarch64 / x86_64 — the [dmoe-bit-ref] contract. The NEW clause SS-6 never
 *  needed: the VERSION PIN. A sharded, independently-consolidated expert can go
 *  deterministic-but-WRONG on version skew; the pin turns skew into REFUSAL
 *  (degrade), never silent divergence.
 */
#ifndef PKERNEL_LLM_DMOE_BANK_H
#define PKERNEL_LLM_DMOE_BANK_H

#include <stdint.h>
#include <stddef.h>
#include "student.h"        /* st_model, ST_DMOE_FLEET_MAX, the hook types      */
#include "placement.h"      /* UB / INT / st_expert_owners_in (HRW, read-only)  */

/* Baseline replica fan-out and the per-token remote budget (§2.2 / §4.2). */
#define DMOE_R_DEFAULT      2
#define DMOE_TOK_BUDGET_MS  800

/* Bank modes — the cert drives the disease/sabotage arms through these; NORMAL
 * is the only production value. */
#define DMOE_MODE_NORMAL         0   /* resident local; else remote via transport */
#define DMOE_MODE_RESIDENT_ONLY  1   /* DISEASE: never go remote -> foreign shard drops */
#define DMOE_MODE_FORCE_ACCEPT   2   /* SABOTAGE: transport ignores the ver pin    */
#define DMOE_MODE_THEATER_LOCAL  3   /* SABOTAGE: secretly read the local NaN canary */

/* The remote transport (caller-installed, like SS-6's st_remote_expert_fn). The
 * bank's fire() picks an HRW owner and calls this; the cert wires it to an
 * in-process fleet. A production integration WOULD wire it to the SS6L v2 UDP
 * REQ/REP (ss6_live.c serves the conscience floor only today) — a NOT-YET-WIRED
 * follow-up (cross-audit #7: no production caller exists yet; the cert proves
 * the MECHANISM in-process). Returns 0 (out[d] filled with the owner's bit-
 * identical [D] output) or <0
 * (refuse: not resident there, ver skew, absent, or budget). `force_accept`
 * carries the sabotage flag so the [dmoe-version-skew] arm can prove the pin is
 * load-bearing (force-accept a stale blob -> hash diverges from the oracle). */
typedef int (*dmoe_remote_fn)(UB owner, int xid, int layer, const float *fin,
                              int d, uint64_t expect_ver, uint32_t expect_epoch,
                              int force_accept, float *out, void *ctx);

/* An alive predicate over node ids (the SWIM view). Reachability + the remote
 * ladder consult it so a killed owner honestly degrades. Returns non-zero if
 * `node` is currently alive. */
typedef int (*dmoe_alive_fn)(UB node, void *ctx);

/* One bank expert slot as seen by ONE node. `resident` blocks are held only by
 * the expert's HRW owners; every node replicates the router row + the manifest
 * fields (xid/ver/core_epoch/R/salience/holders). */
typedef struct {
    int      xid;                   /* global expert id (>= E_res)               */
    uint64_t ver;                   /* FNV-1a64(blocks || core-epoch) — the pin  */
    uint32_t core_epoch;            /* the core-epoch this expert was trained under */
    int      R;                     /* replica count (manifest)                  */
    uint32_t salience;              /* fire/fallback EWMA slot (DMOE-C; carried)  */
    int      resident;             /* 1 = this node holds the FFN blocks         */
    float   *w1, *w3, *w2;         /* [L][DFF][D],[L][DFF][D],[L][D][DFF] or NULL */
    float   *canary;               /* NaN page backing a non-resident slot (cert) */
    UB       holders[ST_PLACE_RMAX];/* nodes holding ver-current blocks (manifest) */
    int      n_holders;
} dmoe_slot;

typedef struct {
    int       nbank;                /* live bank experts                         */
    int       E_res, L, D, DFF;     /* the floor tier dims (bank experts match)  */
    uint32_t  core_epoch;           /* this node's fleet core-epoch              */
    UB        self_node;            /* this node's SWIM id                        */
    float    *router;               /* [nbank][L][D] replicated router rows       */
    dmoe_slot slot[ST_DMOE_FLEET_MAX];

    /* fleet view (the cert supplies these; a production SWIM + SS6L wiring is a NOT-YET-WIRED follow-up (#7)). */
    UB        members[LOOKUP_MAX_MEMBERS];  /* the HRW alive member set          */
    int       n_members;
    dmoe_alive_fn  alive;   void *alive_ctx;
    dmoe_remote_fn remote;  void *remote_ctx;
    int       mode;                 /* DMOE_MODE_*                               */
} dmoe_bank;

/* Per-expert FFN footprint in BYTES (the "big" that is sharded): (2·DFF·D +
 * D·DFF)·L·sizeof(float). Lets the cert print the budget assertion
 * bytes(all bank experts) > B >= bytes(resident shard) (§3.2). */
size_t dmoe_expert_bytes(const st_model *tier);

/* Sum of resident bank experts' FFN bytes on THIS node (the resident shard). */
size_t dmoe_resident_bytes(const dmoe_bank *b);

/* The version pin: FNV-1a64 over the canonical FFN blocks (all L layers) plus
 * the core-epoch. Every node computes the IDENTICAL ver from identical blocks,
 * so a stale replica's ver differs and the remote-fire refuses (§2.3). */
uint64_t dmoe_expert_ver(const st_model *tier, const float *w1, const float *w3,
                         const float *w2, uint32_t core_epoch);

/* Same pin over explicit dims (no st_model in scope). */
uint64_t dmoe_expert_ver_dims(int L, int D, int DFF, const float *w1,
                              const float *w3, const float *w2, uint32_t core_epoch);

/* Initialise an empty bank sized to the floor tier `tier` on node `self`. Allocs
 * the replicated router table (zeroed). Returns 0 / <0 (OOM/arg). */
int  dmoe_bank_init(dmoe_bank *b, const st_model *tier, UB self);
void dmoe_bank_free(dmoe_bank *b);

/* Add one bank expert. `router_rows` is [L][D] (replicated onto every node).
 * `w1/w3/w2` are the CANONICAL blocks (used to compute the ver on every node);
 * they are COPIED into resident storage iff `self_node` is among `holders`,
 * else the slot is non-resident (blocks absent) and the ver comes from the
 * manifest. `holders` (n_holders, HRW-rank order) is the manifest's owner list.
 * Returns the slot index (>=0) or <0. */
int  dmoe_bank_add(dmoe_bank *b, int xid, const float *router_rows,
                   const float *w1, const float *w3, const float *w2,
                   uint32_t core_epoch, const UB *holders, int n_holders, int R);

/* Back a non-resident slot with a NaN canary page (the [dmoe-nonresident]
 * anti-theater tooth): any secret LOCAL read of a non-resident expert then
 * poisons the logits (NaN) instead of silently returning a plausible-wrong
 * number. No-op on a resident slot. */
void dmoe_bank_set_canary(dmoe_bank *b, int slot);

/* Deliberately corrupt slot `slot`'s manifest ver to `bad_ver` (the
 * [dmoe-version-skew] disease: a replica that hasn't adopted the new blob). The
 * requester's pin then mismatches this owner and the remote-fire refuses. */
void dmoe_bank_force_ver(dmoe_bank *b, int slot, uint64_t bad_ver);

/* The bank analog of st_expert_forward_ref: slot `slot`'s [D] SwiGLU output at
 * `layer` from the RESIDENT blocks (one math, bit-identical to the floor path).
 * Returns 0 (out[d] filled) or <0 if the slot is not resident here — except a
 * canary-backed slot returns NaN (the theater tooth). No mutation. */
int  dmoe_expert_forward_ref(const dmoe_bank *b, int slot, int layer,
                             const float *fin, float *out);

/* Fleet-view setters (the cert supplies these; a production SWIM + SS6L wiring is a NOT-YET-WIRED follow-up (#7)). */
void dmoe_set_members(dmoe_bank *b, const UB *members, int n);
void dmoe_set_alive(dmoe_bank *b, dmoe_alive_fn fn, void *ctx);
void dmoe_set_transport(dmoe_bank *b, dmoe_remote_fn fn, void *ctx);
void dmoe_set_mode(dmoe_bank *b, int mode);

/* THE capacity number (closes degrade.c:155's cosmetic gap): the REAL count of
 * distinct experts the fleet can route to from this node's view —
 *   E_reach = E_res + #{ bank xid : resident here OR >=1 alive holder }.
 * Print this hosted-side; do NOT wire it into degrade.c (bare-metal, would force
 * a crown re-bless for a display change, §3.3). It must visibly DROP when owners
 * die — capacity honesty is part of the observability. */
int  dmoe_experts_reachable(const dmoe_bank *b);

/* Install this bank's score/fire hooks into student.c (st_dmoe_install), so the
 * next st_forward routes the union floor+bank. Pass NULL to clear (floor-only).
 * The bank fires remotely, so call ONLY around generation/eval, never training. */
void dmoe_activate(dmoe_bank *b);
void dmoe_deactivate(void);

#endif /* PKERNEL_LLM_DMOE_BANK_H */
