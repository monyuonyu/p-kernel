/* student_merge_e3_test.c - the cert of research/brain-merge-e3.md section 3:
 * does the ss3 W0 pair's plain mean still lose to the worse parent when the
 * parents are trained the way the product trains now (ST_LR, Adam eps 1e-8)?
 *
 *   student_merge_e3_test <arm> <seed>
 *
 * Arms (fixed in the design before any judgement run):
 *   prod  learning rate ST_LR (student.h), build with the default eps (1e-8)
 *   old   learning rate 0.02,  build with -DST_ADAM_EPS=0.015625f (the old ss3)
 *   e2a   learning rate 3e-3,  default eps build
 *   e2b   learning rate 3e-3,  -DST_ADAM_EPS=0.015625f build
 * The arm only picks the learning rate; the eps is whatever student.c was
 * built with, and the line prints it so the judge (merge_e3_check.py) can
 * refuse an arm run on the wrong build.
 *
 * The shape is the a140 probe's (pk-scratch/a140/ss3e3.c): the corpus and
 * the W0 pair of student_merge_test.c (one init, parent A walks the training
 * windows in order 1, parent B in order 7), 40 rounds, 20 train / 4 VAL /
 * 4 TEST windows of 24 bytes. The plain mean is st_merge_cohort(A, {B}) as
 * the product does it. Prints one "[me3] arm=..." line and "[me3] done".
 * Deterministic: the judge wants two runs of one seed byte-identical. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "student.h"

#ifndef ST_ADAM_EPS
#define ST_ADAM_EPS 1e-8f     /* must match student.c's default */
#endif
#define SEQLEN 24
#define ROUNDS 40
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
static const uint8_t *VALW[64], *TESTW[64];
static float *g_logits;

static void train_rounds(st_model *m, int shift, int rounds, float lr)
{
    for (int r = 0; r < rounds; r++)
        for (int j = 0; j < TW; j++) {
            int w = (j * shift + 3) % TW;
            const uint8_t *b = CORPUS + w * SEQLEN;
            st_zero_grad(m); st_forward(m, b, SEQLEN, g_logits);
            st_backward(m, b, SEQLEN); st_adam_step(m, lr);
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
    if (argc != 3) {
        fprintf(stderr, "usage: %s prod|old|e2a|e2b <seed>\n", argv[0]);
        return 2;
    }
    const char *arm = argv[1];
    float lr;
    if (!strcmp(arm, "prod")) lr = ST_LR;
    else if (!strcmp(arm, "old")) lr = 0.02f;
    else if (!strcmp(arm, "e2a") || !strcmp(arm, "e2b")) lr = 3e-3f;
    else { fprintf(stderr, "unknown arm %s\n", arm); return 2; }
    uint32_t seed = (uint32_t)strtoul(argv[2], 0, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    NW = ((int)sizeof(CORPUS) - 1) / SEQLEN;
    SW = NW / 6; VW = NW / 6; TW = NW - SW - VW;
    for (int i = 0; i < VW; i++) VALW[i]  = CORPUS + (TW + i) * SEQLEN;
    for (int i = 0; i < SW; i++) TESTW[i] = CORPUS + (TW + VW + i) * SEQLEN;
    g_logits = (float *)malloc((size_t)SEQLEN * ST_VOCAB * sizeof(float));
    if (!g_logits) return 1;
    st_model I, A, B, S, M;
    st_init_tier(&I, seed, ST_TIER_S);
    clone(&A, &I); clone(&B, &I);
    train_rounds(&A, 1, ROUNDS, lr);
    train_rounds(&B, 7, ROUNDS, lr);
    st_init_tier(&S, 1, ST_TIER_S);
    float ct[ST_MERGE_NALPHA], cv[ST_MERGE_NALPHA];
    float bt = st_merge_barrier(&A, &B, &S, TESTW, SW, SEQLEN, ct);
    float bv = st_merge_barrier(&A, &B, &S, VALW, VW, SEQLEN, cv);
    clone(&M, &A);
    long cap = (long)st_blob_size(&B);
    unsigned char *blob = malloc((size_t)cap);
    if (!blob) return 1;
    long len = st_save(&B, blob, (size_t)cap);
    if (len <= 0) { printf("[me3] st_save failed %ld\n", len); return 1; }
    const void *pb[1] = { blob }; size_t pl[1] = { (size_t)len };
    int acc = st_merge_cohort(&M, pb, pl, 1);
    float la = loss_on(&A, TESTW, SW), lb = loss_on(&B, TESTW, SW), lm = loss_on(&M, TESTW, SW);
    float worse = la > lb ? la : lb, better = la < lb ? la : lb;
    printf("[me3] arm=%s seed=%u lr=%g eps=%g rounds=%d tw=%d vw=%d sw=%d acc=%d"
           " A=%.4f B=%.4f mean=%.4f mw=%+.4f mb=%+.4f barT=%.4f barV=%.4f"
           " vA=%.4f vB=%.4f vmean=%.4f\n",
           arm, seed, (double)lr, (double)ST_ADAM_EPS, ROUNDS, TW, VW, SW, acc,
           (double)la, (double)lb, (double)lm, (double)(lm - worse), (double)(lm - better),
           (double)bt, (double)bv, (double)loss_on(&A, VALW, VW),
           (double)loss_on(&B, VALW, VW), (double)loss_on(&M, VALW, VW));
    printf("[me3] done\n");
    free(blob); free(g_logits);
    return 0;
}
