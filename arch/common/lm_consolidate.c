/*
 *  lm_consolidate.c -- living-mind first slice: DMN sleep-consolidation.
 *
 *  See lm_consolidate.h + docs/architecture/living-mind.md Part II.
 *
 *  THE CLAIM (II.1): a rest-time ("sleep") consolidation that REPLAYS
 *  stored engrams and DISTILLS them into the dtr weights via the G22
 *  no-central merge lets the mind learn a STREAM of tasks WITHOUT
 *  catastrophic forgetting, decentralized, surviving node death + rejoin.
 *
 *  TASK INSTANTIATION: (B) region-shift (II.2). Each task occupies a
 *  DISJOINT region of input space (a per-task offset that REGION-ENTANGLES
 *  the discriminative level, so no region-invariant class feature exists).
 *  A per-task label-permutation table is kept (LM_PERM) but left identity
 *  here: the region entanglement alone produces catastrophic forgetting,
 *  and identity labels keep the in-region signal monotone (strong, honest
 *  single-task learning). We pick (B) over (A) class-permutation and
 *  honestly: class-permutation reuses the IDENTICAL input distribution,
 *  so a replayed task-0 engram (X, y0) directly CONTRADICTS the current
 *  task's (same X, y_t) -- the model cannot satisfy both and replay
 *  cannot cure forgetting (it would only confuse the model to ~chance).
 *  Region-shift keeps task regions disjoint, so engrams never contradict
 *  later data: replay restores the past AND plasticity survives. Same
 *  input/output dims as the sensor brain (DTR_SEQ_LEN=4 in, 3 classes
 *  out): the 635-param dtr body is reused UNCHANGED -- no architecture
 *  change (that is the Evolution layer, out of scope II.10).
 *
 *  ANTI-FORK (II.7): no math is duplicated. Training/eval/weights come
 *  from dtr.h; the consolidation merge IS gl_merge from gossip_learn.h;
 *  the durable engram store IS pfs_dag_save/read. We only orchestrate.
 */

#include "lm_consolidate.h"
#include "dtr.h"            /* dtr_train_batch/eval/reinit/get/set/grad_check */
#include "gossip_learn.h"   /* gl_merge -- the no-central consolidation merge  */
#include "reflex.h"         /* G38 earned salience (Part IV): reflex_on_inference,
                            * reflex_threat_experience, guard_class_exp save/restore */
#include "drpc.h"           /* drpc_my_node (src for reflex_on_inference)      */
#include "pfs_block.h"
#include "pfs_dag.h"        /* durable engram store (II.4)                    */
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* output helpers (sio frame channel, like gossip_learn.c)             */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void lp(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}
static void lpd(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { lp("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    lp(&buf[i]);
}
/* xx.x percent / loss */
static void lpf1(float f)
{
    if (f < 0.0f) { lp("-"); f = -f; }
    UW whole = (UW)f;
    UW frac  = (UW)((f - (float)whole) * 10.0f + 0.5f);
    if (frac >= 10) { whole++; frac = 0; }
    lpd(whole); lp("."); lpd(frac);
}
/* x.xxx finer print for gradient errors */
static void lpf3(float f)
{
    if (f < 0.0f) { lp("-"); f = -f; }
    UW whole = (UW)f;
    UW frac  = (UW)((f - (float)whole) * 1000.0f + 0.5f);
    if (frac >= 1000) { whole++; frac = 0; }
    lpd(whole); lp(".");
    if (frac < 100) lp("0");
    if (frac < 10)  lp("0");
    lpd(frac);
}

/* ------------------------------------------------------------------ */
/* the task stream (II.2): T region-shifted, label-permuted tasks       */
/* ------------------------------------------------------------------ */

#define LM_T        3                 /* tasks in the stream            */
#define LM_NCLASS   DTR_OUT_DIM       /* 3 classes                      */
#define LM_NTR      192               /* per-task train episodes        */
#define LM_NTE      60                /* per-task held-out episodes      */
#define LM_SEED     0x5EED2026UL      /* same dataset on every node/ABI */

/* B_RING -- the honesty knob (II.6). Engrams kept PER TASK, B_RING <<
 * |task data|. PRINTED at test time with the ratio so the auditor can
 * confirm the ring is genuinely small (not joint-training in disguise). */
#define B_RING      24                /* engrams per task               */

_Static_assert(B_RING * 4 < LM_NTR, "B_RING must be << per-task data");

/* ── salience-weighted replay (living-mind.md Part IV) ───────────────────
 * The danger class is the MIDDLE/HARDEST class (reflex 'alert' = class 1):
 * lm_gen places class centers at 0/14/28, so the extreme classes (0,2)
 * overlap on one side only (easier) while the middle class (1) overlaps on
 * both sides (Bayes-error highest). Retaining the HARD class better is a
 * genuine win, not metric saturation (IV.6 option (a)). The safe baseline
 * is class 0, whose act_table[0]==NONE so it never accrues guard experience
 * (classw[safe] stays 1). LM_WMAX mirrors GL_WMAX (gossip_learn.c:567). */
#define LM_DANGER   1                 /* danger = middle/hardest (reflex alert) */
#define LM_SAFE     0                 /* act_table NONE -> never earns exp       */
#define LM_WMAX     3                 /* mirrors GL_WMAX; max per-class weight    */

/* per-task class label permutation pi_t (II.2 (A)-style permutation, but
 * applied within (B) disjoint regions). Distinct, contradictory mappings
 * so sequential training really overwrites earlier competence. */
static const UB LM_PERM[LM_T][LM_NCLASS] = {
    { 0, 1, 2 },     /* task 0 */
    { 0, 1, 2 },     /* task 1 (identity: see lm_gen note on region-entangled) */
    { 0, 1, 2 },     /* task 2 */
};

/* per-task region offset on the discriminative channel (II.2 (B)). The
 * three regions are disjoint (gaps survive the observation noise), so a
 * region-0 engram never falls in region-t's input range -> no label
 * contradiction under replay; yet a model calibrated to the latest
 * region misreads the earlier ones -> catastrophic forgetting is real. */
static const INT LM_OFF[LM_T] = { -65, 0, 65 };

/* the stream, materialized once. Static (never on the task stack). */
static B  lm_trx[LM_T][LM_NTR][DTR_SEQ_LEN];
static UB lm_try[LM_T][LM_NTR];
static B  lm_tex[LM_T][LM_NTE][DTR_SEQ_LEN];
static UB lm_tey[LM_T][LM_NTE];
static UB lm_ready = 0;

static UW lm_rng;
static UW lm_rand(void)
{
    lm_rng = lm_rng * 1664525UL + 1013904223UL;
    return (lm_rng >> 16) & 0x7FFF;
}
static INT lm_uniform(INT lo, INT hi)
{
    return lo + (INT)(lm_rand() % (UW)(hi - lo + 1));
}
/* approx-gaussian noise: sum of 4 uniforms in [-s,s], halved (sigma ~ 0.58s) */
static INT lm_noise(INT s)
{
    INT v = 0;
    for (INT i = 0; i < 4; i++) v += lm_uniform(-s, s);
    return v / 2;
}
static B lm_clamp(INT v)
{
    if (v >  127) v =  127;
    if (v < -128) v = -128;
    return (B)v;
}

/* one episode for task t, class c. The discriminative channel 0 carries
 * a region-shifted, class-graded latent (so the decision surface is
 * region-SPECIFIC); the other three channels are pure distractors. The
 * sub-band width + observation noise overlap at band edges so the task
 * is learnable but NOT separable to 100% (Bayes error > 0), the same
 * spirit as the dtr_train.c / gossip_learn.c sensor family. */
static void lm_gen(UB t, UB c, B out[DTR_SEQ_LEN], UB *label_out)
{
    /* class-graded latent (centers 0/14/28, width ~8 -> overlap at edges so
     * the task is learnable but NOT separable to 100%, the dtr_train.c
     * spirit) plus a per-task REGION offset. The discriminative level is
     * REGION-ENTANGLED: the same latent reads as a different absolute value
     * in each task region, the regions are disjoint, and the offset cannot
     * be cancelled by any channel difference (the three signal channels are
     * identical up to independent noise, so a difference cancels the signal
     * too). So there is NO region-invariant class feature -> a model
     * calibrated to the latest region MISREADS the earlier ones
     * (catastrophic forgetting is real), yet an earlier-region engram never
     * contradicts later data (disjoint regions -> replay stays clean). */
    INT sig = (INT)c * 14 + lm_uniform(-4, 4);
    INT lvl = LM_OFF[t] + sig;
    out[0] = lm_clamp(lvl + lm_noise(11));         /* signal token 1        */
    out[1] = lm_clamp(lvl + lm_noise(11));         /* signal token 2        */
    out[2] = lm_clamp(lvl + lm_noise(11));         /* signal token 3        */
    out[3] = lm_clamp(lm_uniform(-30, 30));        /* pure distractor       */
    *label_out = LM_PERM[t][c];                    /* per-task label map     */
}

static void lm_ds_init(void)
{
    if (lm_ready) return;
    lm_rng = LM_SEED;
    for (UB t = 0; t < LM_T; t++) {
        for (INT i = 0; i < LM_NTR; i++)
            lm_gen(t, (UB)(i % LM_NCLASS), lm_trx[t][i], &lm_try[t][i]);
        for (INT i = 0; i < LM_NTE; i++)
            lm_gen(t, (UB)(i % LM_NCLASS), lm_tex[t][i], &lm_tey[t][i]);
    }
    lm_ready = 1;
}

/* held-out accuracy (%) of task t under the currently-loaded weights.
 * Measured on FRESH episodes (II.6), never the replayed engrams. */
static float lm_acc(UB t)
{
    UW correct = 0;
    (void)dtr_eval_batch(lm_tex[t], lm_tey[t], LM_NTE, &correct);
    return (float)correct * 100.0f / (float)LM_NTE;
}

/* per-class held-out accuracy (%), AGGREGATED ACROSS ALL TASKS (the salience
 * weight is class-global), on FRESH episodes (lm_tex/lm_tey), never the
 * replayed engrams (IV.4/IV.6). A held-out episode i has class i%LM_NCLASS
 * (lm_ds_init generates round-robin) and identity label, so we argmax
 * dtr_forward_probs and compare. Reuses only shared dtr kernels (no fork). */
static float lm_acc_class(UB cls)
{
    UW correct = 0, total = 0;
    for (UB t = 0; t < LM_T; t++)
        for (INT i = 0; i < LM_NTE; i++) {
            if ((UB)(i % LM_NCLASS) != cls) continue;
            float p[DTR_OUT_DIM];
            dtr_forward_probs(lm_tex[t][i], p);
            UB arg = 0; float best = p[0];
            for (UB k = 1; k < DTR_OUT_DIM; k++) if (p[k] > best) { best = p[k]; arg = k; }
            if (arg == lm_tey[t][i]) correct++;
            total++;
        }
    return total ? (float)correct * 100.0f / (float)total : 0.0f;
}
/* macro (mean per-class) held-out accuracy (%). */
static float lm_acc_macro(void)
{
    float s = 0.0f;
    for (UB c = 0; c < LM_NCLASS; c++) s += lm_acc_class(c);
    return s / (float)LM_NCLASS;
}

/* Part IV (IV.7): derive the per-class replay weight from EARNED guard
 * experience using the SAME clamp formula G22 uses (gossip_learn.c:635) —
 * MIRRORED, not called (gl_build_weighted is file-static there and its
 * minibatch-inflation body would break our fixed B_RING budget). With every
 * exp[c]==0 this yields all-1 (the no-regress hinge). The single source of
 * truth for the weighting math in this file. */
static void lm_classw_from_exp(const UW exp[LM_NCLASS], UW out[LM_NCLASS])
{
    UW mx = 1;
    for (UB c = 0; c < LM_NCLASS; c++) if (exp[c] > mx) mx = exp[c];
    for (UB c = 0; c < LM_NCLASS; c++) {
        UW w = 1 + (exp[c] * ((UW)LM_WMAX - 1) + mx - 1) / mx;   /* mirror :635 */
        if (w < 1) w = 1;
        if (w > (UW)LM_WMAX) w = (UW)LM_WMAX;
        out[c] = w;
    }
}

/* ------------------------------------------------------------------ */
/* the engram ring (II.3) -- the "hippocampus"                          */
/* ------------------------------------------------------------------ */

static LM_ENGRAM lm_ring[LM_T][B_RING];
static UW        lm_ring_n[LM_T];          /* valid engrams per task     */

static void lm_ring_clear(void)
{
    for (UB t = 0; t < LM_T; t++) lm_ring_n[t] = 0;
}

/* per-class earned replay weight (living-mind.md IV). Default {1,1,1} =
 * NO earned salience -> lm_ring_capture is byte-identical to LM-1 uniform
 * (the no-regress hinge, IV.5 #3). The salience cert sets it from
 * reflex_threat_experience() via the mirrored G22 clamp (IV.7) and resets
 * it back to all-1 afterwards, so the live DMN path is never weighted. */
static UW lm_classw[LM_NCLASS] = { 1, 1, 1 };

/* capture B_RING engrams of task t into the ring.
 *
 * Part IV — SALIENCE-WEIGHTED, FIXED-BUDGET REALLOCATION. The ring still
 * holds EXACTLY B_RING engrams/task (budget unchanged; NOT a bigger ring,
 * NOT minibatch inflation — IV.2/IV.6). Salience changes only WHICH engrams
 * fill the fixed slots: the per-class share of the B_RING slots is
 * proportional to lm_classw[c] (danger gets more, low-salience fewer), and
 * each engram's salience field carries its EARNED class weight (a real
 * reflex-derived number, not the constant 1).
 *
 * NO-REGRESS HINGE: when every lm_classw[c]==1 (no earned experience) this
 * reduces to the EXACT LM-1 uniform stride — byte-identical engrams in the
 * same slots — so all [dmn-*] stay unchanged (IV.5 #3). (gl_build_weighted,
 * cited by the doc, is file-static in gossip_learn.c and its minibatch-
 * inflation loop would break the fixed budget; we MIRROR only its clamp
 * FORMULA, IV.7, not its body.) */
static void lm_ring_capture(UB t)
{
    /* no-regress hinge: no earned salience -> exact LM-1 uniform stride. */
    BOOL uniform = TRUE;
    for (UB c = 0; c < LM_NCLASS; c++) if (lm_classw[c] != 1) uniform = FALSE;
    if (uniform) {
        UW step = LM_NTR / B_RING; if (step == 0) step = 1;
        UW m = 0;
        for (UW i = 0; i < (UW)LM_NTR && m < B_RING; i += step) {
            for (INT k = 0; k < DTR_SEQ_LEN; k++) lm_ring[t][m].input[k] = lm_trx[t][i][k];
            lm_ring[t][m].label    = lm_try[t][i];
            lm_ring[t][m].task_id  = t;
            lm_ring[t][m].salience = 1;
            lm_ring[t][m]._pad     = 0;
            m++;
        }
        lm_ring_n[t] = m;
        return;
    }

    /* weighted: split the FIXED B_RING slots across classes proportional to
     * lm_classw (largest-remainder so the counts sum to EXACTLY B_RING). */
    UW wsum = 0;
    for (UB c = 0; c < LM_NCLASS; c++) wsum += lm_classw[c];
    UW target[LM_NCLASS], rem[LM_NCLASS], base_sum = 0;
    for (UB c = 0; c < LM_NCLASS; c++) {
        UW prod = lm_classw[c] * (UW)B_RING;
        target[c] = prod / wsum;
        rem[c]    = prod - target[c] * wsum;   /* fractional part * wsum */
        base_sum += target[c];
    }
    /* hand out the leftover slots (< LM_NCLASS) to the largest remainders;
     * ties -> lowest class index (deterministic across nodes/ABIs). */
    for (UW left = (UW)B_RING - base_sum; left > 0; left--) {
        UB best = 0;
        for (UB c = 1; c < LM_NCLASS; c++) if (rem[c] > rem[best]) best = c;
        target[best]++;
        rem[best] = 0;
    }

    /* fill: class c episodes are at train indices i = p*LM_NCLASS + c (the
     * dataset is generated round-robin, lm_gen(t, i%LM_NCLASS, ...)); pick
     * target[c] of them by an in-class stride. salience = the class weight. */
    UW m = 0;
    UW pool = (UW)LM_NTR / LM_NCLASS;          /* episodes per class */
    for (UB c = 0; c < LM_NCLASS; c++) {
        if (target[c] == 0) continue;
        UW step = pool / target[c]; if (step == 0) step = 1;
        UW picked = 0;
        for (UW p = 0; p < pool && picked < target[c] && m < B_RING; p += step) {
            UW i = p * LM_NCLASS + c;
            for (INT k = 0; k < DTR_SEQ_LEN; k++) lm_ring[t][m].input[k] = lm_trx[t][i][k];
            lm_ring[t][m].label    = lm_try[t][i];
            lm_ring[t][m].task_id  = t;
            lm_ring[t][m].salience = (UB)lm_classw[c];
            lm_ring[t][m]._pad     = 0;
            m++; picked++;
        }
    }
    lm_ring_n[t] = m;
}

BOOL lm_engrams_pending(void)
{
    for (UB t = 0; t < LM_T; t++) if (lm_ring_n[t]) return TRUE;
    return FALSE;
}

/* living-body inspector: the engram organ's fill gauge (see header). */
INT lm_ring_fill(void)
{
    UW n = 0;
    for (UB t = 0; t < LM_T; t++) n += lm_ring_n[t];
    return (INT)n;
}
INT lm_ring_cap(void) { return (INT)((UW)LM_T * (UW)B_RING); }

/* ------------------------------------------------------------------ */
/* replay-SGD: the consolidation training step                         */
/* ------------------------------------------------------------------ */

/* combined replay minibatch: current task's full online data + the
 * bounded engram rings of EARLIER tasks (II.3 "interleave a minibatch
 * from the ring into SGD"). Max size = LM_NTR + (LM_T-1)*B_RING. */
#define LM_CB_MAX  (LM_NTR + (LM_T - 1) * B_RING)
static B  lm_cb_x[LM_CB_MAX][DTR_SEQ_LEN];
static UB lm_cb_y[LM_CB_MAX];

/* step-decayed LR (same shape dtr_train.c / gossip_learn.c use, with a
 * finer tail so the full 9-cluster replay problem -- not just one region --
 * converges and the NEWEST task keeps its plasticity). */
static float lm_lr(UW step, UW total)
{
    if (step <= total / 2)     return 0.10f;
    if (step <= total * 3 / 4) return 0.05f;
    return 0.02f;
}

#define LM_EPOCHS  500          /* full-batch SGD epochs per task        */

/* train task t for LM_EPOCHS. with_replay!=0 interleaves earlier tasks'
 * engrams into every batch (consolidation); ==0 is the naive online
 * learner that forgets. Both run THROUGH dtr_train_batch -- no fork. */
static void lm_train_task(UB t, INT with_replay)
{
    UW n = 0;
    for (UW i = 0; i < (UW)LM_NTR; i++) {
        for (INT k = 0; k < DTR_SEQ_LEN; k++) lm_cb_x[n][k] = lm_trx[t][i][k];
        lm_cb_y[n] = lm_try[t][i];
        n++;
    }
    if (with_replay) {
        for (UB tp = 0; tp < t; tp++)
            for (UW j = 0; j < lm_ring_n[tp] && n < LM_CB_MAX; j++) {
                for (INT k = 0; k < DTR_SEQ_LEN; k++) lm_cb_x[n][k] = lm_ring[tp][j].input[k];
                lm_cb_y[n] = lm_ring[tp][j].label;
                n++;
            }
    }
    for (UW e = 1; e <= LM_EPOCHS; e++)
        (void)dtr_train_batch(lm_cb_x, lm_cb_y, n, lm_lr(e, LM_EPOCHS));
}

/* reconsolidate FROM ENGRAMS ONLY (the rejoin path II.5 #4): after a
 * kill the live task stream is gone; the node has only the durable
 * engram ring. Replay-train on the union of all rings. */
static void lm_reconsolidate_from_engrams(void)
{
    UW n = 0;
    for (UB t = 0; t < LM_T; t++)
        for (UW j = 0; j < lm_ring_n[t] && n < LM_CB_MAX; j++) {
            for (INT k = 0; k < DTR_SEQ_LEN; k++) lm_cb_x[n][k] = lm_ring[t][j].input[k];
            lm_cb_y[n] = lm_ring[t][j].label;
            n++;
        }
    if (n == 0) return;
    for (UW e = 1; e <= LM_EPOCHS; e++)
        (void)dtr_train_batch(lm_cb_x, lm_cb_y, n, lm_lr(e, LM_EPOCHS));
}

/* ------------------------------------------------------------------ */
/* durable engram store on p-fs (II.3/II.4): one named block holds the  */
/* whole fixed-size ring -> 1 ref (PFS_REF_MAX=8), not one ref/engram.   */
/* ------------------------------------------------------------------ */

#define LM_ENG_REF      "lm/engrams"
#define LM_ENG_REF_LEN  10
#define LM_ENG_MAGIC    0x474E454CUL    /* "LENG" LE */

static struct __attribute__((packed, aligned(4))) {
    UW        magic;                     /* LM_ENG_MAGIC                  */
    UW        count;                     /* total engrams that follow     */
    LM_ENGRAM e[LM_T * B_RING];
} lm_blob;

_Static_assert(sizeof(LM_ENGRAM) == 8, "engram must be 8 bytes");
_Static_assert(sizeof(lm_blob) == 8 + LM_T * B_RING * 8,
               "engram blob = header + fixed engram array");
_Static_assert(sizeof(lm_blob) <= PFS_BLOCK_MAX,
               "engram ring must fit one p-fs block (PFS_REF_MAX-friendly)");

/* save the whole ring as ONE versioned, region-replicated p-fs object. */
static INT lm_engrams_save(void)
{
    UW c = 0;
    for (UB t = 0; t < LM_T; t++)
        for (UW j = 0; j < lm_ring_n[t] && c < LM_T * B_RING; j++)
            lm_blob.e[c++] = lm_ring[t][j];
    lm_blob.magic = LM_ENG_MAGIC;
    lm_blob.count = c;
    INT r = pfs_dag_save((const UB *)LM_ENG_REF, LM_ENG_REF_LEN,
                         &lm_blob, (UW)sizeof(lm_blob));
    return (r == PFS_OK) ? 0 : -1;
}

/* restore the ring from p-fs (the rejoin reload, II.5 #4). Repartitions
 * engrams back into per-task ring slots by their stored task_id. */
static INT lm_engrams_restore(void)
{
    INT r = pfs_dag_read((const UB *)LM_ENG_REF, LM_ENG_REF_LEN,
                         &lm_blob, (UW)sizeof(lm_blob));
    if (r != (INT)sizeof(lm_blob)) return -1;
    if (lm_blob.magic != LM_ENG_MAGIC) return -1;
    lm_ring_clear();
    for (UW i = 0; i < lm_blob.count && i < LM_T * B_RING; i++) {
        UB t = lm_blob.e[i].task_id;
        if (t < LM_T && lm_ring_n[t] < B_RING) lm_ring[t][lm_ring_n[t]++] = lm_blob.e[i];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* shared starting weights so distributed merges are well-behaved early */
/* ------------------------------------------------------------------ */

#define LM_INIT_SEED  0xC0FFEE11UL      /* same shared seed as G22        */

/* ================================================================== */
/* the acceptance suite (living-mind.md II.5)                           */
/* ================================================================== */

static float lm_chance(void) { return 100.0f / (float)LM_NCLASS; }

/* run the whole NO-REPLAY stream from the shared seed; returns the
 * task-0 held-out accuracy right after task 0 (acc0_fresh) and after the
 * full stream (*end_out). The naive learner -- the disease. */
static float lm_run_noreplay(float *end_out, float *last_out)
{
    dtr_reinit_weights(LM_INIT_SEED);
    lm_train_task(0, 0);
    float fresh = lm_acc(0);
    for (UB t = 1; t < LM_T; t++) lm_train_task(t, 0);
    if (end_out)  *end_out  = lm_acc(0);
    if (last_out) *last_out = lm_acc(LM_T - 1);
    return fresh;
}

/* run the WITH-REPLAY (DMN consolidation) stream from the same seed,
 * capturing engrams after each task and interleaving earlier engrams
 * into later tasks. Returns task-0 held-out acc after the full stream;
 * *last_out = newest task acc (plasticity not sacrificed). Leaves the
 * ring POPULATED (the live state the survive test then persists). */
static float lm_run_replay(float *last_out)
{
    dtr_reinit_weights(LM_INIT_SEED);
    lm_ring_clear();
    for (UB t = 0; t < LM_T; t++) {
        lm_train_task(t, 1);
        lm_ring_capture(t);          /* DMN stores the day's episodes */
    }
    if (last_out) *last_out = lm_acc(LM_T - 1);
    return lm_acc(0);
}

/* Part IV: run the full LM-1 stream once under a given per-class replay
 * weight vector (uniform {1,1,1} or earned salience), capturing engrams
 * after each task. Same seed + same B_RING as lm_run_replay (the ONLY
 * change is the capture allocation). Counts the danger-class engrams that
 * landed in the fixed ring (*n_danger_out). Leaves lm_classw reset to
 * all-1 so the live DMN path is never left weighted. Leaves the trained
 * weights loaded so per-class held-out can be measured by the caller. */
static void lm_run_replay_classw(const UW classw[LM_NCLASS], UW *n_danger_out)
{
    for (UB c = 0; c < LM_NCLASS; c++) lm_classw[c] = classw[c];
    dtr_reinit_weights(LM_INIT_SEED);
    lm_ring_clear();
    for (UB t = 0; t < LM_T; t++) { lm_train_task(t, 1); lm_ring_capture(t); }
    if (n_danger_out) {
        UW nd = 0;
        for (UB t = 0; t < LM_T; t++)
            for (UW j = 0; j < lm_ring_n[t]; j++)
                if (lm_ring[t][j].label == LM_DANGER) nd++;
        *n_danger_out = nd;
    }
    for (UB c = 0; c < LM_NCLASS; c++) lm_classw[c] = 1;   /* restore uniform default */
}

void lm_test(void)
{
    INT fails = 0;
    float chance = lm_chance();

    lp("[dmn-test] ==== living-mind: DMN sleep-consolidation (living-mind.md II) ====\r\n");
    lp("[dmn-test] task stream: "); lpd(LM_T);
    lp(" region-shift tasks (instantiation B; region-entangled signal, identity label map); 3 classes; chance ");
    lpf1(chance); lp("%\r\n");
    lp("[dmn-test] engram ring B_RING="); lpd(B_RING);
    lp(" per task; |task data|="); lpd((UW)LM_NTR);
    lp("; ratio B_RING/|task| = "); lpd((UW)(B_RING * 1000 / LM_NTR));
    lp("/1000 ("); lpf1((float)B_RING * 100.0f / (float)LM_NTR);
    lp("% -- bounded, NOT joint training)\r\n");

    lm_ds_init();
    dtr_ga_busy = 1;

    /* ---- 1. [dmn-forgetting] -- the disease is real (precondition) ---- */
    float acc0_end_nr = 0.0f, acc_last_nr = 0.0f;
    float acc0_fresh = lm_run_noreplay(&acc0_end_nr, &acc_last_nr);
    float drop = acc0_fresh - acc0_end_nr;
    lp("[dmn-test] no-replay: acc0_fresh="); lpf1(acc0_fresh);
    lp("%  acc0_end="); lpf1(acc0_end_nr);
    lp("%  (drop "); lpf1(drop);
    lp(" pts)  acc_lastTask="); lpf1(acc_last_nr); lp("%\r\n");
    {
        INT ok = (drop >= 25.0f) && (acc0_end_nr <= chance + 15.0f);
        if (ok) {
            lp("[dmn-forgetting] PASS (sequential training collapsed task-0: drop ");
            lpf1(drop); lp(" pts >= 25 AND end "); lpf1(acc0_end_nr);
            lp("% <= chance+15)\r\n");
        } else {
            lp("[dmn-forgetting] FAIL (no real forgetting -- fix the task generator, not the bar)\r\n");
            fails++;
        }
    }

    /* ---- 2. [dmn-consolidated] -- replay cures it (headline) ---------- */
    float acc_last_rp = 0.0f;
    float acc0_end_rp = lm_run_replay(&acc_last_rp);
    float cure = acc0_end_rp - acc0_end_nr;
    lp("[dmn-test] with-replay: acc0_end_replay="); lpf1(acc0_end_rp);
    lp("%  acc_lastTask_replay="); lpf1(acc_last_rp);
    lp("%  (cure over no-replay = +"); lpf1(cure); lp(" pts)\r\n");
    {
        INT ok = (acc0_end_rp >= chance + 30.0f)
              && (cure >= 25.0f)
              && (acc_last_rp >= chance + 30.0f);
        if (ok) {
            lp("[dmn-consolidated] PASS (replay retained task-0 at ");
            lpf1(acc0_end_rp); lp("% >= chance+30, +"); lpf1(cure);
            lp(" over disease, newest task "); lpf1(acc_last_rp);
            lp("% -- plasticity kept)\r\n");
        } else {
            lp("[dmn-consolidated] FAIL\r\n");
            fails++;
        }
    }

    /* ---- 3. [dmn-distributed] -- no central consolidator ------------- */
    /* Build N consolidated models (distinct seeds), then run the EXACT
     * G22 no-central discipline on the consolidated gl_merge: fwd vs
     * reverse order |delta|=O(1e-6) rounding (not O(1) structural
     * privilege), and single-model merge = identity. Reuses gl_merge. */
    {
        #define LM_DNODES 3
        static float lm_model[LM_DNODES][DTR_WEIGHT_FLOATS];
        for (UW kk = 0; kk < LM_DNODES; kk++) {
            dtr_reinit_weights(LM_INIT_SEED + kk * 7919UL);
            lm_ring_clear();
            for (UB t = 0; t < LM_T; t++) { lm_train_task(t, 1); lm_ring_capture(t); }
            dtr_weights_get(lm_model[kk]);
        }
        const float *fwd[LM_DNODES], *rev[LM_DNODES];
        for (UW kk = 0; kk < LM_DNODES; kk++) {
            fwd[kk] = lm_model[kk];
            rev[kk] = lm_model[LM_DNODES - 1 - kk];
        }
        static float ma[DTR_WEIGHT_FLOATS], mb[DTR_WEIGHT_FLOATS];
        gl_merge(ma, fwd, LM_DNODES, DTR_WEIGHT_FLOATS);   /* node-0 order  */
        gl_merge(mb, rev, LM_DNODES, DTR_WEIGHT_FLOATS);   /* reverse order */
        float worst = 0.0f;
        for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) {
            float d = ma[i] - mb[i]; if (d < 0.0f) d = -d;
            if (d > worst) worst = d;
        }
        gl_merge(ma, fwd, 1, DTR_WEIGHT_FLOATS);           /* identity      */
        INT ident = 1;
        for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++)
            if (ma[i] != lm_model[0][i]) { ident = 0; break; }
        lp("[dmn-test] distributed: |merge(fwd)-merge(rev)| max=");
        lpf1(worst * 1000000.0f);
        lp("e-6 (rounding only; structural privilege would be O(1)); single-merge identity=");
        lp(ident ? "yes" : "no"); lp("\r\n");
        if (worst < 1e-4f && ident) {
            lp("[dmn-distributed] PASS (consolidated gl_merge is peer-symmetric / order-independent; no aggregator)\r\n");
        } else {
            lp("[dmn-distributed] FAIL (merge order mattered or identity broke -- a central consolidator exists)\r\n");
            fails++;
        }
        #undef LM_DNODES
    }

    /* ---- 4. [dmn-survive] -- engrams outlive a node ----------------- */
    /* Reconstruct the populated ring (a fresh replay run), PERSIST it to
     * p-fs, KILL (drop RAM weights + ring), RELOAD from p-fs on rejoin,
     * and reconsolidate FROM ENGRAMS ONLY. */
    {
        (void)lm_run_replay(0);                 /* repopulate ring in RAM   */
        INT saved = lm_engrams_save();          /* durable, region-repl.    */

        /* the kill: forget the learned weights AND wipe the RAM ring. */
        dtr_reinit_weights(LM_INIT_SEED);
        lm_ring_clear();
        float acc0_dead = lm_acc(0);            /* should be ~chance         */

        /* the rejoin: reload engrams from p-fs, reconsolidate. */
        INT restored = lm_engrams_restore();
        UW reloaded = 0;
        for (UB t = 0; t < LM_T; t++) reloaded += lm_ring_n[t];
        if (restored == 0) lm_reconsolidate_from_engrams();
        float acc0_rejoin = lm_acc(0);

        lp("[dmn-test] survive: save="); lp(saved == 0 ? "ok" : "ERR");
        lp(" acc0_after_kill="); lpf1(acc0_dead);
        lp("%  reloaded_engrams="); lpd(reloaded);
        lp(" acc0_after_rejoin="); lpf1(acc0_rejoin); lp("%\r\n");
        if (saved == 0 && restored == 0 && reloaded > 0
            && acc0_rejoin >= chance + 25.0f) {
            lp("[dmn-survive] PASS (engrams reloaded from p-fs after kill; reconsolidated task-0 to ");
            lpf1(acc0_rejoin); lp("% >= chance+25 -- the past came from durable storage, not RAM)\r\n");
        } else {
            lp("[dmn-survive] FAIL\r\n");
            fails++;
        }
    }

    /* ---- 5. [dmn-gradcheck] -- real gradients on the replay path ----- */
    {
        lm_ds_init();
        lm_run_replay(0);                       /* populate the ring        */
        /* grad-check a REPLAYED engram (input + its stored label), the
         * exact data the consolidation SGD trains on. */
        LM_ENGRAM *eg = &lm_ring[0][0];
        float ge = dtr_grad_check(eg->input, eg->label);
        lp("[dmn-test] gradcheck (analytic vs central finite diff) on a REPLAYED engram: max rel err ");
        lpf3(ge); lp("\r\n");
        if (ge < 0.05f) {
            lp("[dmn-gradcheck] PASS (replay trains real analytic gradients; rel err < 0.05)\r\n");
        } else {
            lp("[dmn-gradcheck] FAIL\r\n");
            fails++;
        }
    }

    /* ---- 6. [salience-*] -- salience-weighted replay (living-mind.md IV) -- */
    /* The DMN's third function: rehearse the survival-critical (danger) class
     * MORE under a FIXED replay budget. Salience is EARNED from real reflex
     * guard firings (not hand-set); the budget B_RING is unchanged; the gain
     * is a REALLOCATION tradeoff measured on FRESH held-out (IV.1/IV.6). */
    {
        #define LM_KFIRE  8     /* reflex guard firings to EARN salience */

        /* (a) EARN salience: drive K real reflex firings on the danger class
         * through the PUBLIC accrual hook (reflex_on_inference). Save/restore
         * the live G38 counters exactly as reflex_self_test (~ln 904) so we do
         * not pollute production experience; run with reflex enabled; observe
         * SAFE after to release the CONSERVE latch (IV.3). */
        UW sv_exp[REFLEX_NUM_CLASSES];
        reflex_guard_exp_save(sv_exp);
        BOOL sv_en = reflex_set_enabled(TRUE);
        {
            UW zero[REFLEX_NUM_CLASSES];
            for (INT c = 0; c < REFLEX_NUM_CLASSES; c++) zero[c] = 0;
            reflex_guard_exp_restore(zero);                  /* clean probe slate */
            for (UW f = 0; f < LM_KFIRE; f++)
                reflex_on_inference(LM_DANGER, 100, drpc_my_node);  /* alert fires */
            reflex_on_inference(LM_SAFE, 100, drpc_my_node);  /* SAFE: release latch */
        }
        UW exp_vec[LM_NCLASS];
        for (UB c = 0; c < LM_NCLASS; c++) exp_vec[c] = reflex_threat_experience(c);
        UW exp_danger = exp_vec[LM_DANGER];
        UW exp_safe   = exp_vec[LM_SAFE];

        /* derive classw via the MIRRORED G22 clamp formula (gossip_learn.c:635),
         * NOT a call (it is file-static + minibatch-inflation; IV.7). */
        UW classw[LM_NCLASS];
        lm_classw_from_exp(exp_vec, classw);

        /* restore live G38 counters + reflex enable state (no pollution). */
        reflex_guard_exp_restore(sv_exp);
        reflex_set_enabled(sv_en);

        /* uniform vs salience capture: SAME seed, SAME B_RING (IV.5 #2). */
        UW uniw[LM_NCLASS] = { 1, 1, 1 };
        UW n_danger_uni = 0, n_danger_sal = 0;

        lm_run_replay_classw(uniw, &n_danger_uni);
        float acc_d_uni = lm_acc_class(LM_DANGER);
        float acc_s_uni = lm_acc_class(LM_SAFE);
        float macro_uni = lm_acc_macro();

        lm_run_replay_classw(classw, &n_danger_sal);
        float acc_d_sal = lm_acc_class(LM_DANGER);
        float acc_s_sal = lm_acc_class(LM_SAFE);
        float macro_sal = lm_acc_macro();

        float dgain = acc_d_sal - acc_d_uni;     /* danger retained better? */
        float sloss = acc_s_uni - acc_s_sal;     /* safe class traded away  */

        lp("[salience] ==== salience-weighted replay (living-mind.md IV) ====\r\n");
        lp("[salience] danger class="); lpd(LM_DANGER);
        lp(" (middle/hardest, reflex 'alert'); safe class="); lpd(LM_SAFE);
        lp("; B_RING="); lpd(B_RING); lp(" (FIXED budget, reallocation only)\r\n");
        lp("[salience] EARNED experience: reflex_threat_experience(danger)=");
        lpd(exp_danger); lp(" > (safe)="); lpd(exp_safe);
        lp("  -> classw=["); for (UB c = 0; c < LM_NCLASS; c++) { lpd(classw[c]); if (c+1<LM_NCLASS) lp(","); }
        lp("] (WMAX="); lpd(LM_WMAX); lp(")\r\n");
        lp("[salience] danger engrams in ring: uniform="); lpd(n_danger_uni);
        lp("  salience="); lpd(n_danger_sal); lp(" (same B_RING)\r\n");
        lp("[salience] held-out danger: uniform="); lpf1(acc_d_uni);
        lp("%  salience="); lpf1(acc_d_sal); lp("%  (dgain "); lpf1(dgain); lp(" pts)\r\n");
        lp("[salience] held-out safe  : uniform="); lpf1(acc_s_uni);
        lp("%  salience="); lpf1(acc_s_sal); lp("%  (sloss "); lpf1(sloss); lp(" pts)\r\n");
        lp("[salience] macro held-out : uniform="); lpf1(macro_uni);
        lp("%  salience="); lpf1(macro_sal); lp("%\r\n");

        /* ---- [salience-earned] -- salience is EARNED, not hand-set (gate) -- */
        {
            INT ok = (exp_danger > exp_safe)
                  && (classw[LM_DANGER] > 1) && (classw[LM_SAFE] == 1)
                  && (n_danger_sal > n_danger_uni);
            if (ok) {
                lp("[salience-earned] PASS (reflex experience accrued ");
                lpd(exp_danger); lp(">"); lpd(exp_safe);
                lp("; classw[danger]="); lpd(classw[LM_DANGER]);
                lp(">1, classw[safe]=1; ring reallocated "); lpd(n_danger_uni);
                lp("->"); lpd(n_danger_sal); lp(" danger engrams at fixed B_RING)\r\n");
            } else {
                lp("[salience-earned] FAIL (salience not earned -- fix the accrual, do NOT hand-set the weight)\r\n");
                fails++;
            }
        }

        /* ---- [salience-retains] -- danger retained better at EQUAL budget -- */
        {
            INT ok = (dgain >= 5.0f)
                  && (dgain >= sloss)
                  && (macro_sal >= macro_uni - 3.0f)
                  && (acc_d_uni <= 90.0f);
            if (ok) {
                lp("[salience-retains] PASS (dgain "); lpf1(dgain);
                lp(" >= +5 AND dgain >= sloss "); lpf1(sloss);
                lp(" (net survival-favoring) AND macro "); lpf1(macro_sal);
                lp(" >= macro_uni-3 "); lpf1(macro_uni - 3.0f);
                lp(" AND danger headroom acc_uni "); lpf1(acc_d_uni);
                lp(" <= 90)\r\n");
            } else {
                lp("[salience-retains] FAIL (dgain="); lpf1(dgain);
                lp(" sloss="); lpf1(sloss);
                lp(" macro_sal="); lpf1(macro_sal); lp(" macro_uni="); lpf1(macro_uni);
                lp(" acc_d_uni="); lpf1(acc_d_uni); lp(")\r\n");
                fails++;
            }
        }

        /* ---- [salience-noregress] -- uniform DMN path unchanged ----------- */
        /* With guard_class_exp all-zero, the mirrored formula yields every
         * classw[c]==1, and the salience capture is BYTE-IDENTICAL to the
         * LM-1 uniform capture (same engrams in the same slots) -- the safety
         * hinge that keeps every [dmn-*] green when no danger has been met. */
        {
            UW zexp[LM_NCLASS];
            for (UB c = 0; c < LM_NCLASS; c++) zexp[c] = 0;
            UW zclassw[LM_NCLASS];
            lm_classw_from_exp(zexp, zclassw);     /* zero experience -> all 1 */
            INT all_one = 1;
            for (UB c = 0; c < LM_NCLASS; c++) if (zclassw[c] != 1) all_one = 0;

            /* capture uniform, snapshot; capture under zero-derived classw,
             * compare every engram byte. (capture reads only the dataset.) */
            static LM_ENGRAM lm_snap[LM_T][B_RING];
            static UW        lm_snap_n[LM_T];
            for (UB c = 0; c < LM_NCLASS; c++) lm_classw[c] = 1;
            lm_ring_clear();
            for (UB t = 0; t < LM_T; t++) lm_ring_capture(t);
            for (UB t = 0; t < LM_T; t++) {
                lm_snap_n[t] = lm_ring_n[t];
                for (UW j = 0; j < B_RING; j++) lm_snap[t][j] = lm_ring[t][j];
            }
            for (UB c = 0; c < LM_NCLASS; c++) lm_classw[c] = zclassw[c];
            lm_ring_clear();
            for (UB t = 0; t < LM_T; t++) lm_ring_capture(t);

            INT identical = 1;
            for (UB t = 0; t < LM_T && identical; t++) {
                if (lm_ring_n[t] != lm_snap_n[t]) { identical = 0; break; }
                const UB *a = (const UB *)lm_ring[t];
                const UB *b = (const UB *)lm_snap[t];
                for (UW k = 0; k < B_RING * sizeof(LM_ENGRAM); k++)
                    if (a[k] != b[k]) { identical = 0; break; }
            }
            for (UB c = 0; c < LM_NCLASS; c++) lm_classw[c] = 1;   /* restore default */

            if (all_one && identical) {
                lp("[salience-noregress] PASS (no earned experience -> classw all 1 -> capture byte-identical to LM-1 uniform)\r\n");
            } else {
                lp("[salience-noregress] FAIL (all_one="); lpd((UW)all_one);
                lp(" identical="); lpd((UW)identical); lp(")\r\n");
                fails++;
            }
        }
        #undef LM_KFIRE
    }

    dtr_ga_busy = 0;

    if (fails == 0) lp("[dmn-test] ALL PASS\r\n");
    else { lp("[dmn-test] FAILURES="); lpd((UW)fails); lp("\r\n"); }
}

/* ------------------------------------------------------------------ */
/* live DMN idle hook (II.7): EXTEND dmn_idle_work, do not fork.        */
/* One bounded replay-consolidation round when engrams are pending.     */
/* ------------------------------------------------------------------ */

#define LM_IDLE_STEPS  4        /* bounded; the slow band, not a tick    */

INT lm_consolidate_idle_round(void)
{
    if (!lm_engrams_pending()) return 0;

    /* build a replay minibatch from the whole durable ring and run a few
     * SGD steps through the SAME dtr_train_batch -- distilling the stored
     * engrams into the slow weights (sleep). Block dtr_infer meanwhile. */
    UW n = 0;
    for (UB t = 0; t < LM_T; t++)
        for (UW j = 0; j < lm_ring_n[t] && n < LM_CB_MAX; j++) {
            for (INT k = 0; k < DTR_SEQ_LEN; k++) lm_cb_x[n][k] = lm_ring[t][j].input[k];
            lm_cb_y[n] = lm_ring[t][j].label;
            n++;
        }
    if (n == 0) return 0;

    dtr_ga_busy = 1;
    for (UW s = 1; s <= LM_IDLE_STEPS; s++)
        (void)dtr_train_batch(lm_cb_x, lm_cb_y, n, lm_lr(s, LM_IDLE_STEPS));
    dtr_ga_busy = 0;
    return 1;
}
