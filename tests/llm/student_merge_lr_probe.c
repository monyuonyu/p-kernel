/*
 * student_merge_lr_probe.c — research topic 2, stage 2 PROBE (a measurement,
 * not a cert; brain-merge.md §3):
 * does a decaying learning-rate schedule make the two babies of the
 * [ss3-blob-merge] shape linearly connected (Frankle et al. train with a
 * schedule; stage 1 used a constant Adam 0.02 and the barrier did not vanish)?
 *
 * Same corpus, windows, seed, visit orders and round count as
 * tests/llm/student_merge_test.c. argv[1] = schedule:
 *   const   0.02 throughout (must reproduce stage 1: W0 barrier 0.375)
 *   linear  0.02 -> 0 linearly over the whole run
 *   cosine  0.02 * 0.5 * (1 + cos(pi * t / T))
 *   step    0.02, x0.1 at 50%, x0.1 again at 75% (Frankle's shape)
 * The schedule is over the WHOLE 40 rounds, so a pair that shares k rounds
 * branches at the same point of the schedule a W0 pair would be at.
 *
 * Build and run (one schedule per process; 2026-09-27 on the dev box, four at
 * once next to other jobs: about 10 min each):
 *   cc -std=c11 -O1 -ffp-contract=off tests/llm/student_merge_lr_probe.c \
 *      arch/common/llm/student.c -lm -o probe && ./probe cosine
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../../arch/common/llm/student.h"

#define SEQLEN 24
#define ROUNDS 40
#define LR0    0.02f

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

static int NW, TW, VW, SW;
static const uint8_t *TESTW[64];
static float *g_logits;
static const char *g_sched;

static float lr_at(long step, long total)
{
    double t = (double)step / (double)total;
    if (!strcmp(g_sched, "linear")) return (float)(LR0 * (1.0 - t));
    if (!strcmp(g_sched, "cosine")) return (float)(LR0 * 0.5 * (1.0 + cos(3.14159265358979 * t)));
    if (!strcmp(g_sched, "step"))   return t < 0.5 ? LR0 : (t < 0.75 ? LR0 * 0.1f : LR0 * 0.01f);
    return LR0;
}

/* rounds [r0, r1) of the ROUNDS-round schedule */
static void train_span(st_model *m, int shift, int r0, int r1)
{
    long total = (long)ROUNDS * TW;
    for (int r = r0; r < r1; r++)
        for (int j = 0; j < TW; j++) {
            int w = (j * shift + 3) % TW;
            const uint8_t *b = CORPUS + w * SEQLEN;
            st_zero_grad(m); st_forward(m, b, SEQLEN, g_logits);
            st_backward(m, b, SEQLEN);
            st_adam_step(m, lr_at((long)r * TW + j, total));
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

int main(int argc, char **argv)
{
    g_sched = argc > 1 ? argv[1] : "const";
    NW = ((int)sizeof(CORPUS) - 1) / SEQLEN;
    SW = NW / 6; VW = NW / 6; TW = NW - SW - VW;
    for (int i = 0; i < SW; i++) TESTW[i] = CORPUS + (TW + VW + i) * SEQLEN;
    g_logits = (float *)malloc((size_t)SEQLEN * ST_VOCAB * sizeof(float));
    st_model S; st_init_tier(&S, 1, ST_TIER_S);

    static const int KS[5] = { 0, 4, 10, 20, 30 };
    for (int q = 0; q < 5; q++) {
        st_model K, A, B;
        st_init_tier(&K, 0xC0FFEE, ST_TIER_S);
        train_span(&K, 1, 0, KS[q]);
        clone(&A, &K); clone(&B, &K);
        train_span(&A, 1, KS[q], ROUNDS);
        train_span(&B, 7, KS[q], ROUNDS);
        float curve[ST_MERGE_NALPHA];
        float bar = st_merge_barrier(&A, &B, &S, TESTW, SW, SEQLEN, curve);
        float lA = loss_on(&A, TESTW, SW), lB = loss_on(&B, TESTW, SW);
        float worse = lA > lB ? lA : lB;
        printf("[lr-probe] sched=%s shared=%d A=%.4f B=%.4f path:", g_sched, KS[q], (double)lA, (double)lB);
        for (int k = 0; k < ST_MERGE_NALPHA; k++) printf(" %.4f", (double)curve[k]);
        /* how far the two moved from the branch point, and from each other:
         * a barrier that vanishes because nothing moved is not connectivity */
        double dA = 0, dB = 0, dAB = 0, nK = 0;
        for (int i = 0; i < K.n_params; i++) {
            double a = A.w[i] - K.w[i], b = B.w[i] - K.w[i], c = A.w[i] - B.w[i];
            dA += a * a; dB += b * b; dAB += c * c; nK += (double)K.w[i] * K.w[i];
        }
        printf(" barrier=%.4f plain<=worse:%s |A-K|=%.3f |B-K|=%.3f |A-B|=%.3f |K|=%.3f\n", (double)bar,
               curve[ST_MERGE_NALPHA / 2] <= worse + 1e-4f ? "yes" : "no",
               sqrt(dA), sqrt(dB), sqrt(dAB), sqrt(nK));
        fflush(stdout);
        st_free(&K); st_free(&A); st_free(&B);
    }
    return 0;
}
