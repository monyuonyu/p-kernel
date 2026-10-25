/*
 * b2_curve_test.c — [b2-curve] (ROADMAP B2; design: research/b2-size-curve.md)
 *
 * One arm of the S -> M -> L curve: a student of one tier and seed learns the
 * first D bytes of the fixture's train side (TEACHER_FIXTURE, 768 / 1536 /
 * 3072 B) one SEQ window per optimiser step, in order and round again, with
 * lr 3e-3 as B1 does. At k = 96, 192, 384, 768, 1536 (up to kmax) it prints
 * the bits per byte on the independent held text (tests/llm/b1_held.txt, 331
 * windows), evaluating inside the one training run (no retraining per point).
 *
 * Also printed once per arm: the 16-byte runs the trained bytes share with the
 * held text (G3), and the byte unigram / bigram comparators counted from the
 * arm's own D bytes (audit-83 note 1: counting them on held would leak).
 * -DB2_LEAK interleaves the held text's first half into the training stream
 * (the negative control LEAK). The pass lines are judged by b2_curve_check.py.
 *
 *   b2_curve_test <held.txt> <tier 0|1|2> <D> <seed> <kmax>    exit 0 = ran
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../../arch/common/llm/student.h"
#include "../../arch/common/llm/held_indep.h"
#include "../../arch/common/llm/teacher_fixture.h"

#define SEQ 32
#define LN2 0.69314718055994530942

static uint8_t held[65536];
static uint8_t stream[65536 + 4096];

static double bpb(st_model *m, int n, int *wins, int *skipped)
{
    double s = 0.0; int got = 0, skip = 0;
    for (int w = 0; (w + 1) * SEQ <= n; w++) {
        int np = 0;
        float l = st_eval_loss(m, held + w * SEQ, SEQ, &np);
        if (np) { s += l; got++; } else skip++;
    }
    *wins = got; *skipped = skip;
    return got ? s / got / LN2 : 0.0;
}

int main(int argc, char **argv)
{
    if (argc < 6) { fprintf(stderr, "usage: %s <held.txt> <tier> <D> <seed> <kmax>\n", argv[0]); return 2; }
    int tier = atoi(argv[2]), D = atoi(argv[3]), kmax = atoi(argv[5]);
    uint32_t seed = (uint32_t)strtoul(argv[4], NULL, 10);
    float lr = 3e-3f;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    int n = (int)fread(held, 1, sizeof held, f);
    fclose(f);

    const uint8_t *fx = (const uint8_t *)TEACHER_FIXTURE;
    int fxn = (int)sizeof(TEACHER_FIXTURE) - 1;
    int fx_end = st_held_split(fxn, SEQ).train_end;
    if (D <= 0 || D % SEQ || D > fx_end) {
        printf("[b2] FAIL D=%d is not a multiple of %d within the train side 0..%d\n", D, SEQ, fx_end);
        return 2;
    }

    /* the training stream: D bytes of the fixture (+ the held text's first half) */
    memcpy(stream, fx, (size_t)D);
    int sn = D;
#ifdef B2_LEAK
    int leak = 1, half = (n / 2 / SEQ) * SEQ;
    memcpy(stream + sn, held, (size_t)half);
    sn += half;
#else
    int leak = 0;
#endif

    /* 16-byte runs shared with the held text, over the fixture bytes trained */
    st_held_report r;
    int rc = st_held_check_seen(held, n, n, 0, 1, fx, D, ST_HELD_K, &r);
    int shared = rc < 0 ? -1 : r.shared;
    if (leak) {
        rc = st_held_check_seen(held, n, n, 0, 1, stream, sn, ST_HELD_K, &r);
        shared = rc < 0 ? -1 : r.shared;
    }

    /* comparators on the predicted held bytes, counted from the arm's D bytes */
    static double c1[256], c2[256][256];
    double t1 = 0, u = 0, b = 0; int np = 0;
    for (int i = 0; i < D; i++) { c1[fx[i]] += 1; t1 += 1; }
    for (int i = 0; i + 1 < D; i++) c2[fx[i]][fx[i + 1]] += 1;
    for (int w = 0; (w + 1) * SEQ <= n; w++)
        for (int t = 1; t < SEQ; t++) {
            uint8_t p = held[w * SEQ + t - 1], c = held[w * SEQ + t];
            u += -log((c1[c] + 1) / (t1 + 256));
            b += -log((c2[p][c] + 1) / (c1[p] + 256));
            np++;
        }
    printf("[b2] arm tier=%d D=%d seed=%u leak=%d shared=%d unigram=%.4f bigram=%.4f held_windows=%d\n",
           tier, D, (unsigned)seed, leak, shared, u / np / LN2, b / np / LN2, n / SEQ);

    st_model m;
    if (st_init_tier(&m, seed, tier) != 0) { printf("[b2] FAIL init tier %d\n", tier); return 1; }
    float *logits = (float *)malloc((size_t)SEQ * ST_VOCAB * sizeof(float));
    if (!logits) return 2;
    static const int at[] = { 96, 192, 384, 768, 1536 };
    int next = 0, o = 0;
    for (int k = 1; k <= kmax; k++) {
        ST_TRAIN_STEP(&m, stream + o, SEQ, logits, lr);
        o += SEQ;
        if (o + SEQ > sn) o = 0;
        if (next < (int)(sizeof at / sizeof at[0]) && k == at[next]) {
            int wins = 0, skipped = 0;
            double v = bpb(&m, n, &wins, &skipped);
            printf("[b2] point k=%d bpb=%.4f wins=%d skipped=%d\n", k, v, wins, skipped);
            fflush(stdout);
            next++;
        }
    }
    free(logits);
    st_free(&m);
    printf("[b2] done\n");
    return 0;
}
