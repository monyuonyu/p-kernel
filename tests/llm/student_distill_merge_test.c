/*
 *  student_distill_merge_test.c — host cert for the smarter merge, stage 3:
 *  merge by distillation (docs/architecture/30-module/research/
 *  brain-merge-distill.md §3; the gates below were fixed there BEFORE any
 *  distillation code was written).
 *
 *  Same babies and windows as student_merge_test.c (corpus, SEQLEN 24,
 *  ROUNDS 40, train / VAL / TEST windows). Parents' LR 3e-3 since stage 5
 *  (0.02 before; -DST_DISTILL_PARENT_LR_OLD), precondition
 *  [d5c-parents-ok] in every arm (brain-merge-stable.md §2-3).
 *
 *    S1  same data, different order (seed 0xC0FFEE, order 1 vs 7)
 *    S2  disjoint data: A trains on the first half of the train windows,
 *        B on the second half (ROUNDS*2 passes each)
 *
 *  Two arms for the distillation text `xfer`:
 *    T-real  the train windows (reference: the raw data is handed over)
 *    T-gen   text the two parents generate themselves (no raw data; the
 *            intended use)
 *
 *  Two starts for the child (each gate line ends in start=pick|init):
 *    pick  stage 3: the guarded pick, D_STEPS at D_LR
 *    init  stage 3b (brain-merge-distill.md §4, fixed before results): the
 *          common ancestor I, one parent's budget (ROUNDS x TW steps) at the
 *          parents' LR
 *
 *  Gates, per arm and start (each prints PASS/FAIL; rc is over ALL of
 *  them, T-gen included — a red T-gen is reported red, not re-labelled):
 *    [d5c-s1-gain]  S1 TEST: child <= better parent - GAIN
 *    [d5c-s2-both]  S2 both halves: child <= better parent - GAIN, and on
 *                   each half child <= midpoint of the two parents
 *    [d5c-floor]    child <= worse parent (S1 TEST, S2 both halves)
 *    [d5c-determ]   the merge twice -> byte-identical
 *    [d5c-b-plain]  start=init, T-real only: child <= plain hard-label
 *                   training from I with the same steps and LR (S1 TEST,
 *                   S2 both halves); printed, not gated, on T-gen
 *
 *  Stage 4 (brain-merge-select.md §3, fixed before code): start=sel runs
 *  both starts with their stage-3/3b steps and LR and keeps the lower VAL
 *  (st_merge_distill_sel). T-real only; the same gates plus
 *    [d5c-sel-val]  the child is the start with the lower VAL loss, and is
 *                   byte-identical to that single-start child (TEST of both
 *                   printed; a VAL/TEST disagreement is recorded, not red)
 *  Env: D5C_SEED (the ancestor's seed, default 0xC0FFEE), D5C_ONLY_SEL=1
 *  (only the sel arm). run_st_distill_sel.sh runs seeds 0xC0FFEE 2 3 and
 *  the NCs -DST_DISTILL_SEL_INVERT / _SEL_PICK / _SEL_INIT.
 *
 *  Stage 7 (brain-merge-swa.md §2-3, fixed before code), -DD5C_SWA: the
 *  init arm returns the mean of the weights at every ST_DISTILL_EVAL-th
 *  step after steps/2 (st_merge_distill_sel_swa, ST_DISTILL_START_INIT_SWA)
 *  instead of the VAL-best; start=sel and the single-start init child in
 *  [d5c-sel-val] both use it. Adds
 *    [d5c-swa-points]     the init arm averaged steps/10 - (steps/2)/10
 *                         points (gated: the mechanism, not the research)
 *    [d5c-teacher-reach]  S1: the teacher's TEST <= better parent - GAIN
 *                         (recorded, not gated). run_st_distill_swa.sh.
 *
 *  Negative controls (run_st_distill.sh judges the target line
 *  "[d5c-s2-both] ... start=init", not the exit code):
 *    -DST_DISTILL_ONE_TEACHER  the teacher is `into` alone
 *    -DST_DISTILL_RAND_XFER    xfer is pseudo-random bytes
 *
 *  Build (wave-49): -O1 -ffp-contract=off.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "../../arch/common/llm/student.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

#define SEQLEN  24
#define ROUNDS  40
/* stage 5 (brain-merge-stable.md §2): the parents learn at the resident
 * brain's rate, ST_DMN_LR in student_shell.c; at 0.02 (stages 3-4) 11 of 32
 * probe parents diverged. -DST_DISTILL_PARENT_LR_OLD = the stage 3/4 rate
 * (the negative control, and how to reproduce brain-merge-select-log.md). */
#ifdef ST_DISTILL_PARENT_LR_OLD
#define LR      0.02f
#else
#define LR      3e-3f
#endif
#define GAIN    0.03f     /* nats, brain-merge-distill.md §3 */
#define D_STEPS 400       /* distillation steps (fixed, d5c3-plan.md) */
#define D_LR    1e-3f     /* Adam, FedDF §4.1 */
/* stage 3b: the init arm's LR, steps ROUNDS * TW. Stage 6
 * (brain-merge-blr.md §2): its own constant, -DD5C_B_LR=<v>; the default
 * stays the parents' LR (= stage 5's output) */
#ifdef D5C_B_LR
#define B_LR    (D5C_B_LR)
#else
#define B_LR    LR
#endif
#define NGEN_MAX 128
#define START_SEL 2       /* test-local: st_merge_distill_sel */

static const uint8_t CORPUS[] =
    "the cat sat on the mat. the dog ran in the sun. she said the sea is blue "
    "and the sky is blue too. a bird sang on the old oak tree at dawn. the cat "
    "and the dog sat by the sea and saw the sun set red. the old man ran to the "
    "sea to see the red sun set on the blue sea at the end of the day. the bird "
    "and the cat saw the old man by the tree. the sun is red and the sea is blue. "
    "a dog and a cat ran to the old tree by the sea. the man said the sky is red "
    "at dawn and blue at the end of the day. the bird sat on the mat by the dog. "
    "she ran in the sun to the sea and saw a red bird on the old oak tree. the "
    "cat said the dog is old and the man is at the sea. the sun set on the tree.";

static uint64_t g_seed = 0xC0FFEE;
static st_distill_sel_stats g_ss;
static int NW, TW, VW, SW, NA, NB;
static const uint8_t *ALLW[64], *VALW[64], *TESTW[64], *TRA[64], *TRB[64];
static float *g_logits;
static uint8_t GENBUF[NGEN_MAX][SEQLEN];
static const uint8_t *GENW[NGEN_MAX];

static float loss_on(st_model *m, const uint8_t *const *w, int n)
{
    double t = 0.0;
    for (int i = 0; i < n; i++) { int np = 0; t += st_eval_loss(m, w[i], SEQLEN, &np); }
    return n ? (float)(t / n) : 0.0f;
}

static char g_msg[256];
static const char *G(const char *gate, const char *sn)
{
    snprintf(g_msg, sizeof g_msg, "%s start=%s", gate, sn);
    return g_msg;
}

/* own-data loss of a parent, per round (stage 5, [d5c-parents-ok]) */
typedef struct { float min, last; } own_curve;

static void train_list(st_model *m, const uint8_t *const *w, int n, int shift,
                       int rounds, own_curve *oc)
{
    if (oc) { oc->min = 1e30f; oc->last = 0.0f; }
    for (int r = 0; r < rounds; r++) {
        for (int j = 0; j < n; j++) {
            const uint8_t *b = w[(j * shift + 3) % n];
            st_zero_grad(m); st_forward(m, b, SEQLEN, g_logits);
            st_backward(m, b, SEQLEN); st_adam_step(m, LR);
        }
        if (oc) {
            oc->last = loss_on(m, w, n);
            if (oc->last < oc->min) oc->min = oc->last;
        }
    }
}

/* brain-merge-stable.md §2: a parent that ends more than 0.3 above its own
 * per-round minimum, or above 1.0, diverged or did not learn - the merge is
 * then not tested. Prints both parents, one gate line per scenario. */
static void parents_ok(const char *sc, const char *sn, const own_curve *a,
                       const own_curve *b)
{
    printf("  [parents %s] own-data loss A min=%.4f last=%.4f | B min=%.4f last=%.4f\n",
           sc, (double)a->min, (double)a->last, (double)b->min, (double)b->last);
    int ok = a->last <= a->min + 0.3f && a->last <= 1.0f &&
             b->last <= b->min + 0.3f && b->last <= 1.0f;
    char m[128];
    snprintf(m, sizeof m, "[d5c-parents-ok] %s: both parents end <= own min + 0.3 and <= 1.0", sc);
    CHECK(ok, G(m, sn));
}

static void clone(st_model *dst, const st_model *src)
{
    st_init_tier(dst, 1, src->tier);
    memcpy(dst->w, src->w, (size_t)src->n_params * sizeof(float));
}

/* T-gen: each parent writes half of the text. Prompt = one byte from a fixed
 * list (not from the corpus windows), fixed seeds, temp 1, all of the vocab. */
static int make_gen(st_model *A, st_model *B, int count)
{
    static const uint8_t P[] = "tasdbhmo";
    if (count > NGEN_MAX) count = NGEN_MAX;
    for (int i = 0; i < count; i++) {
        st_model *who = (i & 1) ? B : A;
        uint8_t pr = P[(i / 2) % (int)(sizeof(P) - 1)];
        GENBUF[i][0] = pr;
        int got = st_generate(who, &pr, 1, &GENBUF[i][1], SEQLEN - 1, 1.0f, 0,
                              0x5EED0000ULL + (uint64_t)i);
        for (int k = got < 0 ? 0 : got; k < SEQLEN - 1; k++) GENBUF[i][1 + k] = ' ';
        GENW[i] = GENBUF[i];
    }
    return count;
}

#ifdef ST_DISTILL_RAND_XFER
static void make_rand(int count)
{
    uint64_t s = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < count; i++) {
        for (int k = 0; k < SEQLEN; k++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            GENBUF[i][k] = (uint8_t)(s >> 24);
        }
        GENW[i] = GENBUF[i];
    }
}
#endif

/* merge peer into a copy of `a`; the child goes to *child */
static int merge(st_model *child, st_model *a, st_model *b, const st_model *I,
                 st_model *S, const uint8_t *const *x, int nx, int start,
                 st_distill_stats *st)
{
    clone(child, a);
    if (start == START_SEL) {
        memset(&g_ss, 0, sizeof g_ss);
#ifdef D5C_SWA
        int r = st_merge_distill_sel_swa(child, b, I, S, x, nx, VALW, VW, SEQLEN,
                                         D_STEPS, D_LR, ROUNDS * TW, B_LR, &g_ss);
#else
        int r = st_merge_distill_sel(child, b, I, S, x, nx, VALW, VW, SEQLEN,
                                     D_STEPS, D_LR, ROUNDS * TW, B_LR, &g_ss);
#endif
        *st = g_ss.chose_init ? g_ss.init : g_ss.pick;
        return r;
    }
    if (start == ST_DISTILL_START_INIT)
        return st_merge_distill(child, b, I, S, x, nx, VALW, VW, SEQLEN,
                                ROUNDS * TW, B_LR,
#ifdef D5C_SWA
                                ST_DISTILL_START_INIT_SWA,
#else
                                start,
#endif
                                st);
    return st_merge_distill(child, b, I, S, x, nx, VALW, VW, SEQLEN, D_STEPS,
                            D_LR, start, st);
}

static void report(const char *arm, const char *sc, int rc,
                   const st_distill_stats *st)
{
    printf("  [%s %s] rc=%d init_pick=%d best_step=%d val %.4f -> %.4f\n",
           arm, sc, rc, st->init_pick, st->best_step,
           (double)st->val_before, (double)st->val_after);
}

#ifdef D5C_SWA
/* stage 7: the teacher's own loss on windows w = mean next-byte NLL under
 * softmax(0.5 (logits A + logits B)), as st_teacher_row builds it */
static float teacher_loss_on(st_model *a, st_model *b, const uint8_t *const *w, int n)
{
    size_t row = (size_t)SEQLEN * ST_VOCAB;
    float *la = (float *)malloc(row * sizeof(float));
    float *lb = (float *)malloc(row * sizeof(float));
    double tot = 0.0;
    for (int i = 0; i < n; i++) {
        st_forward(a, w[i], SEQLEN, la);
        st_forward(b, w[i], SEQLEN, lb);
        double s1 = 0.0;
        for (int t = 0; t < SEQLEN - 1; t++) {
            const float *za = la + (size_t)t * ST_VOCAB, *zb = lb + (size_t)t * ST_VOCAB;
            double mx = 0.5 * ((double)za[0] + zb[0]), s = 0.0;
            for (int o = 1; o < ST_VOCAB; o++) {
                double z = 0.5 * ((double)za[o] + zb[o]);
                if (z > mx) mx = z;
            }
            for (int o = 0; o < ST_VOCAB; o++) s += exp(0.5 * ((double)za[o] + zb[o]) - mx);
            int y = w[i][t + 1];
            s1 -= 0.5 * ((double)za[y] + zb[y]) - mx - log(s);
        }
        tot += s1 / (SEQLEN - 1);
    }
    free(la); free(lb);
    return n ? (float)(tot / n) : 0.0f;
}
#endif

/* stage 4: the sel child against both single-start children */
static void sel_check(const char *sc, st_model *C, st_model *A, st_model *B,
                      const st_model *I, st_model *S, const uint8_t *const *x,
                      int nx, const uint8_t *const *tw, int tn, int rc)
{
    st_distill_sel_stats ss = g_ss;
    st_model Cp, Ci; st_distill_stats sp, si;
    int rp = merge(&Cp, A, B, I, S, x, nx, ST_DISTILL_START_PICK, &sp);
    int ri = merge(&Ci, A, B, I, S, x, nx, ST_DISTILL_START_INIT, &si);
    float tp = loss_on(&Cp, tw, tn), ti = loss_on(&Ci, tw, tn);
    int want = ss.init.val_after < ss.pick.val_after;
    const st_model *W = want ? &Ci : &Cp;
    int same = rc >= 0 && rp >= 0 && ri >= 0 &&
               memcmp(C->w, W->w, (size_t)C->n_params * sizeof(float)) == 0;
    int test_agrees = (ti < tp) == want;
    /* stage 6: both arms' walks (brain-merge-blr.md §2). guard = VAL of the
     * guarded pick, the fall-back of both arms; ancestor = VAL of I, where
     * the init arm starts */
    printf("  [walk %s pick] best_step=%d val guard %.4f -> %.4f fell_back=%d\n",
           sc, ss.pick.best_step, (double)ss.pick.val_before,
           (double)ss.pick.val_after, ss.pick.best_step == 0);
#ifdef D5C_SWA
    int steps = ROUNDS * TW, want_pts = steps / ST_DISTILL_EVAL - (steps / 2) / ST_DISTILL_EVAL;
    printf("  [walk %s init] best_step=%d val guard %.4f ancestor %.4f -> %.4f fell_back=%d swa_points=%d\n",
           sc, ss.init.best_step, (double)ss.init.val_before,
           (double)loss_on((st_model *)I, VALW, VW), (double)ss.init.val_after,
           ss.init.best_step == 0, ss.init.n_avg);
    snprintf(g_msg, sizeof g_msg,
             "[d5c-swa-points] %s: the init arm averaged %d points (steps %d)", sc, want_pts, steps);
    CHECK(rc >= 0 && ss.init.n_avg == want_pts, g_msg);
#else
    printf("  [walk %s init] best_step=%d val guard %.4f ancestor %.4f -> %.4f fell_back=%d\n",
           sc, ss.init.best_step, (double)ss.init.val_before,
           (double)loss_on((st_model *)I, VALW, VW), (double)ss.init.val_after,
           ss.init.best_step == 0);
#endif
    printf("  [sel %s] VAL pick=%.4f init=%.4f -> chose %s | TEST pick=%.4f init=%.4f -> %s\n",
           sc, (double)ss.pick.val_after, (double)ss.init.val_after,
           ss.chose_init ? "init" : "pick", (double)tp, (double)ti,
           test_agrees ? "TEST agrees" : "TEST DISAGREES (recorded, not red)");
    snprintf(g_msg, sizeof g_msg,
             "[d5c-sel-val] %s: child = the lower-VAL start, byte-identical start=sel", sc);
    CHECK(rc >= 0 && ss.chose_init == want && same, g_msg);
    st_free(&Cp); st_free(&Ci);
}

static void run_arm(const char *arm, int gen, int start)
{
    const char *sn = start == START_SEL ? "sel" :
                     start == ST_DISTILL_START_INIT ? "init" : "pick";
    printf("=== arm %s start=%s ===\n", arm, sn);
    st_model I, A, B, S, C, C2, P;
    st_distill_stats st, st2;
    const uint8_t *const *x; int nx;

    st_init_tier(&I, g_seed, ST_TIER_S);
    st_init_tier(&S, 1, ST_TIER_S);
    /* plain reference (stage 3b): hard labels from I, same steps and LR */
    clone(&P, &I);
    train_list(&P, ALLW, TW, 1, ROUNDS, NULL);
    own_curve oa, ob;

    /* ---- S1 ---- */
    clone(&A, &I); clone(&B, &I);
    train_list(&A, ALLW, TW, 1, ROUNDS, &oa);
    train_list(&B, ALLW, TW, 7, ROUNDS, &ob);
    parents_ok("S1", sn, &oa, &ob);
    if (gen) { nx = make_gen(&A, &B, 2 * TW); x = GENW; }
    else     { nx = TW; x = ALLW; }
#ifdef ST_DISTILL_RAND_XFER
    nx = 2 * TW; make_rand(nx); x = GENW;
#endif
    int rc  = merge(&C,  &A, &B, &I, &S, x, nx, start, &st);
    int rc2 = merge(&C2, &A, &B, &I, &S, x, nx, start, &st2);
    report(arm, "S1", rc, &st);
    if (start == START_SEL)
        sel_check("S1", &C, &A, &B, &I, &S, x, nx, TESTW, SW, rc);
    float lA = loss_on(&A, TESTW, SW), lB = loss_on(&B, TESTW, SW);
    float lC = loss_on(&C, TESTW, SW), lP = loss_on(&P, TESTW, SW);
    float better = lA < lB ? lA : lB, worse = lA > lB ? lA : lB;
    printf("  [%s S1 %s] TEST A=%.4f B=%.4f child=%.4f plain=%.4f (gain vs better %.4f)\n",
           arm, sn, (double)lA, (double)lB, (double)lC, (double)lP,
           (double)(better - lC));
#ifdef D5C_SWA
    if (start == START_SEL) {
        float lT = teacher_loss_on(&A, &B, TESTW, SW);
        printf("  [d5c-teacher-reach] S1 start=sel: teacher TEST %.4f vs better parent - %.2f = %.4f -> %s (recorded, not gated)\n",
               (double)lT, (double)GAIN, (double)(better - GAIN),
               lT <= better - GAIN ? "reach" : "MISS");
    }
#endif
    CHECK(rc >= 0, G("st_merge_distill returned ok (S1)", sn));
    CHECK(rc >= 0 && lC <= better - GAIN, G("[d5c-s1-gain] S1 TEST: child <= better parent - 0.03", sn));
    CHECK(rc >= 0 && lC <= worse + 1e-4f, G("[d5c-floor] S1 TEST: child <= worse parent", sn));
    CHECK(rc >= 0 && rc == rc2 && st.best_step == st2.best_step &&
          memcmp(C.w, C2.w, (size_t)C.n_params * sizeof(float)) == 0,
          G("[d5c-determ] merge twice -> byte-identical", sn));
    CHECK(rc >= 0 && C.adam_t == 0, G("Adam state reset after merge", sn));
    /* In S1, P is trained exactly as A (I, order 1, ROUNDS): lP == lA, so this
     * gate is "child <= A", implied by [d5c-s1-gain] - not an independent
     * check (audit-28; brain-merge-distill.md §4). Kept as fixed before
     * results. */
    if (start == ST_DISTILL_START_INIT && !gen)
        CHECK(rc >= 0 && lC <= lP, G("[d5c-b-plain] S1 TEST: child <= plain training from I", sn));
    st_free(&A); st_free(&B); st_free(&C); st_free(&C2);

    /* ---- S2 ---- */
    clone(&A, &I); clone(&B, &I);
    train_list(&A, TRA, NA, 1, ROUNDS * 2, &oa);
    train_list(&B, TRB, NB, 1, ROUNDS * 2, &ob);
    parents_ok("S2", sn, &oa, &ob);
    if (gen) { nx = make_gen(&A, &B, 2 * TW); x = GENW; }
    else     { nx = TW; x = ALLW; }
#ifdef ST_DISTILL_RAND_XFER
    nx = 2 * TW; make_rand(nx); x = GENW;
#endif
    rc = merge(&C, &A, &B, &I, &S, x, nx, start, &st);
    report(arm, "S2", rc, &st);
    if (start == START_SEL)
        sel_check("S2", &C, &A, &B, &I, &S, x, nx, TESTW, SW, rc);
    float aA = loss_on(&A, ALLW, TW), aB = loss_on(&B, ALLW, TW), aC = loss_on(&C, ALLW, TW);
    float aP = loss_on(&P, ALLW, TW);
    float hA[2], hB[2], hC[2];
    hA[0] = loss_on(&A, TRA, NA); hB[0] = loss_on(&B, TRA, NA); hC[0] = loss_on(&C, TRA, NA);
    hA[1] = loss_on(&A, TRB, NB); hB[1] = loss_on(&B, TRB, NB); hC[1] = loss_on(&C, TRB, NB);
    float tA = loss_on(&A, TESTW, SW), tB = loss_on(&B, TESTW, SW), tC = loss_on(&C, TESTW, SW);
    printf("  [%s S2 %s] both  A=%.4f B=%.4f child=%.4f plain=%.4f\n", arm, sn,
           (double)aA, (double)aB, (double)aC, (double)aP);
    printf("  [%s S2] A-half A=%.4f B=%.4f child=%.4f mid=%.4f\n", arm,
           (double)hA[0], (double)hB[0], (double)hC[0], (double)(0.5f * (hA[0] + hB[0])));
    printf("  [%s S2] B-half A=%.4f B=%.4f child=%.4f mid=%.4f\n", arm,
           (double)hA[1], (double)hB[1], (double)hC[1], (double)(0.5f * (hA[1] + hB[1])));
    printf("  [%s S2] TEST  A=%.4f B=%.4f child=%.4f (reported, not gated)\n", arm,
           (double)tA, (double)tB, (double)tC);
    float b2 = aA < aB ? aA : aB, w2 = aA > aB ? aA : aB;
    int halves = hC[0] <= 0.5f * (hA[0] + hB[0]) && hC[1] <= 0.5f * (hA[1] + hB[1]);
    CHECK(rc >= 0, G("st_merge_distill returned ok (S2)", sn));
    CHECK(rc >= 0 && aC <= b2 - GAIN && halves,
          G("[d5c-s2-both] S2: both halves child <= better - 0.03, each half <= midpoint", sn));
    CHECK(rc >= 0 && aC <= w2 + 1e-4f, G("[d5c-floor] S2 both halves: child <= worse parent", sn));
    if (start == ST_DISTILL_START_INIT && !gen)
        CHECK(rc >= 0 && aC <= aP, G("[d5c-b-plain] S2 both halves: child <= plain training from I", sn));
    st_free(&A); st_free(&B); st_free(&C);
    st_free(&I); st_free(&S); st_free(&P);
}

/* soft-target backward: (a) a one-hot q gives st_backward's loss and
 * gradient; (b) with a spread q (another baby's softmax) the output-head
 * gradient matches central finite differences of -sum q log p (the head is
 * after the router, so a step there cannot change which experts fire). */
static double soft_loss(st_model *m, const uint8_t *x, const float *q)
{
    st_forward(m, x, SEQLEN, g_logits);
    double tot = 0.0;
    for (int t = 0; t < SEQLEN - 1; t++) {
        const float *z = g_logits + (size_t)t * ST_VOCAB;
        double mx = z[0], s = 0.0;
        for (int o = 1; o < ST_VOCAB; o++) if (z[o] > mx) mx = z[o];
        for (int o = 0; o < ST_VOCAB; o++) s += exp(z[o] - mx);
        double lse = mx + log(s);
        for (int o = 0; o < ST_VOCAB; o++)
            tot -= (double)q[(size_t)t * ST_VOCAB + o] * ((double)z[o] - lse);
    }
    return tot / (SEQLEN - 1);
}

static void soft_grad(void)
{
    printf("=== soft-target backward ===\n");
    st_model M, T;
    st_init_tier(&M, 0xABCD, ST_TIER_S);
    st_init_tier(&T, 0x1234, ST_TIER_S);
    const uint8_t *x = ALLW[0];
    size_t row = (size_t)SEQLEN * ST_VOCAB;
    float *q  = (float *)calloc(row, sizeof(float));
    float *g1 = (float *)malloc((size_t)M.n_params * sizeof(float));

    for (int t = 0; t < SEQLEN - 1; t++) q[(size_t)t * ST_VOCAB + x[t + 1]] = 1.0f;
    st_zero_grad(&M); st_forward(&M, x, SEQLEN, g_logits);
    float lh = st_backward(&M, x, SEQLEN);
    memcpy(g1, M.g, (size_t)M.n_params * sizeof(float));
    st_zero_grad(&M); st_forward(&M, x, SEQLEN, g_logits);
    float ls = st_backward_soft(&M, x, SEQLEN, q);
    float gmax = 0.0f, dmax = 0.0f;
    for (int i = 0; i < M.n_params; i++) {
        float a = g1[i] < 0 ? -g1[i] : g1[i];
        float d = g1[i] - M.g[i]; if (d < 0) d = -d;
        if (a > gmax) gmax = a;
        if (d > dmax) dmax = d;
    }
    printf("  one-hot q: loss hard=%.6f soft=%.6f  max|dg|/max|g|=%.3g\n",
           (double)lh, (double)ls, (double)(gmax > 0 ? dmax / gmax : 1.0f));
    CHECK(gmax > 0.0f && dmax <= 1e-5f * gmax && (lh - ls) < 1e-5f && (ls - lh) < 1e-5f,
          "[d5c-soft-onehot] one-hot q == st_backward (loss, every gradient)");

    /* spread q: T's next-byte distribution */
    st_forward(&T, x, SEQLEN, g_logits);
    for (int t = 0; t < SEQLEN - 1; t++) {
        const float *z = g_logits + (size_t)t * ST_VOCAB;
        double mx = z[0], s = 0.0;
        for (int o = 1; o < ST_VOCAB; o++) if (z[o] > mx) mx = z[o];
        for (int o = 0; o < ST_VOCAB; o++) s += exp(z[o] - mx);
        for (int o = 0; o < ST_VOCAB; o++)
            q[(size_t)t * ST_VOCAB + o] = (float)(exp(z[o] - mx) / s);
    }
    st_zero_grad(&M); st_forward(&M, x, SEQLEN, g_logits);
    float la = st_backward_soft(&M, x, SEQLEN, q);
    double lr = soft_loss(&M, x, q);
    float worst = 0.0f;
    int nhead = ST_VOCAB * M.d;
    for (int k = 0; k < 8; k++) {         /* the 8 largest |g| in the head */
        int bi = -1; float bg = -1.0f;
        for (int i = 0; i < nhead; i++) {
            float a = M.g[M.o_out + i]; if (a < 0) a = -a;
            if (a > bg) { bg = a; bi = i; }
        }
        float ga = M.g[M.o_out + bi];
        float w0 = M.w[M.o_out + bi], eps = 1e-2f;
        M.w[M.o_out + bi] = w0 + eps; double lp = soft_loss(&M, x, q);
        M.w[M.o_out + bi] = w0 - eps; double lm = soft_loss(&M, x, q);
        M.w[M.o_out + bi] = w0;
        float fd = (float)((lp - lm) / (2.0 * eps));
        float den = (fd < 0 ? -fd : fd) > (ga < 0 ? -ga : ga) ? (fd < 0 ? -fd : fd) : (ga < 0 ? -ga : ga);
        float re = den > 0 ? (fd - ga) / den : 0.0f; if (re < 0) re = -re;
        if (re > worst) worst = re;
        M.g[M.o_out + bi] = 0.0f;         /* next largest */
    }
    printf("  spread q: loss backward=%.6f recomputed=%.6f  FD max rel err (8 head weights)=%.3g\n",
           (double)la, lr, (double)worst);
    CHECK(worst < 1e-2f && (la - lr) < 1e-4 && (lr - la) < 1e-4,
          "[d5c-soft-fd] spread q: loss and head gradient match finite differences");
    free(q); free(g1); st_free(&M); st_free(&T);
}

int main(void)
{
    printf("=== [d5c] merge by distillation (stage 3/4) cert ===\n");
    const char *es = getenv("D5C_SEED");
    if (es && *es) g_seed = strtoull(es, NULL, 0);
    const char *eo = getenv("D5C_ONLY_SEL");
    int only_sel = eo && *eo == '1';
    printf("  seed 0x%llX%s\n", (unsigned long long)g_seed, only_sel ? " (only start=sel)" : "");
    printf("  LR %g B_LR %g\n", (double)LR, (double)B_LR);
#ifdef D5C_SWA
    printf("  stage 7: init arm = mean of the second-half weights (st_merge_distill_sel_swa)\n");
#endif
#ifdef ST_DISTILL_SWA_FIRST
    printf("(NEGATIVE CONTROL build: -DST_DISTILL_SWA_FIRST — the first half is averaged; read S1 TEST against plain)\n");
#endif
#if defined(ST_DISTILL_SEL_INVERT)
    printf("(NEGATIVE CONTROL build: -DST_DISTILL_SEL_INVERT — expect RED on s1-gain or s2-both start=sel)\n");
#elif defined(ST_DISTILL_SEL_PICK)
    printf("(NEGATIVE CONTROL build: -DST_DISTILL_SEL_PICK — expect RED on s1-gain start=sel)\n");
#elif defined(ST_DISTILL_SEL_INIT)
    printf("(NEGATIVE CONTROL build: -DST_DISTILL_SEL_INIT — expect RED on s2-both start=sel)\n");
#endif
#ifdef ST_DISTILL_PARENT_LR_OLD
    printf("(NEGATIVE CONTROL build: -DST_DISTILL_PARENT_LR_OLD — parents at 0.02, expect RED on parents-ok on some seed)\n");
#endif
#ifdef ST_DISTILL_ONE_TEACHER
    printf("(NEGATIVE CONTROL build: -DST_DISTILL_ONE_TEACHER — expect RED on s2-both start=init)\n");
#endif
#ifdef ST_DISTILL_RAND_XFER
    printf("(NEGATIVE CONTROL build: -DST_DISTILL_RAND_XFER — expect RED on s2-both start=init)\n");
#endif
    NW = ((int)sizeof(CORPUS) - 1) / SEQLEN;
    SW = NW / 6; VW = NW / 6; TW = NW - SW - VW;
    for (int i = 0; i < TW; i++) ALLW[i]  = CORPUS + i * SEQLEN;
    for (int i = 0; i < VW; i++) VALW[i]  = CORPUS + (TW + i) * SEQLEN;
    for (int i = 0; i < SW; i++) TESTW[i] = CORPUS + (TW + VW + i) * SEQLEN;
    NA = TW / 2; NB = TW - NA;
    for (int i = 0; i < NA; i++) TRA[i] = ALLW[i];
    for (int i = 0; i < NB; i++) TRB[i] = ALLW[NA + i];
    printf("  windows: train=%d (A %d / B %d) val=%d test=%d\n", TW, NA, NB, VW, SW);
    g_logits = (float *)malloc((size_t)SEQLEN * ST_VOCAB * sizeof(float));

    if (!only_sel) {
        soft_grad();
        run_arm("T-real", 0, ST_DISTILL_START_PICK);
        run_arm("T-gen", 1, ST_DISTILL_START_PICK);
        run_arm("T-real", 0, ST_DISTILL_START_INIT);
        run_arm("T-gen", 1, ST_DISTILL_START_INIT);
    }
    run_arm("T-real", 0, START_SEL);

    printf("\n[d5c] %d PASS / %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
