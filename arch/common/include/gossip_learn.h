/*
 *  gossip_learn.h — G22 / survival-network.md §8 §9: decentralized
 *  COLLECTIVE learning. "全体が未来を強くする" made real.
 *
 *  The standing audits (philosophy-gap-audit-6 §4, G22) found the
 *  network does NOT learn collectively: a node can train locally
 *  (dtr.c analytic backprop) and SAVE its weights to p-fs, and another
 *  node can LOAD them — but that is COPY, not learning. Cross-node
 *  weight update = 0; fedlearn's FedAvg was E_NOSPT; the only
 *  "deliberation" was one scalar (learned_conserve).
 *
 *  This module makes the second band of §8 — the slow "deliberation"
 *  band where the WHOLE swarm strengthens the future — actually run:
 *
 *    - N nodes each train on a DISJOINT shard of the data (leave-one-
 *      class-out): NO single node sees the whole task, so NO node can
 *      learn it alone. This is the crux — if every node had all the
 *      data, "collective learning" would be unprovable.
 *    - Each node periodically GOSSIPS its current model (the full 635-
 *      param transformer weight body, not just a bias) and MERGES peers'
 *      models into its own by averaging (decentralized SGD / FedAvg with
 *      a short local period). Over rounds EVERY node's accuracy on the
 *      FULL task rises ABOVE its solo shard-only ceiling — the swarm
 *      learned what no node could alone.
 *    - NO central aggregator (§7): there is no server that collects
 *      everyone. Each node reads only gossiped peer models and averages
 *      locally — peer-symmetric. gl_merge() takes no aggregator index;
 *      averaging is commutative, so every node computes the same merge
 *      from the models it has.
 *    - Slow band (§8): the merge runs on the deliberation cadence
 *      (~seconds via tk_dly_tsk), not the reflex tick.
 *    - Survives node death (§3): a killed node's last model stays in
 *      p-fs (P1-replicated); survivors keep averaging and improving; a
 *      rejoining node catches up via gossip.
 *
 *  Transport: p-fs. Each node saves its current model under a per-node
 *  named ref "dtr/model/<n>" (pfs_dag_save); neighbours load peers' refs
 *  (pfs_dag_read) and average. Reuses existing P1/P2 infra — kdds/pfs
 *  PUBLIC APIs only, read-only with respect to other domains.
 *
 *  arch/common discipline: no host libc, fixed-width LP64 types on the
 *  wire, static (not task-stack) buffers, _Static_assert on sizes.
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* generic gossiped model blob (header + n little-endian float32)      */
/* ------------------------------------------------------------------ */

#define GL_BLOB_MAGIC  0x4C535347UL   /* "GSSL" LE */

typedef struct {
    UW magic;                       /* GL_BLOB_MAGIC                   */
    UW n;                           /* float count that follows        */
} __attribute__((packed)) GL_BLOB_HDR;     /* 8 bytes */

/* ------------------------------------------------------------------ */
/* generic decentralized primitives (shared with fedlearn.c)           */
/* ------------------------------------------------------------------ */

/* acc[i] += w[i] for i<n — fold one more model into a running sum. */
void gl_accumulate(float *acc, const float *w, UW n);

/* w[i] *= s for i<n — used to turn a running sum into an average. */
void gl_scale(float *w, float s, UW n);

/* out[i] = mean over the `count` models in models[][] (count>=1).
 * NO aggregator index: the inputs are a flat, order-independent set of
 * peer models; this is exactly what makes the merge no-central. */
void gl_merge(float *out, const float *const *models, UW count, UW n);

/* ------------------------------------------------------------------ */
/* LM-11 / Path W² — per-parameter WEIGHTED merge (living-mind Part XII) */
/*                                                                     */
/* gl_merge (above) is the PLAIN UNWEIGHTED mean — it halves every      */
/* contributor's pull equally. The weighted siblings below generalize   */
/* it to a per-parameter weighted mean WITHOUT forking gl_merge (which  */
/* keeps driving LM-10): each parameter i is averaged weighted by how   */
/* much it MATTERS to each model (the diagonal Fisher Fₖ[i], XII.2 W2). */
/* gl_merge stays byte-identical; these are ADDITIVE.                   */
/* ------------------------------------------------------------------ */

/* Weighted accumulate: acc[i] += wt[i]*w[i] AND wsum[i] += wt[i], for
 * i<n. The per-parameter sibling of gl_accumulate — fold one more model
 * into a running WEIGHTED sum + the per-param weight running sum. */
void gl_accumulate_w(float *acc, float *wsum,
                     const float *w, const float *wt, UW n);

/* Per-parameter Fisher-weighted mean (XII.2 W2):
 *   out[i] = (Σₖ wtₖ[i]·wₖ[i]) / (Σₖ wtₖ[i] + eps)
 * where weights[k] is model k's per-parameter weight vector (e.g. its
 * diagonal Fisher). The eps floor means a parameter NEITHER model
 * trained (Fₖ[i]≈0 for all k) falls back toward the plain mean of the
 * shared pretrained backbone — the correct fallback (XII.2). Still
 * order-independent (a per-param weighted SUM, [wmerge-nocentral]).
 * NO aggregator index: a flat, order-independent set of (model,weight). */
void gl_merge_w(float *out, const float *const *models,
                const float *const *weights, UW count, float eps, UW n);

/* Publish `w` (n floats) to the p-fs named ref `ref` (a versioned,
 * region-replicated object). Returns 0 on success, <0 on pfs error.
 * Shell-task context only (shares pfs_dag scratch). */
INT  gl_pfs_publish(const char *ref, UW reflen, const float *w, UW n);

/* Fetch the head version of `ref` into `w` (expects exactly n floats).
 * Returns 0 on success, -1 if not present yet / bad blob. A P1 WANT is
 * issued by pfs_dag_read on miss, so a later round usually succeeds. */
INT  gl_pfs_fetch(const char *ref, UW reflen, float *w, UW n);

/* ------------------------------------------------------------------ */
/* in-process self-test ([g22-shard-solo] / [g22-gossip-learn] /       */
/* [g22-no-central]) — greppable, wired to CI                          */
/* ------------------------------------------------------------------ */

void gl_self_test(void);

/* ------------------------------------------------------------------ */
/* G38 — THINKING CHANGES GUARDING (survival-network §8 §9 two-layer    */
/* couple). In-process property tests, greppable, wired to CI:         */
/*   [g38-confidence-live]          — real max-softmax gates the reflex */
/*                                    (low-conf stays quiet; learned-   */
/*                                    confident threat fires; not 0xFF) */
/*   [g38-learning-improves-guarding]— a COLLECTIVELY-LEARNED model     */
/*                                    guards measurably better than an  */
/*                                    UNLEARNED one (numbers printed)    */
/*   [g38-guard-feeds-learning]     — the reflex's per-class threat     */
/*                                    experience prioritizes the learn  */
/* Run from `dtr gossip g38`. */
void gl_g38_test(void);

/* ------------------------------------------------------------------ */
/* G23 — node ceiling > 32 (gap-ledger row G23). Proves the >32 code    */
/* path works for real: a 40-model core gl_merge() AND a 40-entry live  */
/* membership fold over the REAL dnode_table, neither truncating at 32. */
/*   [g23-ceiling] — run from `dtr gossip ceiling`.                     */
/* ------------------------------------------------------------------ */
void gl_g23_test(void);

/* ------------------------------------------------------------------ */
/* shell dispatcher for `dtr gossip ...` (live multi-node demo)        */
/*   dtr gossip test            — run the in-process self-test         */
/*   dtr gossip solo  [steps]   — measure THIS node's solo shard ceil  */
/*   dtr gossip run   [rounds] [steps] — live no-central gossip learn   */
/*   dtr gossip status          — shard + round info                   */
/* args points just past "gossip". */
void gl_cmd(const UB *args, UW len);
