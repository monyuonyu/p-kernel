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
/* shell dispatcher for `dtr gossip ...` (live multi-node demo)        */
/*   dtr gossip test            — run the in-process self-test         */
/*   dtr gossip solo  [steps]   — measure THIS node's solo shard ceil  */
/*   dtr gossip run   [rounds] [steps] — live no-central gossip learn   */
/*   dtr gossip status          — shard + round info                   */
/* args points just past "gossip". */
void gl_cmd(const UB *args, UW len);
