/*
 *  gossip_learn.c — G22 / §8 §9 decentralized COLLECTIVE learning.
 *
 *  See gossip_learn.h for the why. The crux, in one sentence: N nodes
 *  each see only PART of the task (disjoint, leave-one-class-out shards),
 *  yet by periodically averaging each other's full weight bodies (no
 *  central aggregator) every node ends up ABOVE the best a node can do
 *  on its shard alone. The swarm learns what no node could.
 *
 *  Built on dtr.c's REAL 635-param transformer + analytic backprop
 *  (dtr_train_batch / dtr_eval_batch / dtr_weights_get/set /
 *  dtr_reinit_weights — all public). We never touch dtr's math; we
 *  drive it through its public API, swapping weight sets in and out to
 *  simulate N nodes in-process, and gossiping them over p-fs live.
 */

#include "gossip_learn.h"
#include "dtr.h"
#include "pfs_block.h"
#include "pfs_dag.h"
#include "drpc.h"
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* output helpers (sio frame channel, like dtr_train.c / protect.c)    */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void gp(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void gpd(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { gp("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    gp(&buf[i]);
}

/* xx.x percent / loss */
static void gpf1(float f)
{
    if (f < 0.0f) { gp("-"); f = -f; }
    UW whole = (UW)f;
    UW frac  = (UW)((f - (float)whole) * 10.0f + 0.5f);
    if (frac >= 10) { whole++; frac = 0; }
    gpd(whole); gp("."); gpd(frac);
}

/* ------------------------------------------------------------------ */
/* generic decentralized primitives                                    */
/* ------------------------------------------------------------------ */

void gl_accumulate(float *acc, const float *w, UW n)
{
    for (UW i = 0; i < n; i++) acc[i] += w[i];
}

void gl_scale(float *w, float s, UW n)
{
    for (UW i = 0; i < n; i++) w[i] *= s;
}

void gl_merge(float *out, const float *const *models, UW count, UW n)
{
    if (count == 0) return;
    for (UW i = 0; i < n; i++) out[i] = 0.0f;
    for (UW k = 0; k < count; k++) gl_accumulate(out, models[k], n);
    gl_scale(out, 1.0f / (float)count, n);
}

/* ------------------------------------------------------------------ */
/* p-fs transport (public pfs_dag API only)                            */
/* ------------------------------------------------------------------ */

/* one shared scratch blob: 8-byte header + N float32. Static, never on
 * the task stack (feedback_hosted_relay_stack_overflow). Driven from
 * the shell task only, so no concurrent use. */
#define GL_MAXFLOATS  DTR_WEIGHT_FLOATS
static struct __attribute__((packed, aligned(4))) {
    GL_BLOB_HDR h;
    float       w[GL_MAXFLOATS];
} gl_blob;

_Static_assert(sizeof(GL_BLOB_HDR) == 8, "GL_BLOB_HDR must be 8 bytes");
_Static_assert(sizeof(float) == 4, "float must be IEEE754 binary32");
_Static_assert(sizeof(gl_blob) == 8 + GL_MAXFLOATS * 4,
               "gl_blob must be header + GL_MAXFLOATS packed float32");
_Static_assert(sizeof(gl_blob) <= PFS_BLOCK_MAX,
               "gl_blob must fit one p-fs block");

INT gl_pfs_publish(const char *ref, UW reflen, const float *w, UW n)
{
    if (n > GL_MAXFLOATS) return -1;
    gl_blob.h.magic = GL_BLOB_MAGIC;
    gl_blob.h.n     = n;
    for (UW i = 0; i < n; i++) gl_blob.w[i] = w[i];
    INT r = pfs_dag_save((const UB *)ref, reflen, &gl_blob,
                         (UW)(sizeof(GL_BLOB_HDR) + n * 4));
    return (r == PFS_OK) ? 0 : -1;
}

INT gl_pfs_fetch(const char *ref, UW reflen, float *w, UW n)
{
    if (n > GL_MAXFLOATS) return -1;
    INT r = pfs_dag_read((const UB *)ref, reflen, &gl_blob,
                         (UW)(sizeof(GL_BLOB_HDR) + n * 4));
    if (r != (INT)(sizeof(GL_BLOB_HDR) + n * 4)) return -1;
    if (gl_blob.h.magic != GL_BLOB_MAGIC || gl_blob.h.n != n) return -1;
    for (UW i = 0; i < n; i++) w[i] = gl_blob.w[i];
    return 0;
}

/* ------------------------------------------------------------------ */
/* dataset — deterministic synthetic sensor readings.                  */
/*                                                                     */
/* Same task family as dtr_train.c (latent temperature -> 3 classes,   */
/* with correlated distractor channels and label noise so a fixed      */
/* threshold cannot reach 100%). Regenerated here (not shared) so this  */
/* module is self-contained; the generator is deterministic, identical  */
/* bytes on every node / every ABI.                                    */
/* ------------------------------------------------------------------ */

#define GL_N       300
#define GL_TRAIN   240            /* shards partition the first 240    */
#define GL_TEST    (GL_N - GL_TRAIN)  /* held-out 60: all 3 classes    */
#define GL_NCLASS  3
#define GL_SEED    0x5EED2026UL

static B  gl_x[GL_N][DTR_SEQ_LEN];
static UB gl_y[GL_N];
static UB gl_ds_ready = 0;
static UW gl_rng;

static UW gl_rand(void)
{
    gl_rng = gl_rng * 1664525UL + 1013904223UL;
    return (gl_rng >> 16) & 0x7FFF;
}
static INT gl_uniform(INT lo, INT hi)
{
    return lo + (INT)(gl_rand() % (UW)(hi - lo + 1));
}
static INT gl_noise(INT s)
{
    INT v = 0;
    for (INT i = 0; i < 4; i++) v += gl_uniform(-s, s);
    return v / 2;
}
static B gl_clamp(INT v)
{
    if (v >  127) v =  127;
    if (v < -128) v = -128;
    return (B)v;
}

static void gl_ds_init(void)
{
    if (gl_ds_ready) return;
    gl_rng = GL_SEED;
    for (INT i = 0; i < GL_N; i++) {
        UB c = (UB)(i % GL_NCLASS);          /* class-balanced both splits */
        INT latent;
        if (c == 0)      latent = gl_uniform(-50,  24);
        else if (c == 1) latent = gl_uniform( 25,  69);
        else             latent = gl_uniform( 70, 120);
        INT temp  = latent + gl_noise(12);
        INT hum   = 60 - latent / 2 + gl_noise(20);
        INT press = gl_uniform(-30, 90);
        INT light = 10 + 30 * (INT)c + gl_noise(35);
        gl_x[i][0] = gl_clamp(temp);
        gl_x[i][1] = gl_clamp(hum);
        gl_x[i][2] = gl_clamp(press);
        gl_x[i][3] = gl_clamp(light);
        gl_y[i]    = c;
    }
    gl_ds_ready = 1;
}

/* ------------------------------------------------------------------ */
/* shards — leave-one-class-out (DISJOINT, the crux)                   */
/*                                                                     */
/* node k's shard = TRAIN samples whose class != (k % NCLASS). So with  */
/* 3 nodes each MISSES a different class entirely and cannot classify   */
/* it solo — its ceiling on the full (all-class) task is bounded well   */
/* below 100%. Union of the shards covers every class, so the swarm     */
/* CAN, but only by combining (averaging) — not by copying one node.    */
/* ------------------------------------------------------------------ */

#define GL_MAXNODES 4

static B  sh_x[GL_MAXNODES][GL_TRAIN][DTR_SEQ_LEN];
static UB sh_y[GL_MAXNODES][GL_TRAIN];
static UW sh_n[GL_MAXNODES];

/* build node k's shard array (excludes class k%NCLASS from TRAIN). */
static void gl_build_shard(UW k)
{
    UB excl = (UB)(k % GL_NCLASS);
    UW m = 0;
    for (INT i = 0; i < GL_TRAIN; i++) {
        if (gl_y[i] == excl) continue;
        for (INT t = 0; t < DTR_SEQ_LEN; t++) sh_x[k][m][t] = gl_x[i][t];
        sh_y[k][m] = gl_y[i];
        m++;
    }
    sh_n[k] = m;
}

/* full-task held-out accuracy (%) under the currently loaded weights. */
static float gl_full_acc(void)
{
    UW correct = 0;
    (void)dtr_eval_batch(gl_x + GL_TRAIN, gl_y + GL_TRAIN, GL_TEST, &correct);
    return (float)correct * 100.0f / (float)GL_TEST;
}

/* shard accuracy (%) for node k under the currently loaded weights. */
static float gl_shard_acc(UW k)
{
    UW correct = 0;
    (void)dtr_eval_batch(sh_x[k], sh_y[k], sh_n[k], &correct);
    return (float)correct * 100.0f / (float)sh_n[k];
}

/* step-decayed LR (same shape dtr_train.c uses). A healthy LR is what lets
 * a 2-node sub-swarm re-balance its class mix quickly after a peer dies, so
 * the survivors hold above their solo ceilings; the rejoin demo reconverges
 * a fresh 3-node swarm (robust at any LR) rather than chasing frozen peers. */
static float gl_lr(UW step, UW total)
{
    return (step <= total / 2) ? 0.10f : 0.05f;
}

/* ------------------------------------------------------------------ */
/* in-process model bank (simulate N nodes through dtr's single model)  */
/* ------------------------------------------------------------------ */

static float gl_model[GL_MAXNODES][DTR_WEIGHT_FLOATS];
static float gl_avg[DTR_WEIGHT_FLOATS];

/* shared, deterministic starting point so averaging same-origin models
 * is well-behaved early (linear mode connectivity). Every node seeds
 * from the same weights, then diverges only via its shard. */
#define GL_INIT_SEED 0xC0FFEE11UL

/* train the currently-loaded weights for `steps` SGD steps on shard k. */
static void gl_train_local(UW k, UW steps, UW lr_total, UW lr_base)
{
    for (UW s = 1; s <= steps; s++)
        (void)dtr_train_batch(sh_x[k], sh_y[k], sh_n[k],
                              gl_lr(lr_base + s, lr_total));
}

/* ------------------------------------------------------------------ */
/* [g22-shard-solo] — a node on its shard alone caps low on full task  */
/* ------------------------------------------------------------------ */

/* Train each node ONLY on its own shard for `total` steps (no gossip),
 * eval on the full task; return the BEST (max) solo accuracy — the
 * strongest honest baseline the collective must beat. */
static float gl_run_solo(UW nodes, UW total, float per_node_out[])
{
    float best = 0.0f;
    for (UW k = 0; k < nodes; k++) {
        dtr_reinit_weights(GL_INIT_SEED);
        for (UW s = 1; s <= total; s++)
            (void)dtr_train_batch(sh_x[k], sh_y[k], sh_n[k],
                                  gl_lr(s, total));
        float a = gl_full_acc();
        if (per_node_out) per_node_out[k] = a;
        if (a > best) best = a;
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* [g22-gossip-learn] — N nodes, disjoint shards, gossip-merge rounds  */
/* ------------------------------------------------------------------ */

/* Decentralized SGD: each round every node does `local` SGD steps on
 * its own shard, then ALL models are averaged (gossip merge) and each
 * node adopts the average. Returns the (common) full-task accuracy. */
static float gl_run_gossip(UW nodes, UW rounds, UW local)
{
    UW total = rounds * local;
    /* every node starts from the same seed */
    for (UW k = 0; k < nodes; k++) {
        dtr_reinit_weights(GL_INIT_SEED);
        dtr_weights_get(gl_model[k]);
    }
    for (UW r = 0; r < rounds; r++) {
        /* phase A: independent local training on each disjoint shard */
        for (UW k = 0; k < nodes; k++) {
            dtr_weights_set(gl_model[k]);
            gl_train_local(k, local, total, r * local);
            dtr_weights_get(gl_model[k]);
        }
        /* phase B: each node merges (averages) the models it gossiped.
         * No aggregator: gl_merge over the symmetric set of models. */
        const float *ptrs[GL_MAXNODES];
        for (UW k = 0; k < nodes; k++) ptrs[k] = gl_model[k];
        gl_merge(gl_avg, ptrs, nodes, DTR_WEIGHT_FLOATS);
        for (UW k = 0; k < nodes; k++)
            for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++)
                gl_model[k][i] = gl_avg[i];
    }
    dtr_weights_set(gl_model[0]);
    return gl_full_acc();
}

/* ------------------------------------------------------------------ */
/* [g22-no-central] — the merge is peer-symmetric, no aggregator       */
/* ------------------------------------------------------------------ */

/* Structural proof that the merge has NO central aggregator:
 *
 *   1. gl_merge's signature carries a flat SET of models and NO
 *      aggregator/server index — there is no privileged node by
 *      construction (the live path passes {self} U {peers I fetched};
 *      every node runs the same call on the set it gossiped).
 *   2. Order independence: merging the same set in node-0 order vs the
 *      reverse (node-(N-1)) order yields the SAME model up to float
 *      rounding — so no position in the set is special. (Exact byte
 *      equality is NOT required and would be wrong: float addition is
 *      not associative, so a different summation order legitimately
 *      differs in the low bits; a STRUCTURAL privilege would instead
 *      shift weights by O(1), not O(1e-6).)
 *   3. Identity: a single-model "merge" returns that model exactly — no
 *      hidden global state is folded in.
 *
 * All three <=> the merge is peer-symmetric, computed locally by each
 * node from local+peer models, with no aggregator. */
static INT gl_check_no_central(UW nodes)
{
    /* give each "node" a distinct model so order would matter (by O(1))
     * if the merge secretly privileged a position */
    for (UW k = 0; k < nodes; k++) {
        dtr_reinit_weights(GL_INIT_SEED + k * 7919UL);
        dtr_weights_get(gl_model[k]);
    }
    const float *fwd[GL_MAXNODES], *rev[GL_MAXNODES];
    for (UW k = 0; k < nodes; k++) {
        fwd[k] = gl_model[k];
        rev[k] = gl_model[nodes - 1 - k];
    }
    static float a[DTR_WEIGHT_FLOATS], b[DTR_WEIGHT_FLOATS];
    gl_merge(a, fwd, nodes, DTR_WEIGHT_FLOATS);   /* node-0 perspective */
    gl_merge(b, rev, nodes, DTR_WEIGHT_FLOATS);   /* node-(N-1) view    */

    float worst = 0.0f;
    for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) {
        float d = a[i] - b[i]; if (d < 0.0f) d = -d;
        if (d > worst) worst = d;
    }
    gp("[g22]   no-central: |merge(fwd)-merge(rev)| max="); gpf1(worst * 1000.0f);
    gp("e-3 (rounding only; structural privilege would be O(1))\r\n");
    if (worst >= 1e-4f) return 0;                  /* O(1) shift => central */

    /* identity: single-model merge returns it exactly */
    gl_merge(a, fwd, 1, DTR_WEIGHT_FLOATS);
    for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++)
        if (a[i] != gl_model[0][i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* the self-test                                                       */
/* ------------------------------------------------------------------ */

#define GL_ST_NODES   3
#define GL_ST_ROUNDS  40
#define GL_ST_LOCAL   4

void gl_self_test(void)
{
    gp("[g22] ==== decentralized collective learning (G22 / survival-network §8 §9) ====\r\n");
    gl_ds_init();
    for (UW k = 0; k < GL_ST_NODES; k++) gl_build_shard(k);

    gp("[g22] task: 3-class sensor classify; shards leave-one-class-out (DISJOINT).\r\n");
    for (UW k = 0; k < GL_ST_NODES; k++) {
        gp("[g22]   node"); gpd(k); gp(" shard: ");
        gpd(sh_n[k]); gp(" samples, MISSING class ");
        gpd(k % GL_NCLASS); gp(" (cannot classify it solo)\r\n");
    }

    UW total = GL_ST_ROUNDS * GL_ST_LOCAL;

    /* --- [g22-shard-solo] --- */
    float solo_pn[GL_MAXNODES];
    float solo = gl_run_solo(GL_ST_NODES, total, solo_pn);
    gp("[g22] solo (each node, its shard only, ");
    gpd(total); gp(" steps): full-task acc per node = [");
    for (UW k = 0; k < GL_ST_NODES; k++) {
        gpf1(solo_pn[k]); gp("%"); if (k + 1 < GL_ST_NODES) gp(" ");
    }
    gp("]\r\n");
    gp("[g22] solo CEILING (best any node alone) = "); gpf1(solo); gp("%\r\n");
    /* a node missing one of three classes cannot exceed ~2/3 on a
     * class-balanced full task; assert it really is bounded low. */
    if (solo < 80.0f) {
        gp("[g22-shard-solo] PASS (no node alone learns the whole task; ceiling ");
        gpf1(solo); gp("% < 80%)\r\n");
    } else {
        gp("[g22-shard-solo] FAIL (a solo shard scored too high — shards not disjoint?)\r\n");
    }

    /* --- [g22-gossip-learn] --- */
    float coll = gl_run_gossip(GL_ST_NODES, GL_ST_ROUNDS, GL_ST_LOCAL);
    gp("[g22] gossip-learn ("); gpd((UW)GL_ST_ROUNDS);
    gp(" rounds x "); gpd((UW)GL_ST_LOCAL);
    gp(" local steps, no-central avg): full-task acc = ");
    gpf1(coll); gp("%\r\n");
    gp("[g22]   solo-ceiling "); gpf1(solo);
    gp("%  ->  collective "); gpf1(coll); gp("%  (delta +");
    gpf1(coll - solo); gp(")\r\n");
    if (coll > solo + 3.0f)
        gp("[g22-gossip-learn] PASS (collective EXCEEDS solo ceiling — the swarm learned what no node could)\r\n");
    else
        gp("[g22-gossip-learn] FAIL (collective did not clear solo ceiling by margin)\r\n");

    /* --- [g22-no-central] --- */
    if (gl_check_no_central(GL_ST_NODES))
        gp("[g22-no-central] PASS (merge is peer-symmetric / order-independent; no aggregator index — every node averages locally)\r\n");
    else
        gp("[g22-no-central] FAIL (merge depended on order — a privileged aggregator exists)\r\n");

    /* restore a sane trained model for any follow-on infer */
    dtr_weights_set(gl_model[0]);
    gp("[g22] ==== done ====\r\n");
}

/* ------------------------------------------------------------------ */
/* live multi-node — over the relay (sample 32)                        */
/* ------------------------------------------------------------------ */

/* per-node model ref: "dtr/model/<n>" (<=16 chars, fits PFS_NAME_MAX). */
static UW gl_model_ref(char *out, UW node)
{
    const char *p = "dtr/model/";
    UW i = 0;
    while (p[i]) { out[i] = p[i]; i++; }
    if (node >= 10) out[i++] = (char)('0' + (node / 10) % 10);
    out[i++] = (char)('0' + node % 10);
    return i;
}

/* My shard, derived from this node's id (leave-one-class-out). */
static UW gl_my_shard_slot(void)
{
    /* in-process scratch is indexed 0..GL_MAXNODES-1; map the live node
     * id onto a slot deterministically by its excluded class. We only
     * need one shard locally, stored in slot (node%NCLASS) so its
     * excluded-class arithmetic matches gl_build_shard. */
    UB node = (drpc_my_node == 0xFF) ? 0 : drpc_my_node;
    return node % GL_NCLASS;
}

static UW gl_live_total = 0;     /* live gossip rounds completed         */

/* `dtr gossip solo [steps]` — measure THIS node's solo shard ceiling on
 * the full task, then leave the model fresh (re-seeded) for `run`. */
static void gl_cmd_solo(UW steps)
{
    if (steps == 0) steps = 160;
    gl_ds_init();
    UW slot = gl_my_shard_slot();
    gl_build_shard(slot);
    UB node = (drpc_my_node == 0xFF) ? 0 : drpc_my_node;

    dtr_ga_busy = 1;
    dtr_reinit_weights(GL_INIT_SEED);
    for (UW s = 1; s <= steps; s++)
        (void)dtr_train_batch(sh_x[slot], sh_y[slot], sh_n[slot],
                              gl_lr(s, steps));
    float solo = gl_full_acc();
    /* reset to the shared seed so `run` starts clean & comparable */
    dtr_reinit_weights(GL_INIT_SEED);
    dtr_ga_busy = 0;

    gp("[g22-live] node="); gpd(node);
    gp(" shard=missing-class"); gpd(slot);
    gp(" solo_steps="); gpd(steps);
    gp(" solo_ceiling="); gpf1(solo); gp("%\r\n");
}

/* peer-model cache (function-static; never task-stack), indexed by peer
 * node id (live ids are small: 0..GL_MAXNODES-1). Once a peer's model is
 * fetched it is REMEMBERED, so a round whose 2.5 KB pull lagged the P1
 * want/serve still averages that peer in (with its most recent model)
 * instead of dropping to peers=0. gl_phave[] is reset at run start. */
static float gl_pcache[GL_MAXNODES][DTR_WEIGHT_FLOATS];
static UB    gl_phave[GL_MAXNODES];

/* issue a p-fs WANT for every ALIVE peer's model so the (2.5 KB) content
 * transfers during the slow-band delay — by the next round's merge it is
 * usually local. We deliberately touch only ALIVE peers (dnode_table) and
 * not all 32 ids, to keep the read rate low (the ref table is shared with
 * the gossip task). Return value ignored: this is a prefetch. */
static void gl_prefetch_peers(UB me)
{
    static float discard[DTR_WEIGHT_FLOATS];
    for (UB p = 0; p < DNODE_MAX; p++) {
        if (p == me) continue;
        if (dnode_table[p].state != DNODE_ALIVE) continue;
        char pref[20]; UW prl = gl_model_ref(pref, p);
        (void)gl_pfs_fetch(pref, prl, discard, DTR_WEIGHT_FLOATS);
    }
}

/* MERGE phase: read whatever ALIVE peers' models are already local and
 * average them with my own (no central aggregator — every node does this
 * locally over the symmetric set {self} U {peers it gossiped}). Returns
 * the number of peers actually folded in.
 *
 * Collective learning needs each round to actually fold in EVERY live
 * peer; a 2.5 KB model can lag a P1 want/serve, so we give a missing peer
 * a couple of bounded retries (re-issue the want via the fetch, brief
 * wait) before giving up for this round. Robust to transfer jitter
 * without changing any transport code. */
#define GL_FETCH_RETRY    3
#define GL_FETCH_WAIT_MS  250

static UW gl_merge_peers(UB me)
{
    const float *ptrs[GL_MAXNODES];
    UW cnt = 0;
    ptrs[cnt++] = gl_model[0];                       /* myself */
    for (UB p = 0; p < GL_MAXNODES; p++) {
        if (p == me) continue;
        if (dnode_table[p].state != DNODE_ALIVE) continue;
        char pref[20]; UW prl = gl_model_ref(pref, p);
        for (INT a = 0; a < GL_FETCH_RETRY; a++) {
            if (gl_pfs_fetch(pref, prl, gl_pcache[p], DTR_WEIGHT_FLOATS) == 0) {
                gl_phave[p] = 1;        /* fresh model cached */
                break;
            }
            tk_dly_tsk(GL_FETCH_WAIT_MS);   /* let the want/serve land */
        }
        /* fold the peer in whether the pull was fresh this round or a
         * cached recent model — an alive peer never silently drops out. */
        if (gl_phave[p]) ptrs[cnt++] = gl_pcache[p];
    }
    if (cnt > 1) {
        gl_merge(gl_avg, ptrs, cnt, DTR_WEIGHT_FLOATS);
        for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) gl_model[0][i] = gl_avg[i];
    }
    return cnt - 1;
}

/* §8 deliberation cadence (seconds-band). Long enough that a peer's freshly
 * published 2.5 KB model transfers (P1 want/serve) before the next merge. */
#define GL_SLOW_BAND_MS 2000

/* fresh != 0: start from the shared seed (a node joining/learning from
 * scratch). fresh == 0 ("cont"): keep the CURRENT weights — used by peers
 * that stay in the swarm while a fresh node rejoins, so they hold their
 * converged model instead of restarting (and don't get reset to noise). */
static void gl_cmd_run_ex(UW rounds, UW local, INT fresh)
{
    if (rounds == 0) rounds = 36;
    if (local  == 0) local  = 4;     /* matches the in-process self-test  */
    gl_ds_init();
    UW slot = gl_my_shard_slot();
    gl_build_shard(slot);
    UB me = (drpc_my_node == 0xFF) ? 0 : drpc_my_node;
    UW total = rounds * local;
    for (UW p = 0; p < GL_MAXNODES; p++) gl_phave[p] = 0;   /* fresh cache */

    gp("[g22-live] node="); gpd(me);
    gp(fresh ? " START gossip-learn rounds=" : " CONT gossip-learn rounds=");
    gpd(rounds);
    gp(" local="); gpd(local);
    gp(" shard=missing-class"); gpd(slot);
    gp(" (slow-band "); gpd((UW)GL_SLOW_BAND_MS); gp("ms/round)\r\n");

    dtr_ga_busy = 1;
    if (fresh) dtr_reinit_weights(GL_INIT_SEED);
    dtr_weights_get(gl_model[0]);

    float full = 0.0f;
    for (UW r = 0; r < rounds; r++) {
        /* FedAvg order (matches the in-process self-test): local-train
         * FIRST, then AVERAGE, and KEEP/eval the average. Averaging after
         * training is what defeats non-IID client drift — if we trained
         * after averaging and kept that, local SGD on a 2-class shard
         * would re-suppress the third class the average just taught us. */

        /* (1) local SGD on my disjoint shard, from last round's consensus */
        dtr_weights_set(gl_model[0]);
        gl_train_local(slot, local, total, r * local);
        dtr_weights_get(gl_model[0]);
        float shard = gl_shard_acc(slot);     /* my shard, post local-train */

        /* (2) PUBLISH my locally-updated model for peers to average in */
        char ref[20]; UW rl = gl_model_ref(ref, me);
        (void)gl_pfs_publish(ref, rl, gl_model[0], DTR_WEIGHT_FLOATS);

        /* (3) MERGE: average my model with peers' locally-updated models
         * (no central aggregator). The AVERAGE becomes my model — this is
         * the consensus we keep and evaluate. */
        UW peers = gl_merge_peers(me);
        dtr_weights_set(gl_model[0]);

        full = gl_full_acc();                 /* eval the CONSENSUS model */
        gp("[g22-live] node="); gpd(me);
        gp(" round="); gpd(r + 1);
        gp(" peers="); gpd(peers);
        gp(" shard_acc="); gpf1(shard);
        gp("% full_acc="); gpf1(full); gp("%\r\n");
        gl_live_total++;

        /* (4) prefetch peers + slow deliberation band (not the reflex
         * tick). dtr_ga_busy blocks dtr_infer while weights move; release
         * it across the delay so reflex/inference keep breathing. */
        gl_prefetch_peers(me);
        dtr_ga_busy = 0;
        tk_dly_tsk(GL_SLOW_BAND_MS);
        dtr_ga_busy = 1;
    }
    dtr_ga_busy = 0;

    gp("[g22-live] node="); gpd(me);
    gp(" RESULT rounds="); gpd(rounds);
    gp(" full_acc="); gpf1(full); gp("%\r\n");
}

static void gl_cmd_status(void)
{
    UB me = (drpc_my_node == 0xFF) ? 0 : drpc_my_node;
    gp("[g22-live] node="); gpd(me);
    gp(" excluded-class="); gpd(gl_my_shard_slot());
    gp(" rounds_done="); gpd(gl_live_total);
    gp(" weight_body="); gpd((UW)DTR_WEIGHT_FLOATS); gp(" floats\r\n");
}

/* ------------------------------------------------------------------ */
/* shell dispatcher                                                    */
/* ------------------------------------------------------------------ */

static const UB *gl_skip_ws(const UB *p, const UB *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}
static INT gl_tok(const UB *p, const UB *end, const char *kw)
{
    INT i = 0;
    while (kw[i]) { if (p + i >= end || p[i] != (UB)kw[i]) return 0; i++; }
    return (p + i == end || p[i] == ' ' || p[i] == '\t');
}
static UW gl_parse_uw(const UB **pp, const UB *end)
{
    const UB *p = gl_skip_ws(*pp, end);
    UW v = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (UW)(*p - '0'); p++; }
    *pp = p;
    return v;
}

void gl_cmd(const UB *args, UW len)
{
    const UB *end = args + len;
    const UB *p   = gl_skip_ws(args, end);

    if (p >= end || gl_tok(p, end, "status")) { gl_cmd_status(); return; }
    if (gl_tok(p, end, "test")) { gl_self_test(); return; }
    if (gl_tok(p, end, "solo")) {
        p += 4; UW steps = gl_parse_uw(&p, end);
        gl_cmd_solo(steps); return;
    }
    if (gl_tok(p, end, "run")) {
        p += 3;
        UW rounds = gl_parse_uw(&p, end);
        UW local  = gl_parse_uw(&p, end);
        gl_cmd_run_ex(rounds, local, 1);   /* fresh: start from seed */
        return;
    }
    if (gl_tok(p, end, "cont")) {
        p += 4;
        UW rounds = gl_parse_uw(&p, end);
        UW local  = gl_parse_uw(&p, end);
        gl_cmd_run_ex(rounds, local, 0);   /* keep current converged model */
        return;
    }
    gp("usage: dtr gossip [status] | test | solo [steps] | run [rounds] [local] | cont [rounds] [local]\r\n");
}
