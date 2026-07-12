/*
 *  dlb.h — DLB: test-time DELIBERATION (the missing D3 "熟慮の中身").
 *
 *  depth_iq_path_design.md §3: depth from COMPUTE, not weights. The reflex layer
 *  returns a fast draft (間に合う); the deliberation layer revises slow (正しい)
 *  by SEARCH x VERIFY (§3.1) — sample K reasoning paths, score each with a
 *  verifier better than chance, select the best. Both factors are load-bearing:
 *  best-of-N with a random verifier is a random pick; self-consistency of an
 *  incoherent generator is the mode of noise. The [depth-deliberation] cert
 *  (§6.2) stubs each factor independently (STUB-SEARCH K=1, STUB-VERIFY random)
 *  and BOTH must go RED, proving the decomposition.
 *
 *  HOSTED-TIER ONLY (built alongside student.c / dev_capacity.c into boot/linux
 *  + boot/linux_x86_64; NOT on bare metal — student_stub.o resolves the ABI
 *  there). dlb.c calls ONLY the public student.h API (st_generate, st_span_ce,
 *  st_forward/backward/adam_step); it does NOT touch student.c internals, moe.c,
 *  dmn.c, or the R3 crown. So the bare-metal .text is UNTOUCHED and there is NO
 *  crown re-bless for v1 (design §7).
 *
 *  Determinism / one-math (wave-49, -O1 -ffp-contract=off): the whole loop is a
 *  pure function of (weights, query, budget) — the K candidate seeds are
 *  H(query || i), so a deliberated answer can be re-derived and audited by any
 *  node, byte-identical across arches. No new transcendental math (conf reuses
 *  st_span_ce; verify is a caller-supplied procedural checker).
 *
 *  v1 lives in V-EXACT domains (§3.3): tasks whose correctness is checkable in
 *  code (arithmetic chains, format/hash-chain/capacity invariants). There the
 *  verify factor is FREE and PERFECT (AUC=1), so deliberation pays even at the
 *  tiny byte-baby scale — the same reason o1-style training bootstrapped on
 *  math/code. V-self (the model as its own critic) and V-fleet (the ensemble as
 *  verifier) are admitted only after they measurably beat chance (§3.3) and are
 *  out of scope for this TU (V-fleet == the scaling-law ensemble, called not
 *  duplicated; V-self deferred until it prints AUC > 0.5).
 */
#ifndef PKERNEL_LLM_DLB_H
#define PKERNEL_LLM_DLB_H

#include <stdint.h>
#include <stddef.h>
#include "student.h"

/* A VERIFIER (§3.3): score candidate answer `cand` (cn bytes) for `query` (qn
 * bytes). Higher score = better. For V-exact it is 1.0 (accept) / 0.0 (reject),
 * AUC = 1 by construction. `vctx` carries the task's procedural oracle. The DLB
 * loop treats this as a black box, so the cert can pass the REAL V-exact checker
 * (CURE) or a random-scoring stub (STUB-VERIFY) through the IDENTICAL code path
 * (feedback_cert_isolation_shared_path). */
typedef float (*dlb_verify_fn)(const uint8_t *query, int qn,
                               const uint8_t *cand, int cn, void *vctx);

/* The deliberation BUDGET (§3.2). In production K/max_gen are a function of the
 * interoception stress bus S_n, degrade level, and battery — a stressed / SOLO
 * node answers single-shot (K=1) and SAYS so (the degrade-honesty discipline).
 * v1 exposes the budget directly; wiring the live S_n read is kernel-tier and
 * deferred (design §3.2). theta_easy is the (1-p_max) cheapness gate: a draft
 * whose confidence already clears it returns single-shot — the mind thinks hard
 * ONLY when unsure, exactly the signal retrieval.h gates on. */
typedef struct {
    int   K;          /* SEARCH width: # candidate paths (>=1; 1 == single-shot) */
    int   max_gen;    /* answer bytes sampled per candidate                      */
    float temp;       /* sampling temperature (path diversity)                   */
    int   top_k;      /* nucleus top-k                                           */
    float theta_easy; /* draft-conf cheapness gate; <=0 disables the early-out   */
} dlb_budget;

/* Honest label of how much thought produced the answer (§3.2 step 8). */
typedef struct {
    int   k_used;      /* candidates actually generated (1 == single-shot/stub)  */
    int   flipped;     /* 1 iff the deliberated pick beat the draft on verify    */
    float draft_score; /* verifier score of the single-shot draft (candidate 0)  */
    float best_score;  /* verifier score of the returned answer                  */
    float draft_conf;  /* exp(-answer CE) of the draft — the (1-p_max) trigger    */
} dlb_result;

#define DLB_KMAX    64   /* hard ceiling on search width (bounds compute/stack)   */
#define DLB_GEN_MAX 96   /* answer-scratch bound (== student.c's internal ST_GEN_CAP
                          * clamp; st_generate re-clamps, so this only sizes stack) */

/* The DLB loop (§3.2). Draft fast (candidate 0, seed H(query,0)); if the draft
 * clears theta_easy or K<=1, return it. Else SEARCH K-1 more seeded candidates,
 * VERIFY each, and return the argmax (ties -> lowest index, deterministic). The
 * chosen answer is written into out[max_out] and its length returned (>=0), or a
 * negative ST_E_* on bad args / OOM. `info` (may be NULL) gets the honest label.
 *
 * The loop mutates NOTHING durable: it only calls st_generate (seeded, KV-cached)
 * and st_span_ce (pure forward). Same (m, query, budget, verify) -> same answer
 * on every arch. */
int dlb_answer(st_model *m, const uint8_t *query, int qn,
               uint8_t *out, int max_out,
               const dlb_budget *b, dlb_verify_fn verify, void *vctx,
               dlb_result *info);

/* ---------------------------------------------------------------------------
 * V-EXACT arithmetic gate + read-only oracle (Wave-D2 LIVE feeder). Three PURE
 * INTEGER helpers that let a production caller (student_chat_generate) close the
 * compounding loop end-to-end: recognise a DELIBERATE arithmetic question, VERIFY
 * a candidate answer against ground truth (§3.3), and TRIM the winning answer to
 * its digit run before enqueue. Production AND the depth_compound cert call the
 * SAME functions in the SAME TU (cert-isolation shared-path discipline,
 * feedback_cert_isolation_shared_path). Integer-only -> one-math deterministic on
 * every arch (no new transcendental math).
 * ------------------------------------------------------------------------- */

/* Recognise the arithmetic form  ^\s*(-?\d{1,4})\s*([+\-*])\s*(-?\d{1,4})\s*=\s*$
 * (MUST end in '='; |A|,|B| <= 9999). Returns 1 and writes *expect_out = A op B
 * iff `q` (qn bytes) matches AND qn <= DLB_TRACE_MAX-8 (so the winning trace
 * query||answer, answer <= DLB_CHAT_ANSGEN bytes, fits the compounding ring);
 * else 0 (and *expect_out is untouched). Integer-only; no allocation, no VLA. */
int dlb_gate_vexact(const uint8_t *q, int qn, long *expect_out);

/* A dlb_verify_fn (§3.3) for the arithmetic gate: vctx is the `long *expect`
 * dlb_gate_vexact produced. Parses `cand`'s leading optional-space + optional
 * sign + digit run into an integer and returns 1.0f iff it equals *expect, else
 * 0.0f. READ-ONLY ORACLE: it never writes cand/out, so it structurally cannot
 * inject the answer (mirrors depth_compound_test.c's vexact_verify). */
float dlb_vexact_verify(const uint8_t *query, int qn,
                        const uint8_t *cand, int cn, void *vctx);

/* Length (from cand[0]) of the accepted leading signed-digit run — leading
 * whitespace + optional sign + digits — or 0 if `cand` has no leading number.
 * Used to TRIM a winning answer to its number before dlb_compound_enqueue. */
int dlb_vexact_anslen(const uint8_t *cand, int cn);

/* ---------------------------------------------------------------------------
 * The COMPOUNDING loop (§3.4) — deliberation x DMN (search-distill), the ONE
 * depth mechanism the fleet owns end-to-end (the AlphaZero crack, §4.3).
 *
 * At answer time DLB occasionally finds a verified-correct answer the draft
 * missed (a `flipped` result). The (query, winning trace, verified) tuple is
 * queued here — a bounded ring, the hosted analogue of the salience-replay ring
 * (wave-23: here a hard-question-solved earns salience). At sleep time the DMN's
 * consolidation distills the ring into weights: what needed K samples yesterday
 * needs 1 tomorrow (test-time compute AMORTIZED into weight-resident depth).
 *
 * THE HARD HONESTY GATE (the validator trap, feedback_validator_and_learner_traps):
 * in v1 production distills ONLY V-exact-verified traces (require_verified=1).
 * Distilling V-self-approved / unverified traces amortizes the critic's noise
 * into confident wrongness — the learner optimizing a broken objective. The
 * cert's Arm D drives require_verified=0 on an UNVERIFIED ring and proves
 * held-out depth DEGRADES vs the verified-only loop; that arm going red-on-
 * disease is what licenses the loop at all.
 *
 * PRODUCTION ROUTE (design §7, deferred to keep v1 crown-neutral + deterministic):
 * the live DMN sleep tick would call dlb_compound_distill(g_student, ...,
 * require_verified=1) through the EXISTING student_dmn_consolidate seam (dmn.c is
 * NOT hooked directly — it already calls that hosted symbol). v1 ships the organ
 * + its hard gate + the cert; the live sleep wiring is a later, flagged wave
 * (like the V-fleet [live] leg deferred to the ThinkPad runner).
 * ------------------------------------------------------------------------- */
#define DLB_RING_MAX   64    /* bounded trace ring (salience-replay analogue)     */
#define DLB_TRACE_MAX  32    /* max bytes of one (query+answer) trace             */

/* Per-trace distill BUDGET (Wave-D1). A live DMN feeder (Wave-D2) distills the
 * ring on EVERY sleep tick; without a budget the same verified trace would be
 * re-distilled forever (overfit + permanent per-tick cost) and the ring would
 * saturate. Each trace may receive at most this many total distill passes across
 * all ticks; once spent it is SKIPPED at distill time and reaped by
 * dlb_compound_gc(). Set == the cert's single-shot distill amount so the
 * tick-split live distill accumulates to the SAME total the cert proves. */
#define DLB_TRACE_ROUNDS_MAX 30

/* Clear the compounding ring (start of a session / cert arm). */
void dlb_compound_reset(void);

/* Queue a deliberation trace: the query (qn bytes) followed by its winning
 * answer (an bytes), tagged `verified` (1 iff a V-exact checker accepted it).
 * Refuses (returns 0) if the ring is full or the trace exceeds DLB_TRACE_MAX;
 * returns 1 on enqueue. The HARD GATE is applied at DISTILL time, not here, so
 * the cert can populate an unverified ring and prove distilling it degrades. */
int  dlb_compound_enqueue(const uint8_t *query, int qn,
                          const uint8_t *ans, int an, int verified);

/* Count queued traces matching the gate (require_verified=1 counts only
 * verified ones — what production would distill). Pure read. */
int  dlb_compound_pending(int require_verified);

/* Distill the queued traces into `m` (rounds passes of the SAME zero_grad ->
 * st_forward -> st_backward -> st_adam_step triple the DMN sleep uses, in a
 * FIXED canonical ring order so it is one-math deterministic). When
 * require_verified != 0 ONLY verified traces are distilled (the v1 hard gate);
 * =0 distills ALL (the Arm-D disease path, cert-only). Returns the number of
 * distinct traces distilled, or negative ST_E_* on OOM/bad args. Mutates m->w /
 * Adam state via the public API only. */
int  dlb_compound_distill(st_model *m, int rounds, float lr, int require_verified);

/* Garbage-collect the ring (Wave-D1): compact-remove every trace whose distill
 * budget is spent (rounds_done >= DLB_TRACE_ROUNDS_MAX) and any leftover
 * unverified trace, front-packing the survivors so their canonical distill order
 * (ascending index) is preserved — one-math determinism intact. Unlike
 * dlb_compound_reset() this KEEPS freshly-enqueued not-yet-distilled verified
 * traces (it keys on rounds_done, not the whole ring), so a live feeder can call
 * it every sleep tick without dropping just-arrived work. Intended caller: the
 * DMN task only (Wave-D2 wires it after each dlb_compound_distill). */
void dlb_compound_gc(void);

#endif /* PKERNEL_LLM_DLB_H */
