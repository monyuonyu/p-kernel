/*
 *  student_merge_test.c — host cert for the smarter merge, stage 1
 *  (docs/architecture/30-module/research/brain-merge.md §2/§3).
 *
 *  Two S-tier babies, same seed, same corpus, different visit order (the
 *  [ss3-blob-merge] shape, on a longer corpus so validation and test windows
 *  can be disjoint: train 0..T, VAL next, TEST last).
 *
 *    (A) [merge-barrier-w0]  pair branched at W0: linear-path barrier on TEST
 *                            > BAR_MIN (reproduces the ss3 cause, [1] §3).
 *    (B) [merge-barrier-wk]  pair branched after sharing the first WK_ROUNDS
 *                            rounds: barrier smaller than (A), and the plain
 *                            mean is <= the worse parent on TEST.
 *    (C) [merge-guarded]     st_merge_guarded on the W0 pair, choosing on VAL:
 *                            child <= worse parent on TEST (disjoint windows).
 *    (D) [merge-determ]      guarded merge twice -> byte-identical.
 *
 *  Negative control: -DST_MERGE_PLAIN_ONLY (the guard may only pick the plain
 *  alpha=0.5 mean) must go RED on (C). run_st_merge.sh checks both.
 *
 *  Build (wave-49): -O1 -ffp-contract=off.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../arch/common/llm/student.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

#define BAR_MIN   0.05f   /* nats: "the W0 pair is not linearly connected" */
#define SEQLEN    24
#define ROUNDS    40
#define WK_ROUNDS 4       /* 10% of training shared before branching */
#define LR        0.02f

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

static int NW, TW, VW, SW;          /* total, train, val, test windows   */
static const uint8_t *VALW[64], *TESTW[64];
static float *g_logits;

static void train_rounds(st_model *m, int shift, int rounds)
{
    for (int r = 0; r < rounds; r++)
        for (int j = 0; j < TW; j++) {
            int w = (j * shift + 3) % TW;
            const uint8_t *b = CORPUS + w * SEQLEN;
            st_zero_grad(m); st_forward(m, b, SEQLEN, g_logits);
            st_backward(m, b, SEQLEN); st_adam_step(m, LR);
        }
}

static float loss_on(st_model *m, const uint8_t *const *w, int n)
{
    double t = 0.0;
    for (int i = 0; i < n; i++) { int np = 0; t += st_eval_loss(m, w[i], SEQLEN, &np); }
    return n ? (float)(t / n) : 0.0f;
}

static void clone(st_model *dst, const st_model *src)
{
    st_init_tier(dst, 1, src->tier);
    memcpy(dst->w, src->w, (size_t)src->n_params * sizeof(float));
    memcpy(dst->mu, src->mu, (size_t)src->n_params * sizeof(float));
    memcpy(dst->vu, src->vu, (size_t)src->n_params * sizeof(float));
    dst->adam_t = src->adam_t;
}

int main(void)
{
    printf("=== [merge] smarter merge stage 1 cert ===\n");
#ifdef ST_MERGE_PLAIN_ONLY
    printf("(NEGATIVE CONTROL build: -DST_MERGE_PLAIN_ONLY — expect RED on C)\n");
#endif
    NW = ((int)sizeof(CORPUS) - 1) / SEQLEN;
    SW = NW / 6; VW = NW / 6; TW = NW - SW - VW;
    for (int i = 0; i < VW; i++) VALW[i]  = CORPUS + (TW + i) * SEQLEN;
    for (int i = 0; i < SW; i++) TESTW[i] = CORPUS + (TW + VW + i) * SEQLEN;
    printf("  corpus=%dB windows: train=%d val=%d test=%d\n",
           (int)sizeof(CORPUS) - 1, TW, VW, SW);
    g_logits = (float *)malloc((size_t)SEQLEN * ST_VOCAB * sizeof(float));

    /* ---- W0 pair (the ss3 shape) ---- */
    st_model I, A, B, S;
    st_init_tier(&I, 0xC0FFEE, ST_TIER_S);
    clone(&A, &I); clone(&B, &I);
    train_rounds(&A, 1, ROUNDS);
    train_rounds(&B, 7, ROUNDS);
    st_init_tier(&S, 1, ST_TIER_S);

    float curve[ST_MERGE_NALPHA];
    float bar0 = st_merge_barrier(&A, &B, &S, TESTW, SW, SEQLEN, curve);
    float lA = loss_on(&A, TESTW, SW), lB = loss_on(&B, TESTW, SW);
    float worse = lA > lB ? lA : lB;
    printf("  [W0] test A=%.4f B=%.4f  path:", (double)lA, (double)lB);
    for (int k = 0; k < ST_MERGE_NALPHA; k++) printf(" %.4f", (double)curve[k]);
    printf("  barrier=%.4f\n", (double)bar0);

    printf("[A]\n");
    CHECK(bar0 > BAR_MIN, "[merge-barrier-w0] W0-branched pair: barrier > BAR_MIN");

    /* ---- Wk pair: share WK_ROUNDS first, then branch ---- */
    st_model K, A2, B2;
    st_init_tier(&K, 0xC0FFEE, ST_TIER_S);
    train_rounds(&K, 1, WK_ROUNDS);
    clone(&A2, &K); clone(&B2, &K);
    train_rounds(&A2, 1, ROUNDS - WK_ROUNDS);
    train_rounds(&B2, 7, ROUNDS - WK_ROUNDS);
    float curvek[ST_MERGE_NALPHA];
    float bark = st_merge_barrier(&A2, &B2, &S, TESTW, SW, SEQLEN, curvek);
    float lA2 = loss_on(&A2, TESTW, SW), lB2 = loss_on(&B2, TESTW, SW);
    float worse2 = lA2 > lB2 ? lA2 : lB2;
    printf("  [Wk] test A=%.4f B=%.4f  path:", (double)lA2, (double)lB2);
    for (int k = 0; k < ST_MERGE_NALPHA; k++) printf(" %.4f", (double)curvek[k]);
    printf("  barrier=%.4f\n", (double)bark);
    printf("[B]\n");
    CHECK(bark < bar0, "[merge-barrier-wk] Wk-branched barrier < W0-branched barrier");
    CHECK(curvek[ST_MERGE_NALPHA / 2] <= worse2 + 1e-4f,
          "[merge-barrier-wk] plain mean of the Wk pair <= worse parent (test)");

    /* ---- guarded merge on the W0 pair, choose on VAL, judge on TEST ---- */
    st_model C, C2;
    clone(&C, &A); clone(&C2, &A);
    int pick  = st_merge_guarded(&C,  &B, &I, &S, VALW, VW, SEQLEN);
    int pick2 = st_merge_guarded(&C2, &B, &I, &S, VALW, VW, SEQLEN);
    float lC = loss_on(&C, TESTW, SW);
    printf("  [guarded] pick=%d (0..%d = alpha 0..1, %d = TIES) child test=%.4f "
           "worse parent=%.4f plain=%.4f\n", pick, ST_MERGE_NALPHA - 1,
           ST_MERGE_NALPHA, (double)lC, (double)worse,
           (double)curve[ST_MERGE_NALPHA / 2]);
    printf("[C]\n");
    CHECK(pick >= 0, "st_merge_guarded returned a candidate");
    CHECK(lC <= worse + 1e-4f, "[merge-guarded] child <= worse parent on TEST (chosen on VAL)");
    printf("[D]\n");
    CHECK(pick == pick2 && memcmp(C.w, C2.w, (size_t)C.n_params * 4) == 0,
          "[merge-determ] guarded merge twice -> byte-identical");
    CHECK(C.adam_t == 0, "Adam state reset after merge");

    printf("\n[merge] %d PASS / %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
