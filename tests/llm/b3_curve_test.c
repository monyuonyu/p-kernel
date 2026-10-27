/*
 * b3_curve_test.c — one arm of [b3-data] (ROADMAP B3; design research/b3-data.md)
 *
 * A student of one tier and seed learns the B3 texts (made by the teacher with
 * b3_gen.py, kept by b3_filter.py, joined into one file) one SEQ window per
 * optimiser step, in order, each byte once (lr 3e-3, SEQ 32, as B1 and B2).
 * After U = 24 KB, 96 KB, 384 KB and 1.5 MB of unique bytes (up to Umax) it
 * prints the bits per byte on
 *   - the independent held text tests/llm/b1_held.txt (331 windows), also
 *     split into its first half (held_a) and second half (held_b) for LEAK
 *   - gen_held: 32 KB of teacher texts kept out of training
 * evaluating inside the one training run.
 *
 * Modes:
 *   plain  the first U bytes of the texts, once
 *   rep    the first 24 KB repeated until U (B2's answer: repeating is not
 *          amount; the comparator REP)
 *   leak   plain, with b1_held's first half mixed in at the middle of each
 *          quarter of the bytes up to Umax (4 times; the negative control
 *          LEAK); only the point at Umax is printed
 * Printed once per arm: held64 = the trained positions whose 64-byte run is
 * also in b1_held (G3; 0 after the filter's q5, not 0 in the leak arm).
 *
 *   b3_curve_test <held.txt> <gen_held.txt> <train.txt> <tier 0|1> <seed>
 *                 <plain|rep|leak> <Umax>               exit 0 = ran
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../../arch/common/llm/student.h"

#define SEQ 32
#define RUN 64
#define LN2 0.69314718055994530942

static const int US[] = { 24576, 98304, 393216, 1572864 };
#define NU ((int)(sizeof US / sizeof US[0]))

static uint8_t held[65536], gen[65536];

static int slurp(const char *path, uint8_t **buf, int cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    if (!*buf) *buf = (uint8_t *)malloc((size_t)cap);
    int n = *buf ? (int)fread(*buf, 1, (size_t)cap, f) : -1;
    fclose(f);
    return n;
}

/* bpb over windows [w0, w1) of text t */
static double bpb(st_model *m, const uint8_t *t, int w0, int w1, int *wins, int *skipped)
{
    double s = 0.0; int got = 0, skip = 0;
    for (int w = w0; w < w1; w++) {
        int np = 0;
        float l = st_eval_loss(m, t + w * SEQ, SEQ, &np);
        if (np) { s += l; got++; } else skip++;
    }
    if (wins) *wins = got;
    if (skipped) *skipped = skip;
    return got ? s / got / LN2 : 0.0;
}

static uint64_t h64(const uint8_t *p)
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < RUN; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* trained positions whose 64-byte run is also somewhere in the held text */
static long held64(const uint8_t *s, long sn, int hn)
{
    int nh = hn - RUN + 1;
    if (nh < 1 || sn < RUN) return 0;
    uint64_t *hs = (uint64_t *)malloc((size_t)nh * sizeof *hs);
    if (!hs) return -1;
    for (int i = 0; i < nh; i++) hs[i] = h64(held + i);
    qsort(hs, (size_t)nh, sizeof *hs, cmp_u64);
    long hit = 0;
    for (long o = 0; o + RUN <= sn; o++) {
        uint64_t h = h64(s + o);
        if (!bsearch(&h, hs, (size_t)nh, sizeof *hs, cmp_u64)) continue;
        for (int i = 0; i < nh; i++)   /* confirm the bytes (hash hits are rare) */
            if (memcmp(held + i, s + o, RUN) == 0) { hit++; break; }
    }
    free(hs);
    return hit;
}

int main(int argc, char **argv)
{
    if (argc != 8) {
        fprintf(stderr, "usage: %s <held> <gen_held> <train> <tier> <seed> <plain|rep|leak> <Umax>\n", argv[0]);
        return 2;
    }
    int tier = atoi(argv[4]), umax = atoi(argv[7]);
    uint32_t seed = (uint32_t)strtoul(argv[5], NULL, 10);
    const char *mode = argv[6];
    int rep = !strcmp(mode, "rep"), leak = !strcmp(mode, "leak");
    if (!rep && !leak && strcmp(mode, "plain")) { printf("[b3] FAIL mode %s\n", mode); return 2; }
    uint8_t *hp = held, *gp = gen, *tr = NULL;
    int hn = slurp(argv[1], &hp, sizeof held);
    int gn = slurp(argv[2], &gp, sizeof gen);
    int tn = slurp(argv[3], &tr, 1 << 24);
    if (hn < SEQ || gn < SEQ || tn < 0) return 2;
    int nu = 0;
    while (nu < NU && US[nu] <= umax) nu++;
    if (nu == 0 || US[nu - 1] != umax) { printf("[b3] FAIL Umax %d is not one of the points\n", umax); return 2; }
    int need = rep ? US[0] : umax;
    if (tn < need) { printf("[b3] FAIL the texts are %d bytes, %d needed\n", tn, need); return 2; }

    /* the training stream */
    int hw = hn / SEQ, half = (hw / 2) * SEQ;
    long sn = (long)umax + (leak ? 4L * half : 0);
    uint8_t *s = (uint8_t *)malloc((size_t)sn);
    if (!s) return 2;
    /* where each U point ends in the stream; the leak arm is judged at Umax only */
    long u_end[NU];
    int next = 0;
    if (rep) {
        for (long o = 0; o < sn; o++) s[o] = tr[o % US[0]];
    } else if (leak) {
        /* the held text's first half in the middle of each quarter of the texts */
        long o = 0, c = 0;
        for (int q = 0; q < 4; q++) {
            long mid = (long)umax / 8 * (2 * q + 1);
            memcpy(s + o, tr + c, (size_t)(mid - c)); o += mid - c; c = mid;
            memcpy(s + o, held, (size_t)half); o += half;
        }
        memcpy(s + o, tr + c, (size_t)(umax - c));
        next = nu - 1;
    } else {
        memcpy(s, tr, (size_t)umax);
    }
    for (int i = 0; i < nu; i++) u_end[i] = US[i];
    if (leak) u_end[nu - 1] = sn;

    printf("[b3] arm tier=%s seed=%u mode=%s held64=%ld held_windows=%d\n",
           tier == 0 ? "S" : tier == 1 ? "M" : "?", (unsigned)seed, mode,
           held64(s, sn, hn), hw);
    fflush(stdout);

    st_model m;
    if (st_init_tier(&m, seed, tier) != 0) { printf("[b3] FAIL init tier %d\n", tier); return 1; }
    float *logits = (float *)malloc((size_t)SEQ * ST_VOCAB * sizeof(float));
    if (!logits) return 2;
    float lr = 3e-3f;
    long k = 0;
    for (long o = 0; o + SEQ <= sn && next < nu; o += SEQ) {
        ST_TRAIN_STEP(&m, s + o, SEQ, logits, lr);
        k++;
        if (o + SEQ == u_end[next]) {
            int wins = 0, skipped = 0, wa, wb, wg;
            double v = bpb(&m, held, 0, hw, &wins, &skipped);
            double a = bpb(&m, held, 0, hw / 2, &wa, NULL);
            double b = bpb(&m, held, hw / 2, hw, &wb, NULL);
            double g = bpb(&m, gen, 0, gn / SEQ, &wg, NULL);
            printf("[b3] point U=%d k=%ld bpb=%.4f held_a=%.4f held_b=%.4f gen=%.4f wins=%d skipped=%d\n",
                   US[next], k, v, a, b, g, wins, skipped);
            fflush(stdout);
            next++;
        }
    }
    free(logits);
    st_free(&m);
    free(s);
    free(tr);
    if (next != nu) { printf("[b3] FAIL reached %d of %d points\n", next, nu); return 1; }
    printf("[b3] done\n");
    return 0;
}
