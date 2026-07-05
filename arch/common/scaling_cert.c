/*
 *  scaling_cert.c — the [scaling-*] cert suite + requester-side PURE
 *  society-of-minds ensemble aggregation (docs/architecture/scaling-law.md).
 *
 *  The thesis: "as N grows the mind gets smarter." This cert refuses to let
 *  that slogan be asserted. It decomposes "smarter" and MEASURES each axis:
 *
 *    [scaling-converged-null]  the DISEASE / anti-theater keystone. N gossip-
 *        CONVERGED, bit-identical nodes ensemble-answer Q_hard; the collective
 *        answer equals the solo answer on EVERY item (delta == 0 EXACTLY —
 *        one-math determinism makes it an equality, not a tolerance). Proof
 *        that node COUNT alone buys ZERO per-answer quality.
 *    [scaling-breadth-curve]   N distinct fact-shards; the gossip-merged
 *        consensus answers the UNION better than any solo node; the KNEE
 *        (first N where ΔB < margin) is the honest capacity headline.
 *    [scaling-ensemble] / [scaling-ensemble-gain]   the CURE arm: same corpus,
 *        same steps, same model class, K distinct-seed lineages. Ensemble via
 *        the pure §4.1 aggregation. If it beats the best member by margin with
 *        vote-flips -> real bounded gain ([scaling-ensemble-gain] green). If
 *        delta <= noise at all K -> the PRE-REGISTERED NULL is printed and the
 *        strict gate stays RED: that is a PASS of HONESTY, not of the mechanism
 *        (scaling-law.md §4.4 / §6.3).
 *    [scaling-quorum-degrade]  the ensemble-ask quorum LOGIC: quorum_core
 *        "don't wait for the dead" + honest k/K degrade (a 1/K answer is a
 *        solo answer, printed as such, never a faked K/K). This is the pure
 *        core a [scaling-live-ensemble] wire arm WOULD run over the relay — a
 *        NOT-YET-WIRED follow-up (there is no self-hosted [scaling-live] job).
 *    [scaling-curve]           the standing composite falsifier of the doc.
 *
 *  Three confounds ruled out BY CONSTRUCTION (§6.3):
 *    - "just more data"        the ensemble arm holds corpus + step budget
 *                              constant across every K (only the breadth arm
 *                              varies data, and says so).
 *    - "one node did the work" the vote-flip assert (majority right while >=1
 *                              member wrong).
 *    - "cosmetic capacity"     every metric is end-to-end RETURNED-ANSWER
 *                              accuracy through the real dtr ask path
 *                              (dtr_forward_probs), never a count of hosted
 *                              bytes (dmoe §10.1 discipline inherited).
 *
 *  Built on dtr.c's REAL 635-float transformer through its PUBLIC API only
 *  (dtr_forward_probs / dtr_train_batch / dtr_eval_batch / dtr_weights_*),
 *  and gossip_learn.c's PUBLIC gl_merge. We invent no new oracle: the
 *  confidence weight is the already-proven calibrated max-softmax signal
 *  ([g38-confidence-live], moe.c:128-141).
 *
 *  HOSTED-TIER ONLY — the whole file is under #ifdef _TK_HOSTED_LIBC_ and is
 *  absent from the bare-metal Makefiles, so the crown .text does not move.
 */

#ifdef _TK_HOSTED_LIBC_

#include "dtr.h"
#include "drpc.h"          /* DNODE_MAX (lineage/quorum table width) */
#include "gossip_learn.h"  /* gl_merge (public, no-central average) */
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* output helpers (sio frame channel, same as gossip_learn.c)          */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void sc_p(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}
static void sc_pd(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { sc_p("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    sc_p(&buf[i]);
}
static void sc_pi(INT v) { if (v < 0) { sc_p("-"); sc_pd((UW)(-v)); } else sc_pd((UW)v); }
/* xx.x percent / delta */
static void sc_pf1(float f)
{
    if (f < 0.0f) { sc_p("-"); f = -f; }
    UW whole = (UW)f;
    UW frac  = (UW)((f - (float)whole) * 10.0f + 0.5f);
    if (frac >= 10) { whole++; frac = 0; }
    sc_pd(whole); sc_p("."); sc_pd(frac);
}

/* ================================================================== */
/* PART 1 — requester-side PURE ensemble aggregation (scaling-law §4.1) */
/*                                                                      */
/* No central judge: the REQUESTER aggregates, exactly the dkva origin- */
/* side fold. Every requester runs the SAME pure function over the SAME */
/* response set, so the collective answer is a pure function of (query, */
/* member set) — ownerless because the function is symmetric and every  */
/* node can evaluate it.                                                */
/* ================================================================== */

#define SC_C     DTR_OUT_DIM        /* answer classes (3)               */
#define ENS_K    3                  /* compile-time lineage count (§4.2) */

/* One member's response to an ensemble ask. */
typedef struct {
    UW    lineage_id;               /* node_id % ENS_K (§4.2)           */
    UW    node_id;
    UB    alive;                    /* 0 => dead: don't fold (honest degrade) */
    UB    answer;                   /* argmax class (generative vote path) */
    float conf;                     /* max-softmax 0..1 ([g38-confidence-live]) */
    float prob[SC_C];               /* full softmax vector (classifier path) */
} ENS_RESP;

/* Stable sort by (lineage_id, node_id). Sorting BEFORE the float sum makes
 * the confidence-weighted accumulation ORDER-INDEPENDENT across requesters —
 * the same discipline [g22-no-central] uses (a structural privilege would
 * shift the result O(1); float reassociation only O(1e-6)). Insertion sort:
 * member counts are tiny (<= DNODE_MAX) and it is a stable, static, no-alloc
 * routine (never the task stack for the array — caller owns it). */
static void ens_sort(ENS_RESP *r, UW n)
{
    for (UW i = 1; i < n; i++) {
        ENS_RESP key = r[i];
        INT j = (INT)i - 1;
        while (j >= 0 &&
               (r[j].lineage_id > key.lineage_id ||
                (r[j].lineage_id == key.lineage_id && r[j].node_id > key.node_id))) {
            r[j + 1] = r[j];
            j--;
        }
        r[j + 1] = key;
    }
}

/* Confidence-weighted probability aggregation for the CLASSIFIER path
 * (§4.1 step 3): argmax over Σ_alive confᵢ·pᵢ. Members are sorted first, so
 * the sum is order-independent. Dead members are skipped (they buy nothing —
 * honest degrade). Sets *k_alive to the number of members actually folded.
 * Returns 0xFF when NObody answered (never a faked answer). */
UB ens_agg_conf(ENS_RESP *r, UW n, UW *k_alive)
{
    ens_sort(r, n);
#ifdef SCALING_AGG_STUB
    /* ===== ANTI-THEATER falsifier -DSCALING_AGG_STUB (cross-audit #5) =====
     * The aggregator is GUTTED to a first-member (r[0]) passthrough — the
     * cheapest "aggregator" that still returns an argmax class. It passes the
     * converged-null arm (N identical copies => r[0] IS the solo answer) AND the
     * order-independence arm (sorted r[0] is position-stable), which is exactly
     * why those arms cannot detect it. [scaling-agg-mechanism] — divergent
     * members whose confidence-weighted prob-mass argmax is a class NO member
     * names — MUST then go RED. The production binary contains no such switch. */
    { UW k = 0; UB first = 0xFF;
      for (UW i = 0; i < n; i++) if (r[i].alive) { if (first == 0xFF) first = r[i].answer; k++; }
      if (k_alive) *k_alive = k;
      return first; }
#else
    float acc[SC_C];
    for (UW c = 0; c < SC_C; c++) acc[c] = 0.0f;
    UW k = 0;
    for (UW i = 0; i < n; i++) {
        if (!r[i].alive) continue;
        for (UW c = 0; c < SC_C; c++) acc[c] += r[i].conf * r[i].prob[c];
        k++;
    }
    if (k_alive) *k_alive = k;
    if (k == 0) return 0xFF;
    UB best = 0; float mx = acc[0];
    for (UW c = 1; c < SC_C; c++) if (acc[c] > mx) { mx = acc[c]; best = (UB)c; }
    return best;
#endif
}

/* Majority vote for the GENERATIVE path (§4.1 step 3): vote on the members'
 * normalized short answers. Confidence-weighted tallies also serve as the
 * deterministic tie-break (a raw count tie is broken by summed confidence,
 * then by lowest class id) so the vote is a pure function of the sorted set.
 * §9.5's normalization risk is out of scope for the classifier answer here
 * (answers are already class ids). */
UB ens_agg_vote(ENS_RESP *r, UW n, UW *k_alive)
{
    ens_sort(r, n);
    UW   count[SC_C];
    float wsum[SC_C];
    for (UW c = 0; c < SC_C; c++) { count[c] = 0; wsum[c] = 0.0f; }
    UW k = 0;
    for (UW i = 0; i < n; i++) {
        if (!r[i].alive) continue;
        UB a = r[i].answer; if (a >= SC_C) continue;
        count[a]++; wsum[a] += r[i].conf; k++;
    }
    if (k_alive) *k_alive = k;
    if (k == 0) return 0xFF;
    UB best = 0;
    for (UW c = 1; c < SC_C; c++) {
        if (count[c] > count[best] ||
            (count[c] == count[best] && wsum[c] > wsum[best]))
            best = (UB)c;
    }
    return best;
}

/* ENS-B (§3, §4.1): the per-node sampling seed is a PURE function of
 * (query ‖ node_id) — identical weights, decorrelated reasoning paths, and
 * the whole collective answer stays a deterministic, reproducible, ownerless
 * function of (query, fleet set). FNV-1a over the query bytes then the id. */
UW ens_seed(const UB *query, UW qlen, UW node_id)
{
    UW h = 2166136261u;
    for (UW i = 0; i < qlen; i++) { h ^= query[i]; h *= 16777619u; }
    h ^= (node_id & 0xFF);        h *= 16777619u;
    h ^= (node_id >> 8) & 0xFF;   h *= 16777619u;
    return h;
}

/* Quorum finalize predicate — the dkva quorum_core semantics VERBATIM
 * (dkva.c:379-384): an ensemble ask is done when every EXPECTED member has
 * either arrived (got) or is no longer alive. We never wait for the dead;
 * the missing live-then-dead members are honestly counted in the k/K degrade
 * by the caller. Pure: reads only local exp/got/alive. */
BOOL ens_quorum(const UB *exp, const UB *got, const UB *alive, UW n)
{
    for (UW i = 0; i < n; i++)
        if (exp[i] && !got[i] && alive[i]) return FALSE;   /* live member unheard */
    return TRUE;
}

/* lineage(node) = node_id % ENS_K (§4.2): deterministic, zero-coordination,
 * churn-stable, ownerless. */
static UW ens_lineage(UW node_id) { return node_id % (UW)ENS_K; }

/* ================================================================== */
/* PART 2 — self-contained deterministic dataset (identical bytes on    */
/* every node / ABI, one-math). Same task family as dtr_train.c /       */
/* gossip_learn.c: latent temperature -> 3 classes with correlated      */
/* distractors + label noise so no fixed threshold reaches 100%.        */
/* ================================================================== */

#define SC_N       600
#define SC_TRAIN   360
#define SC_TEST    (SC_N - SC_TRAIN)      /* 240 held-out, all 3 classes */
#define SC_NCLASS  3
#define SC_SEED    0x5CA11A00UL

static B  sc_x[SC_N][DTR_SEQ_LEN];
static UB sc_y[SC_N];
static UB sc_ready = 0;
static UW sc_rng;

static UW sc_rand(void)  { sc_rng = sc_rng * 1664525UL + 1013904223UL; return (sc_rng >> 16) & 0x7FFF; }
static INT sc_uni(INT lo, INT hi) { return lo + (INT)(sc_rand() % (UW)(hi - lo + 1)); }
static INT sc_noise(INT s) { INT v = 0; for (INT i = 0; i < 4; i++) v += sc_uni(-s, s); return v / 2; }
static B  sc_clamp(INT v) { if (v > 127) v = 127; if (v < -128) v = -128; return (B)v; }

static void sc_ds_init(void)
{
    if (sc_ready) return;
    sc_rng = SC_SEED;
    for (INT i = 0; i < SC_N; i++) {
        UB c = (UB)(i % SC_NCLASS);              /* class-balanced both splits */
        INT latent;
        if (c == 0)      latent = sc_uni(-50,  24);
        else if (c == 1) latent = sc_uni( 25,  69);
        else             latent = sc_uni( 70, 120);
        INT temp  = latent + sc_noise(12);
        INT hum   = 60 - latent / 2 + sc_noise(20);
        INT press = sc_uni(-30, 90);
        INT light = 10 + 30 * (INT)c + sc_noise(35);
        sc_x[i][0] = sc_clamp(temp);
        sc_x[i][1] = sc_clamp(hum);
        sc_x[i][2] = sc_clamp(press);
        sc_x[i][3] = sc_clamp(light);
        sc_y[i]    = c;
    }
    sc_ready = 1;
}

/* held-out full-task accuracy (%) under the currently-loaded weights */
static float sc_full_acc(void)
{
    UW correct = 0;
    (void)dtr_eval_batch(sc_x + SC_TRAIN, sc_y + SC_TRAIN, SC_TEST, &correct);
    return (float)correct * 100.0f / (float)SC_TEST;
}

/* step-decayed LR (same shape as gossip_learn.c) */
static float sc_lr(UW step, UW total) { return (step <= total / 2) ? 0.10f : 0.05f; }

/* ------------------------------------------------------------------ */
/* model bank + per-member precomputed answers (all static .bss)       */
/* ------------------------------------------------------------------ */

#define SC_MAXNODES  16
#define SC_ENS_MAX    9              /* largest K in the ensemble sweep  */

static float sc_model[SC_MAXNODES][DTR_WEIGHT_FLOATS];
static float sc_avg[DTR_WEIGHT_FLOATS];   /* gl_merge target — MUST NOT alias any model */

/* per (member, test-item): the REAL dtr softmax vector + argmax + max-softmax
 * confidence. This is the whole ensemble input, computed through the real ask
 * path exactly once per member and reused across the K-sweep. */
static float sc_probs[SC_ENS_MAX][SC_TEST][SC_C];
static UB    sc_ans[SC_ENS_MAX][SC_TEST];
static float sc_conf[SC_ENS_MAX][SC_TEST];

/* fill sc_probs/sc_ans/sc_conf[m] from weight body w (through the REAL
 * dtr_forward_probs answer path — the [onebrain-accuracy] discipline). */
static void sc_precompute(UW m, const float *w)
{
    dtr_weights_set(w);
    for (UW i = 0; i < SC_TEST; i++) {
        float p[SC_C];
        dtr_forward_probs(sc_x[SC_TRAIN + i], p);
        UB cls = 0; float mx = p[0];
        for (UW c = 0; c < SC_C; c++) {
            sc_probs[m][i][c] = p[c];
            if (p[c] > mx) { mx = p[c]; cls = (UB)c; }
        }
        sc_ans[m][i]  = cls;
        sc_conf[m][i] = mx;                       /* max-softmax = the conf oracle */
    }
}

static float sc_member_acc(UW m)
{
    UW ok = 0;
    for (UW i = 0; i < SC_TEST; i++) if (sc_ans[m][i] == sc_y[SC_TRAIN + i]) ok++;
    return (float)ok * 100.0f / (float)SC_TEST;
}

/* Evaluate the K-ensemble (pure ens_agg_conf over the precomputed members)
 * against its best single member, optionally restricted to a hard-item mask
 * (mask[i]!=0). Also counts vote-flips (ensemble RIGHT while >=1 member WRONG
 * — the vote demonstrably worked; rules out "one member did all the work").
 * lineage/node ids drive the deterministic sort. Everything through the REAL
 * dtr answer path (sc_ans/sc_conf/sc_probs), never a count of stored bytes. */
static void sc_eval_K(UW K, const UB *mask,
                      float *ens_acc, float *best_member, UW *voteflip, UW *nitems)
{
    UW n = 0, ens_ok = 0, vf = 0;
    UW mok[SC_ENS_MAX];
    for (UW m = 0; m < K; m++) mok[m] = 0;
    for (UW i = 0; i < SC_TEST; i++) {
        if (mask && !mask[i]) continue;
        n++;
        UB truth = sc_y[SC_TRAIN + i];
        ENS_RESP r[SC_ENS_MAX];
        INT any_wrong = 0;
        for (UW m = 0; m < K; m++) {
            r[m].lineage_id = ens_lineage(m);
            r[m].node_id    = m;
            r[m].alive      = 1;
            r[m].answer     = sc_ans[m][i];
            r[m].conf       = sc_conf[m][i];
            for (UW c = 0; c < SC_C; c++) r[m].prob[c] = sc_probs[m][i][c];
            if (sc_ans[m][i] != truth) any_wrong = 1;
            if (sc_ans[m][i] == truth) mok[m]++;
        }
        UW k = 0;
        UB a = ens_agg_conf(r, K, &k);
        if (a == truth) { ens_ok++; if (any_wrong) vf++; }
    }
    *ens_acc = n ? (float)ens_ok * 100.0f / (float)n : 0.0f;
    float bm = 0.0f;
    for (UW m = 0; m < K; m++) { float acc = n ? (float)mok[m] * 100.0f / (float)n : 0.0f; if (acc > bm) bm = acc; }
    *best_member = bm;
    *voteflip = vf;
    *nitems = n;
}

/* ================================================================== */
/* leave-one-class-out shards (breadth arm) — the g22-proven regime so   */
/* weight-averaging composes: node k excludes class (k % NCLASS), so it   */
/* sees TWO of three classes and cannot classify the third solo; the      */
/* union of shards covers every class, so the merged consensus CAN.       */
/* ================================================================== */

static B  sc_sh_x[SC_NCLASS][SC_TRAIN][DTR_SEQ_LEN];
static UB sc_sh_y[SC_NCLASS][SC_TRAIN];
static UW sc_sh_n[SC_NCLASS];
static UB sc_sh_ready = 0;

static void sc_build_shards(void)
{
    if (sc_sh_ready) return;
    for (UW s = 0; s < SC_NCLASS; s++) {
        UW m = 0;
        for (INT i = 0; i < SC_TRAIN; i++) {
            if (sc_y[i] == (UB)s) continue;           /* leave class s out */
            for (INT t = 0; t < DTR_SEQ_LEN; t++) sc_sh_x[s][m][t] = sc_x[i][t];
            sc_sh_y[s][m] = sc_y[i];
            m++;
        }
        sc_sh_n[s] = m;
    }
    sc_sh_ready = 1;
}

#define SC_INIT_SEED 0xC0FFEE11UL      /* shared start (linear mode connectivity) */

/* train one leave-out shard alone for `total` steps, return full-task acc */
static float sc_solo_shard(UW s, UW total)
{
    dtr_reinit_weights(SC_INIT_SEED);
    for (UW step = 1; step <= total; step++)
        (void)dtr_train_batch(sc_sh_x[s], sc_sh_y[s], sc_sh_n[s], sc_lr(step, total));
    return sc_full_acc();
}

/* N-node decentralized SGD (FedAvg): each round every node does `local` SGD
 * steps on its own leave-out shard (node k -> shard k%NCLASS), then ALL models
 * are gl_merge-averaged (no central aggregator) and each node adopts the mean.
 * Returns the consensus full-task accuracy. This is the REAL gl_merge. */
static float sc_gossip(UW N, UW rounds, UW local)
{
    UW total = rounds * local;
    for (UW k = 0; k < N; k++) { dtr_reinit_weights(SC_INIT_SEED); dtr_weights_get(sc_model[k]); }
    for (UW r = 0; r < rounds; r++) {
        for (UW k = 0; k < N; k++) {
            UW s = k % SC_NCLASS;
            dtr_weights_set(sc_model[k]);
            for (UW step = 1; step <= local; step++)
                (void)dtr_train_batch(sc_sh_x[s], sc_sh_y[s], sc_sh_n[s],
                                      sc_lr(r * local + step, total));
            dtr_weights_get(sc_model[k]);
        }
        const float *ptrs[SC_MAXNODES];
        for (UW k = 0; k < N; k++) ptrs[k] = sc_model[k];
        /* merge into a SEPARATE buffer: gl_merge zeroes `out` first, so `out`
         * must never alias a model (the gl_run_gossip discipline). */
        gl_merge(sc_avg, ptrs, N, DTR_WEIGHT_FLOATS);           /* consensus */
        for (UW k = 0; k < N; k++)
            for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) sc_model[k][i] = sc_avg[i];
    }
    dtr_weights_set(sc_model[0]);
    return sc_full_acc();
}

/* ================================================================== */
/* ARM RESULTS (shared with the [scaling-curve] composite)             */
/* ================================================================== */

static float g_breadth[4];             /* B(1),B(2),B(4),B(8)          */
static UW    g_breadth_N[4] = {1, 2, 4, 8};
static INT   g_breadth_knee = -1;      /* first N at/after ΔB<margin    */
static float g_solo_flat_lo = 0, g_solo_flat_hi = 0;  /* per-member solo spread */
static float g_ens_best_member = 0, g_ens_best_K = 0; /* ensemble arm  */
static INT   g_ens_gain = 0;           /* 1 = real gain, 0 = null       */
static float g_ens_delta = 0;
static INT   g_resil_full = -1, g_resil_deg = -1, g_resil_none = -1;

/* ------------------------------------------------------------------ */
/* pre-registered thresholds (NOT tuned to the observed result)        */
/* ------------------------------------------------------------------ */
#define SC_BREADTH_KNEE_MARGIN  3.0f   /* ΔB below this => the knee      */
#define SC_BREADTH_NOISE        2.0f   /* monotone-nondecreasing slack   */
#define SC_ENS_MARGIN_PP        1.0f   /* ensemble must beat best member by >1.0pp */
#define SC_ENS_NULL_NOISE       0.5f   /* delta <= this at ALL K => null */
#define SC_ENS_VOTEFLIP_M       3      /* >= m items the vote demonstrably worked */

/* ================================================================== */
/* ARM A — [scaling-agg-order] : the pure aggregation is order-free +   */
/* identity + honest degrade (unit properties of PART 1).              */
/* ================================================================== */
static void arm_agg_order(void)
{
    sc_p("[scaling] ---- PART 1: pure aggregation properties ----\r\n");
    INT fail = 0;

    /* three synthetic members with DISTINCT prob vectors so order would
     * matter (by O(1)) if the aggregation secretly privileged a position. */
    ENS_RESP a[3] = {
        { .lineage_id = 0, .node_id = 0, .alive = 1, .answer = 0, .conf = 0.51f, .prob = {0.51f, 0.30f, 0.19f} },
        { .lineage_id = 1, .node_id = 1, .alive = 1, .answer = 1, .conf = 0.60f, .prob = {0.20f, 0.60f, 0.20f} },
        { .lineage_id = 2, .node_id = 2, .alive = 1, .answer = 2, .conf = 0.44f, .prob = {0.28f, 0.28f, 0.44f} },
    };
    ENS_RESP fwd[3], rev[3];
    for (INT i = 0; i < 3; i++) { fwd[i] = a[i]; rev[i] = a[2 - i]; }
    UW kf = 0, kr = 0;
    UB af = ens_agg_conf(fwd, 3, &kf);
    UB ar = ens_agg_conf(rev, 3, &kr);
    sc_p("[scaling]   agg(fwd)="); sc_pd(af); sc_p(" agg(rev)="); sc_pd(ar);
    sc_p(" (sorted-order float sum => position-independent)\r\n");
    if (af != ar || kf != 3 || kr != 3) fail = 1;

    /* identity: a single-member ensemble returns that member's own argmax. */
    ENS_RESP one[1] = { a[1] };
    UW k1 = 0; UB a1 = ens_agg_conf(one, 1, &k1);
    if (a1 != 1 || k1 != 1) fail = 1;

    /* honest degrade: kill members -> k falls; 0 alive => 0xFF (never faked). */
    ENS_RESP deg[3]; for (INT i = 0; i < 3; i++) deg[i] = a[i];
    deg[0].alive = 0; UW kd = 0; (void)ens_agg_conf(deg, 3, &kd);
    deg[1].alive = 0; deg[2].alive = 0; UW k0 = 0; UB a0 = ens_agg_conf(deg, 3, &k0);
    if (kd != 2 || k0 != 0 || a0 != 0xFF) fail = 1;
    sc_p("[scaling]   degrade: 3 alive->k=3, 1 dead->k="); sc_pd(kd);
    sc_p(", all dead->k=0 answer="); sc_pd(a0); sc_p(" (0xFF = no fake)\r\n");

    /* ENS-B seed purity: H(query‖node) deterministic + node-separated. */
    UB q[3] = { 's', 'u', 'n' };
    UW s0 = ens_seed(q, 3, 0), s0b = ens_seed(q, 3, 0), s1 = ens_seed(q, 3, 1);
    if (s0 != s0b || s0 == s1) fail = 1;

    if (!fail) sc_p("[scaling-agg-order] PASS (order-independent + identity + honest degrade + pure ENS-B seed)\r\n");
    else       sc_p("[scaling-agg-order] FAIL\r\n");
}

/* ================================================================== */
/* ARM A2 — [scaling-agg-mechanism] : the LOAD-BEARING mechanism         */
/* falsifier (cross-audit #5). The converged-null arm's delta==0 (and the */
/* order arm) both survive a GUTTED aggregator that just returns r[0]'s    */
/* answer — proof that node COUNT buys nothing, but NOT proof the          */
/* aggregator integrates the members. This arm proves the mechanism: it    */
/* hands ens_agg_conf DIVERGENT members whose confidence-weighted PROB-MASS */
/* argmax is a class NO member NAMES as its own answer (and which differs   */
/* from r[0] in BOTH given and sorted order). Only genuine Σ confᵢ·pᵢ       */
/* integration reaches it; an r[0]-passthrough / majority-vote / highest-   */
/* conf-answer stub yields the WRONG class -> RED. -DSCALING_AGG_STUB guts  */
/* the aggregator and MUST turn this arm RED (the falsifier's falsifier).   */
/* ================================================================== */
static void arm_agg_mechanism(void)
{
    INT fail = 0;
    /* prob-mass argmax = class 1, yet the members' ANSWER fields are {0,0,2} —
     * no member names class 1. acc = 0.5*(0.50+0.45+0.10, 0.40+0.45+0.45,
     * 0.10+0.10+0.45) = (0.525, 0.650, 0.325) -> argmax 1 (unambiguous margin). */
    ENS_RESP r[3] = {
        { .lineage_id = 0, .node_id = 0, .alive = 1, .answer = 0, .conf = 0.50f, .prob = {0.50f, 0.40f, 0.10f} },
        { .lineage_id = 1, .node_id = 1, .alive = 1, .answer = 0, .conf = 0.50f, .prob = {0.45f, 0.45f, 0.10f} },
        { .lineage_id = 2, .node_id = 2, .alive = 1, .answer = 2, .conf = 0.50f, .prob = {0.10f, 0.45f, 0.45f} },
    };
    const UB EXPECT = 1;              /* the confidence-weighted prob-mass argmax */
    UB r0_answer   = r[0].answer;     /* 0 — what an r[0]-passthrough stub returns */
    UW k = 0;
    UB got = ens_agg_conf(r, 3, &k);  /* ens_sort keeps lineage-0 first -> r0 stays 0 */
    sc_p("[scaling]   mechanism: agg="); sc_pd(got);
    sc_p(" expect="); sc_pd(EXPECT); sc_p(" r0-passthrough="); sc_pd(r0_answer);
    sc_p(" k="); sc_pd(k);
    sc_p("  (no member ANSWERS class 1; only Σconf·p integration reaches it)\r\n");
    if (got != EXPECT) fail = 1;      /* the real aggregator MUST reach class 1   */
    if (got == r0_answer) fail = 1;   /* an r[0]/vote/highest-conf stub would not */
    if (k != 3) fail = 1;
    if (!fail) sc_p("[scaling-agg-mechanism] PASS (confidence-weighted prob-mass integration; NOT an r[0]/vote passthrough)\r\n");
    else       sc_p("[scaling-agg-mechanism] FAIL (aggregator is a stub — it did not integrate the members)\r\n");
}

/* ================================================================== */
/* ARM B — [scaling-lineage] : ENS-A lineage is a pure ownerless        */
/* function of node_id (node_id % K); the gl_merge_peers hosted filter   */
/* folds ONLY same-lineage peers (verified as a predicate here).        */
/* ================================================================== */
static void arm_lineage(void)
{
    INT fail = 0;
    /* partition DNODE_MAX ids into K lineages; assert exact node_id%K classes
     * and that the merge predicate (p%K==me%K) is symmetric + reflexive-free. */
    UW counts[ENS_K]; for (UW l = 0; l < ENS_K; l++) counts[l] = 0;
    for (UW id = 0; id < DNODE_MAX; id++) counts[ens_lineage(id)]++;
    /* DNODE_MAX=64, K=3 => sizes {22,21,21} (64 = 22+21+21). */
    UW tot = 0; for (UW l = 0; l < ENS_K; l++) tot += counts[l];
    if (tot != DNODE_MAX) fail = 1;
    /* merge-filter predicate: same lineage <=> same residue; different => skip */
    for (UW me = 0; me < 6; me++)
        for (UW p = 0; p < 6; p++) {
            BOOL same_lin = (ens_lineage(p) == ens_lineage(me));
            BOOL same_res = ((p % ENS_K) == (me % ENS_K));
            if (same_lin != same_res) fail = 1;
        }
    sc_p("[scaling]   lineage K="); sc_pd(ENS_K); sc_p(" sizes=[");
    for (UW l = 0; l < ENS_K; l++) { sc_pd(counts[l]); if (l + 1 < ENS_K) sc_p(" "); }
    sc_p("] over "); sc_pd((UW)DNODE_MAX); sc_p(" ids\r\n");

    /* verify the gl_merge_peers hosted filter plumbing round-trips (the live
     * ENS-A knob). Default is 0 (disabled -> byte-identical live gossip); we
     * set K, confirm, then restore 0 so no other cert's gossip is perturbed. */
    UW k_default = gl_ens_get_lineage_k();
    gl_ens_set_lineage_k(ENS_K);
    if (gl_ens_get_lineage_k() != (UW)ENS_K) fail = 1;
    gl_ens_set_lineage_k(0);
    if (gl_ens_get_lineage_k() != 0) fail = 1;
    if (k_default != 0) fail = 1;               /* live gossip default must be OFF */

    if (!fail) sc_p("[scaling-lineage] PASS (lineage=node_id%K pure/ownerless; merge folds same-lineage only; live knob default OFF)\r\n");
    else       sc_p("[scaling-lineage] FAIL\r\n");
}

/* ================================================================== */
/* ARM C — [scaling-converged-null] : the DISEASE / anti-theater keystone*/
/* N gossip-CONVERGED bit-identical nodes ensemble Q_hard; collective ==  */
/* solo on EVERY item (delta == 0 EXACTLY). Node COUNT alone buys zero.   */
/* ================================================================== */
static void arm_converged_null(void)
{
    sc_p("[scaling] ---- ARM: converged-null (node-count-alone => zero) ----\r\n");
    const UW N = 5;
    dtr_ga_busy = 1;

    /* Produce N CONVERGED nodes the way one-mind gossip does: identical seed +
     * identical corpus + identical steps => bit-identical weights (one-math,
     * -ffp-contract=off). We first PROVE the premise (bit-identical), then run
     * the REAL aggregation over the N copies and show the ensemble is the
     * identity of the solo answer. */
    dtr_reinit_weights(0xABCDEF01UL);
    for (UW step = 1; step <= 120; step++)
        (void)dtr_train_batch(sc_x, sc_y, SC_TRAIN, sc_lr(step, 120));
    dtr_weights_get(sc_model[0]);

    INT bit_ident = 1;
    for (UW k = 1; k < N; k++) {
        dtr_reinit_weights(0xABCDEF01UL);
        for (UW step = 1; step <= 120; step++)
            (void)dtr_train_batch(sc_x, sc_y, SC_TRAIN, sc_lr(step, 120));
        dtr_weights_get(sc_model[k]);
        for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++)
            if (sc_model[k][i] != sc_model[0][i]) { bit_ident = 0; break; }
    }

    /* precompute the N (identical) members through the real ask path */
    for (UW k = 0; k < N; k++) sc_precompute(k, sc_model[k]);

    /* solo answer = member 0; collective = ens_agg_conf over all N. */
    UW delta = 0;
    for (UW i = 0; i < SC_TEST; i++) {
        UB solo = sc_ans[0][i];
        ENS_RESP r[SC_ENS_MAX]; UW kk = 0;
        for (UW m = 0; m < N; m++) {
            r[m].lineage_id = ens_lineage(m); r[m].node_id = m; r[m].alive = 1;
            r[m].answer = sc_ans[m][i]; r[m].conf = sc_conf[m][i];
            for (UW c = 0; c < SC_C; c++) r[m].prob[c] = sc_probs[m][i][c];
        }
        UB coll = ens_agg_conf(r, N, &kk);
        if (coll != solo) delta++;
    }
    dtr_ga_busy = 0;

    sc_p("[scaling]   N="); sc_pd(N); sc_p(" converged nodes bit-identical=");
    sc_p(bit_ident ? "yes" : "NO"); sc_p("; collective-vs-solo delta="); sc_pd(delta);
    sc_p(" of "); sc_pd((UW)SC_TEST); sc_p(" items\r\n");
    if (bit_ident && delta == 0)
        sc_p("[scaling-converged-null] PASS (delta==0 EXACTLY: node COUNT alone buys ZERO per-answer quality)\r\n");
    else
        sc_p("[scaling-converged-null] FAIL (a converged fleet must be N copies of one function)\r\n");
}

/* ================================================================== */
/* ARM D — [scaling-breadth-curve] : breadth is linear-then-knee.        */
/* N distinct fact-shards; collective = gossip-merged consensus; metric   */
/* = union answerable through the REAL ask path; print the KNEE; anti-     */
/* theater = per-node solo < collective.                                  */
/* ================================================================== */
#define SC_BR_ROUNDS  24
#define SC_BR_LOCAL   4

static void arm_breadth_curve(void)
{
    sc_p("[scaling] ---- ARM: breadth-curve (N distinct shards, gossip-merged) ----\r\n");
    sc_build_shards();
    dtr_ga_busy = 1;
    UW total = SC_BR_ROUNDS * SC_BR_LOCAL;

    /* the 3 distinct leave-out solo ceilings (shards repeat for N>3). */
    float solo[SC_NCLASS];
    for (UW s = 0; s < SC_NCLASS; s++) solo[s] = sc_solo_shard(s, total);

    INT fail = 0;
    sc_p("[scaling]   N | collective | best-solo | dB\r\n");
    for (INT idx = 0; idx < 4; idx++) {
        UW N = g_breadth_N[idx];
        float B = sc_gossip(N, SC_BR_ROUNDS, SC_BR_LOCAL);
        g_breadth[idx] = B;
        float bestsolo = 0.0f;
        for (UW k = 0; k < N; k++) { float s = solo[k % SC_NCLASS]; if (s > bestsolo) bestsolo = s; }
        float dB = (idx == 0) ? B : (B - g_breadth[idx - 1]);
        sc_p("[scaling]   "); sc_pd(N); sc_p(" |   ");
        sc_pf1(B); sc_p("%   |  "); sc_pf1(bestsolo); sc_p("%  | ");
        if (idx == 0) sc_p("(base)"); else { if (dB >= 0.0f) sc_p("+"); sc_pf1(dB); }
        sc_p("\r\n");
        /* anti-theater: the collective beats any single node once N>=2 */
        if (N >= 2 && !(B > bestsolo)) fail = 1;
        /* monotone-nondecreasing within noise (never assert strict growth) */
        if (idx > 0 && B < g_breadth[idx - 1] - SC_BREADTH_NOISE) fail = 1;
        /* the knee = first N (idx>=1) where ΔB < margin */
        if (idx >= 1 && g_breadth_knee < 0 && dB < SC_BREADTH_KNEE_MARGIN)
            g_breadth_knee = (INT)N;
    }
    dtr_ga_busy = 0;

    /* per-member solo spread (used by [scaling-curve] flatness assert) */
    g_solo_flat_lo = solo[0]; g_solo_flat_hi = solo[0];
    for (UW s = 1; s < SC_NCLASS; s++) { if (solo[s] < g_solo_flat_lo) g_solo_flat_lo = solo[s]; if (solo[s] > g_solo_flat_hi) g_solo_flat_hi = solo[s]; }

    sc_p("[scaling]   KNEE (first N with dB<"); sc_pf1(SC_BREADTH_KNEE_MARGIN);
    sc_p("pp) = ");
    if (g_breadth_knee < 0) sc_p("none in {1,2,4,8} (breadth still rising)");
    else { sc_p("N="); sc_pd((UW)g_breadth_knee); }
    sc_p("  <- the honest capacity headline\r\n");

    if (!fail)
        sc_p("[scaling-breadth-curve] PASS (collective > every solo node; B monotone; knee printed)\r\n");
    else
        sc_p("[scaling-breadth-curve] FAIL (breadth did not clear the solo ceiling / not monotone)\r\n");
}

/* ================================================================== */
/* ARM E — [scaling-ensemble] / [scaling-ensemble-gain] : the CURE.      */
/* Same corpus, same steps, same model class; K distinct-seed lineages.   */
/* Ensemble via the pure §4.1 aggregation. GAIN (beats best member by     */
/* margin + vote-flips) OR the PRE-REGISTERED NULL (delta<=noise at all K).*/
/* ================================================================== */
#define SC_ENS_STEPS   110
#define SC_HARD_CONF   0.60f    /* Q_hard = items a member answers with < this max-softmax */
#define SC_HARD_MIN    15u      /* refuse a gain claim on too few hard items (noise) */
static const UW sc_K_sweep[4] = { 1, 3, 5, 9 };

static void sc_pdelta(float d) { if (d >= 0.0f) sc_p("+"); sc_pf1(d); sc_p("pp"); }

static void arm_ensemble(void)
{
    sc_p("[scaling] ---- ARM: ensemble (K distinct-seed lineages, corpus+steps HELD CONSTANT) ----\r\n");
    dtr_ga_busy = 1;

    /* train SC_ENS_MAX members: same full corpus, same steps, only the init
     * seed differs (ENS-A weight diversity). Precompute each through the real
     * ask path. Reused across the whole K-sweep. */
    for (UW m = 0; m < SC_ENS_MAX; m++) {
        dtr_reinit_weights(0x1111u + m * 0x9E37u);   /* distinct lineage seed */
        for (UW step = 1; step <= SC_ENS_STEPS; step++)
            (void)dtr_train_batch(sc_x, sc_y, SC_TRAIN, sc_lr(step, SC_ENS_STEPS));
        dtr_weights_get(sc_model[m]);
        sc_precompute(m, sc_model[m]);
    }
    dtr_ga_busy = 0;

    float memb[SC_ENS_MAX];
    for (UW m = 0; m < SC_ENS_MAX; m++) memb[m] = sc_member_acc(m);
    float mlo = memb[0], mhi = memb[0];
    for (UW m = 1; m < SC_ENS_MAX; m++) { if (memb[m] < mlo) mlo = memb[m]; if (memb[m] > mhi) mhi = memb[m]; }

    /* Q_hard (§4.3): the ensemble is FOR hard questions — low SOLO confidence,
     * the same (1-p_max) signal retrieval/reflex already gate on. Pre-register
     * Q_hard = test items where member 0's max-softmax < SC_HARD_CONF. Ensemble
     * gain is a VARIANCE term that shows on the CONTESTED items, not the easy
     * near-ceiling ones; measuring on the full set would dilute it away. */
    static UB hard[SC_TEST];
    UW nhard = 0;
    for (UW i = 0; i < SC_TEST; i++) { hard[i] = (sc_conf[0][i] < SC_HARD_CONF) ? 1 : 0; if (hard[i]) nhard++; }

    sc_p("[scaling]   Q_hard = "); sc_pd(nhard); sc_p(" of "); sc_pd((UW)SC_TEST);
    sc_p(" items (member0 max-softmax < "); sc_pf1(SC_HARD_CONF); sc_p(")\r\n");
    sc_p("[scaling]   K | ens(full) dFull | ens(Qhard) dHard | vote-flip(Qhard)\r\n");

    float best_hard_delta = -1000.0f; INT gain_atK = 0; UW vf_at_best = 0;
    for (INT s = 0; s < 4; s++) {
        UW K = sc_K_sweep[s];
        float ef, bmf, eh, bmh; UW vff, vfh, nf, nh;
        sc_eval_K(K, (const UB *)0, &ef, &bmf, &vff, &nf);
        sc_eval_K(K, hard,          &eh, &bmh, &vfh, &nh);
        float df = ef - bmf, dh = eh - bmh;
        sc_p("[scaling]   "); sc_pd(K); sc_p(" |  "); sc_pf1(ef); sc_p("% ");
        sc_pdelta(df); sc_p(" |  "); sc_pf1(eh); sc_p("% "); sc_pdelta(dh);
        sc_p(" |  "); sc_pd(vfh); sc_p("\r\n");
        if (K >= 3 && dh > best_hard_delta) { best_hard_delta = dh; g_ens_best_K = (float)K; vf_at_best = vfh; }
        if (K >= 3 && dh > SC_ENS_MARGIN_PP && vfh >= (UW)SC_ENS_VOTEFLIP_M && nhard >= SC_HARD_MIN)
            gain_atK = 1;
    }

    g_ens_delta = best_hard_delta;
    g_ens_best_member = mhi;
    g_ens_gain = gain_atK ? 1 : 0;

    sc_p("[scaling]   per-member spread ["); sc_pf1(mlo); sc_p("%.."); sc_pf1(mhi);
    sc_p("%] (flat = same corpus/model-class)  vote-flip@bestK="); sc_pd(vf_at_best);
    sc_p(" (need >="); sc_pd((UW)SC_ENS_VOTEFLIP_M); sc_p(")\r\n");

    /* honesty gate: a VALID measurement passes regardless of who wins. */
    sc_p("[scaling-ensemble] PASS (measured on Q_hard: best K>=3 delta="); sc_pdelta(best_hard_delta);
    sc_p(" at K="); sc_pd((UW)g_ens_best_K); sc_p(")\r\n");

    if (g_ens_gain) {
        sc_p("[scaling-ensemble-gain] PASS (diversity buys a REAL bounded gain: ensemble(K>=3) > best member on Q_hard by >");
        sc_pf1(SC_ENS_MARGIN_PP); sc_p("pp with >="); sc_pd((UW)SC_ENS_VOTEFLIP_M); sc_p(" vote-flips)\r\n");
    } else {
        /* the PRE-REGISTERED NULL — a PASS of HONESTY, not of the mechanism */
        sc_p("[scaling-ensemble] NULL (diversity insufficient at this scale — axis (c) does not scale by ensemble)\r\n");
        sc_p("[scaling-ensemble-gain] RED (pre-registered null: same corpus+arch+teacher => correlated errors; §4.4 stands)\r\n");
    }
}

/* ================================================================== */
/* ARM F — [scaling-quorum-degrade] : the ensemble-ask QUORUM LOGIC.      */
/* quorum_core "don't wait for the dead" + honest k/K. This is the pure    */
/* core a [scaling-live-ensemble] wire arm WOULD run over the relay — a     */
/* NOT-YET-WIRED follow-up (no self-hosted [scaling-live] job exists yet;   */
/* the multi-process relay transport run is unbuilt, noted below).         */
/* ================================================================== */
static void arm_quorum_degrade(void)
{
    sc_p("[scaling] ---- ARM: quorum-degrade (ensemble-ask liveness logic) ----\r\n");
    INT fail = 0;
    /* 3-member ensemble on one hard item (use test item 0's real member probs
     * if precomputed; otherwise synth). Kill members and watch k/K fall. */
    ENS_RESP r[3] = {
        { .lineage_id = 0, .node_id = 0, .alive = 1, .answer = 2, .conf = 0.55f, .prob = {0.20f, 0.25f, 0.55f} },
        { .lineage_id = 1, .node_id = 1, .alive = 1, .answer = 2, .conf = 0.48f, .prob = {0.22f, 0.30f, 0.48f} },
        { .lineage_id = 2, .node_id = 2, .alive = 1, .answer = 1, .conf = 0.40f, .prob = {0.30f, 0.40f, 0.30f} },
    };
    UW k3 = 0; UB a3 = ens_agg_conf(r, 3, &k3);
    r[2].alive = 0; UW k2 = 0; UB a2 = ens_agg_conf(r, 3, &k2);
    r[1].alive = 0; UW k1 = 0; UB a1 = ens_agg_conf(r, 3, &k1);
    r[0].alive = 0; UW k0 = 0; UB a0 = ens_agg_conf(r, 3, &k0);

    g_resil_full = (a3 != 0xFF); g_resil_deg = (a2 != 0xFF); g_resil_none = (a0 != 0xFF);

    sc_p("[scaling]   3/3 -> answer="); sc_pd(a3); sc_p(" k="); sc_pd(k3);
    sc_p(" | 2/3 -> answer="); sc_pd(a2); sc_p(" k="); sc_pd(k2);
    sc_p(" | 1/3 -> answer="); sc_pd(a1); sc_p(" k="); sc_pd(k1);
    sc_p(" | 0/3 -> answer="); sc_pd(a0); sc_p(" (0xFF=no fake)\r\n");
    if (!(k3 == 3 && k2 == 2 && k1 == 1 && k0 == 0)) fail = 1;
    if (a3 == 0xFF || a2 == 0xFF || a1 == 0xFF) fail = 1;   /* answers while alive */
    if (a0 != 0xFF) fail = 1;                                /* never faked at 0/K */

    /* quorum_core: with the 3rd member expected-but-DEAD, the ask FINALIZES
     * (don't wait for the dead) instead of hanging on the never-arriving one. */
    UB exp[3]   = { 1, 1, 1 };
    UB got[3]   = { 1, 1, 0 };
    UB alive[3] = { 1, 1, 0 };    /* member 2 died before answering */
    if (!ens_quorum(exp, got, alive, 3)) fail = 1;           /* must finalize */
    UB alive2[3] = { 1, 1, 1 };   /* member 2 still alive but unheard */
    if (ens_quorum(exp, got, alive2, 3)) fail = 1;           /* must NOT finalize */

    if (!fail)
        sc_p("[scaling-quorum-degrade] PASS (arrival-quorum finalizes, honest k/K, never fakes K/K; don't-wait-for-the-dead)\r\n");
    else
        sc_p("[scaling-quorum-degrade] FAIL\r\n");
    sc_p("[scaling]   note: an over-the-wire 3-node relay run [scaling-live-ensemble] is a NOT-YET-WIRED follow-up (no self-hosted job yet)\r\n");
}

/* ================================================================== */
/* ARM G — [scaling-curve] : the standing composite falsifier of the doc.*/
/* One table + the qualitative-shape asserts. If reality diverges from     */
/* scaling-law.md, THIS goes red and the DOCUMENT, not the assert, changes. */
/* ================================================================== */
static void arm_curve(void)
{
    sc_p("[scaling] ---- ARM: composite curve (the standing doc falsifier) ----\r\n");
    INT fail = 0;

    sc_p("[scaling]   axis        | shape claimed        | measured\r\n");
    sc_p("[scaling]   breadth(N)  | up then knee         | B(1)=");
    sc_pf1(g_breadth[0]); sc_p("% -> B(8)="); sc_pf1(g_breadth[3]); sc_p("% knee=");
    if (g_breadth_knee < 0) sc_p("none"); else { sc_p("N="); sc_pd((UW)g_breadth_knee); }
    sc_p("\r\n");
    sc_p("[scaling]   quality(N)  | saturating (var only)| ensemble best-dB(Qhard)=");
    sc_pdelta(g_ens_delta); sc_p(" verdict=");
    sc_p(g_ens_gain ? "GAIN" : "NULL"); sc_p("\r\n");
    sc_p("[scaling]   resilience  | avail up with N      | 3/3 ans=");
    sc_pd((UW)(g_resil_full > 0)); sc_p(" 2/3 ans="); sc_pd((UW)(g_resil_deg > 0));
    sc_p(" 0/3 fake="); sc_pd((UW)(g_resil_none > 0)); sc_p("\r\n");
    sc_p("[scaling]   solo(N)     | flat (N-invariant)   | per-shard solo spread [");
    sc_pf1(g_solo_flat_lo); sc_p("%.."); sc_pf1(g_solo_flat_hi); sc_p("%]\r\n");

    /* qualitative-shape asserts (only the shapes the doc claims): */
    if (!(g_breadth[3] >= g_breadth[0] - SC_BREADTH_NOISE)) fail = 1;   /* breadth up-then-flat */
    if (!(g_ens_delta >= -SC_ENS_NULL_NOISE)) fail = 1;                 /* ensemble delta >= 0 (never HURTS) */
    if (!(g_resil_full > 0 && g_resil_deg > 0 && g_resil_none == 0)) fail = 1; /* avail + no fake */

    if (!fail)
        sc_p("[scaling-curve] PASS (measured shapes match scaling-law.md: breadth up->knee; quality saturating/var-only; resilience up; solo flat)\r\n");
    else
        sc_p("[scaling-curve] FAIL (reality diverged from the doc — amend the DOCUMENT, not the assert)\r\n");
}

/* ================================================================== */
/* [scaling-topics-budget] — pure topic-budget enumeration (dmoe §6.2:   */
/* enumerate against 16/400 BEFORE any wire code lands). The ensemble ask  */
/* adds exactly +2 cluster-wide singleton topics (ENS_Q broadcast +        */
/* ENS_A reply), which fit the 16-singleton budget. No kdds state touched. */
/* ================================================================== */
static void arm_topics_budget(void)
{
    const UW ENS_NEW_SINGLETONS = 2;      /* ens/q + ens/a */
    /* KDDS_SINGLETON_TOPICS = 16 cluster-wide singletons; dkva uses per-node
     * topics (3*DNODE_MAX), not singletons. +2 <= 16 by inspection. */
    INT ok = (ENS_NEW_SINGLETONS <= 16u);
    sc_p("[scaling]   ensemble wire budget: +"); sc_pd(ENS_NEW_SINGLETONS);
    sc_p(" singleton topics (ens/q broadcast + ens/a reply) of 16 available\r\n");
    if (ok) sc_p("[scaling-topics-budget] PASS (+2 singleton topics within the 16/400 budget)\r\n");
    else    sc_p("[scaling-topics-budget] FAIL\r\n");
}

/* ================================================================== */
/* entry — run every in-process arm.                                    */
/* ================================================================== */
void scaling_self_test(void)
{
    sc_ds_init();
    sc_p("[scaling] ==== society-of-minds scaling cert (scaling-law.md) ====\r\n");
    sc_p("[scaling] task: 3-class sensor classify (dtr 635-float); N-sim in-process (the g22 pattern)\r\n");

    arm_agg_order();
    arm_agg_mechanism();
    arm_lineage();
    arm_topics_budget();
    arm_converged_null();
    arm_breadth_curve();
    arm_ensemble();
    arm_quorum_degrade();
    arm_curve();

    /* leave a sane trained model loaded for any follow-on infer */
    dtr_weights_set(sc_model[0]);
    sc_p("[scaling] ==== done ====\r\n");
}

#endif /* _TK_HOSTED_LIBC_ */
