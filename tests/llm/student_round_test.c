/*
 *  student_round_test.c - B5 stage 1: two babies from one ancestor, each on its
 *  half of the train windows, averaged every E passes through st_round_merge,
 *  in one process (docs/architecture/30-module/research/b5-region-rounds.md).
 *  The lines G1 to G5 were fixed before this code; the judge is b5_judge.py.
 *
 *   student_round_test gates          G4, G5 and the round's own arithmetic
 *   student_round_test run <seed>     the [b5] lines of one seed
 *
 *  B5_PEER=reset   (negative control) the peer goes back to the ancestor before
 *                  each average: G1 must go red
 *  B5_PEER=random  (negative control) the peer's weights are replaced with noise
 *                  before each average: G1 must go red
 *  -DST_ROUND_NO_GEN      the generation check off: [b5-gen] red
 *  -DST_MERGE_NO_FINITE   the finite checks off: [b5-nonfinite] red
 *
 *  Build: -std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../arch/common/llm/student.h"

#define SEQLEN 24
#define PASSES 80
#define EVERY  5          /* passes between averages, memo section 4 */
#define LR     3e-3f

/* the S2 text of student_distill_merge_test.c */
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

static int NW, TW, VW, SW, NA, NB;
static const uint8_t *ALLW[64], *TESTW[64], *TRA[64], *TRB[64];
static float *g_logits;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

static float from_u(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }

static void windows(void)
{
    NW = ((int)sizeof(CORPUS) - 1) / SEQLEN;
    SW = NW / 6; VW = NW / 6; TW = NW - SW - VW;
    for (int i = 0; i < TW; i++) ALLW[i] = CORPUS + i * SEQLEN;
    for (int i = 0; i < SW; i++) TESTW[i] = CORPUS + (TW + VW + i) * SEQLEN;
    NA = TW / 2; NB = TW - NA;
    for (int i = 0; i < NA; i++) TRA[i] = ALLW[i];
    for (int i = 0; i < NB; i++) TRB[i] = ALLW[NA + i];
    g_logits = (float *)malloc((size_t)SEQLEN * ST_VOCAB * sizeof(float));
}

static float loss_on(st_model *m, const uint8_t *const *w, int n)
{
    double t = 0.0;
    for (int i = 0; i < n; i++) { int np = 0; t += st_eval_loss(m, w[i], SEQLEN, &np); }
    return n ? (float)(t / n) : 0.0f;
}
static void pass(st_model *m, const uint8_t *const *w, int n)
{
    for (int j = 0; j < n; j++) {
        const uint8_t *b = w[(j + 3) % n];
        st_zero_grad(m); st_forward(m, b, SEQLEN, g_logits);
        st_backward(m, b, SEQLEN); st_adam_step(m, LR);
    }
}
static int clone(st_model *dst, const st_model *src)
{
    if (st_init_tier(dst, 1, src->tier) != ST_OK) return -1;
    memcpy(dst->w, src->w, (size_t)src->n_params * sizeof(float));
    return 0;
}
static uint64_t model_hash(const st_model *m)
{
    uint64_t h = 1469598103934665603ULL;
    const float *a[3] = { m->w, m->mu, m->vu };
    for (int k = 0; k < 3; k++) {
        const unsigned char *p = (const unsigned char *)a[k];
        for (size_t i = 0; i < (size_t)m->n_params * sizeof(float); i++) {
            h ^= p[i]; h *= 1099511628211ULL;
        }
    }
    return h ^ (uint64_t)(uint32_t)m->adam_t;
}
static void *blob_of(const st_model *m, size_t *len)
{
    size_t cap = st_blob_size(m);
    void *b = malloc(cap);
    if (!b) return NULL;
    long n = st_save(m, b, cap);
    if (n <= 0) { free(b); return NULL; }
    *len = (size_t)n;
    return b;
}
/* one exchange: each side gets the other's blob as it was before the round */
static int exchange(st_model *a, st_model *b, uint32_t gen)
{
    size_t la = 0, lb = 0;
    void *ba = blob_of(a, &la), *bb = blob_of(b, &lb);
    if (!ba || !bb) { free(ba); free(bb); return -1; }
    const void *pa[1] = { bb }, *pb[1] = { ba };
    size_t ra[1] = { lb }, rb[1] = { la };
    uint32_t g[1] = { gen };
    int x = st_round_merge(a, gen, pa, ra, g, 1);
    int y = st_round_merge(b, gen, pb, rb, g, 1);
    free(ba); free(bb);
    return (x == 1 && y == 1) ? 0 : -1;
}
static uint32_t g_rng = 12345u;
static float noise(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return ((float)(g_rng >> 8) / 16777216.0f - 0.5f) * 0.2f;
}

static int gates(void)
{
    st_model a, b;
    if (st_init_tier(&a, 2, ST_TIER_S) != ST_OK || st_init_tier(&b, 3, ST_TIER_S) != ST_OK) return 1;
    const int n = a.n_params;
    a.mu[0] = 0.5f; a.vu[0] = 0.25f; a.adam_t = 7;
    const uint64_t h0 = model_hash(&a);
    size_t l = 0;
    void *p = blob_of(&b, &l);
    const void *pb[2] = { p, p };
    size_t pl[2] = { l, l };

    /* G5 */
    static const uint32_t other[] = { 8, 6, 0, 0xFFFFFFFFu };
    int bad = 0;
    for (unsigned i = 0; i < sizeof other / sizeof other[0]; i++) {
        uint32_t g[1] = { other[i] };
        int r = st_round_merge(&a, 7, pb, pl, g, 1);
        if (r != 0 || model_hash(&a) != h0) {
            printf("    generation %u against 7: accepted %d\n", other[i], r);
            bad++;
        }
    }
    CHECK(bad == 0 && model_hash(&a) == h0, "[b5-gen] weights of another generation: not taken, all bytes the same");

    /* G4, through the round */
    float *w0 = (float *)malloc((size_t)n * sizeof(float));
    memcpy(w0, b.w, (size_t)n * sizeof(float));
    const int at[] = { b.o_out + 'e' * b.d, b.o_embed + 255 * b.d, n - 1 };
    static const uint32_t pat[] = { 0x7FC00000u, 0x7F800000u, 0xFF800000u };
    bad = 0;
    for (unsigned k = 0; k < 3 && model_hash(&a) == h0; k++)
        for (unsigned q = 0; q < 3 && model_hash(&a) == h0; q++) {
            b.w[at[k]] = from_u(pat[q]);
            size_t lp = 0;
            void *pp = blob_of(&b, &lp);
            b.w[at[k]] = w0[at[k]];
            const void *one[1] = { pp };
            size_t ol[1] = { lp };
            uint32_t g[1] = { 7 };
            int r = st_round_merge(&a, 7, one, ol, g, 1);
            free(pp);
            if (r != 0 || model_hash(&a) != h0) {
                printf("    element %d pattern 0x%08X: accepted %d\n", at[k], pat[q], r);
                bad++;
            }
        }
    CHECK(bad == 0 && model_hash(&a) == h0, "[b5-nonfinite] NaN or Inf in a peer: not taken, all bytes the same");

    if (model_hash(&a) == h0) {
        float ka = a.w[5], kb = b.w[5];
        a.w[5] = 3e38f; b.w[5] = 3e38f;
        uint64_t h = model_hash(&a);
        size_t lp = 0;
        void *pp = blob_of(&b, &lp);
        const void *one[1] = { pp };
        size_t ol[1] = { lp };
        uint32_t g[1] = { 7 };
        int r = st_round_merge(&a, 7, one, ol, g, 1);
        free(pp);
        CHECK(r == 0 && model_hash(&a) == h, "[b5-overflow] a sum that overflows: not taken, all bytes the same");
        a.w[5] = ka; b.w[5] = kb;
    } else {
        CHECK(0, "[b5-overflow] the model had already changed");
    }

    /* one of two: the peer of this generation only */
    {
        st_model c1, c2;
        if (st_init_tier(&c1, 2, ST_TIER_S) != ST_OK || st_init_tier(&c2, 2, ST_TIER_S) != ST_OK) return 1;
        uint32_t g1[1] = { 7 }, g2[2] = { 6, 7 };
        int r1 = st_round_merge(&c1, 7, pb, pl, g1, 1);
        int r2 = st_round_merge(&c2, 7, pb, pl, g2, 2);
        CHECK(r1 == 1 && r2 == 1 && model_hash(&c1) == model_hash(&c2),
              "[b5-gen-mixed] of two peers the one of this generation is taken, same bits as alone");
        st_free(&c1); st_free(&c2);
    }

    /* the average is symmetric, the two sides leave the round identical */
    {
        st_model x, y;
        if (st_init_tier(&x, 2, ST_TIER_S) != ST_OK || st_init_tier(&y, 3, ST_TIER_S) != ST_OK) return 1;
        int r = exchange(&x, &y, 0);
        CHECK(r == 0 && model_hash(&x) == model_hash(&y), "[b5-same] both sides leave a round with the same bytes");
        st_free(&x); st_free(&y);
    }
    uint32_t none[1] = { 7 };
    CHECK(st_round_merge(&a, 7, NULL, pl, none, 1) < 0 && st_round_merge(&a, 7, pb, pl, NULL, 1) < 0,
          "[b5-args] missing arrays are an error");

    free(p); free(w0); st_free(&a); st_free(&b);
    printf("[result] %s (%d pass, %d fail)\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

static int run(uint64_t seed)
{
    const char *peer = getenv("B5_PEER");
    int reset = peer && strcmp(peer, "reset") == 0;
    int rnd   = peer && strcmp(peer, "random") == 0;
    st_model I, A, B;
    if (st_init_tier(&I, seed, ST_TIER_S) != ST_OK) return 1;

    /* the parents: never averaged */
    if (clone(&A, &I) || clone(&B, &I)) return 1;
    for (int r = 0; r < PASSES; r++) { pass(&A, TRA, NA); pass(&B, TRB, NB); }
    float pa = loss_on(&A, ALLW, TW), pb = loss_on(&B, ALLW, TW);
    float ta = loss_on(&A, TESTW, SW), tb = loss_on(&B, TESTW, SW);
    st_free(&A); st_free(&B);

    /* the rounds */
    if (clone(&A, &I) || clone(&B, &I)) return 1;
    uint32_t gen = 0;
    int merged = 0;
    for (int r = 1; r <= PASSES; r++) {
        pass(&A, TRA, NA); pass(&B, TRB, NB);
        if (r % EVERY) continue;
        if (reset) {
            memcpy(B.w, I.w, (size_t)B.n_params * sizeof(float));
        } else if (rnd) {
            for (int i = 0; i < B.n_params; i++) B.w[i] = noise();
        }
        if (exchange(&A, &B, gen) != 0) { printf("[b5] seed %llu exchange failed\n", (unsigned long long)seed); return 1; }
        gen++;
        merged++;
    }
    int same = model_hash(&A) == model_hash(&B);
    float ca = loss_on(&A, ALLW, TW), ct = loss_on(&A, TESTW, SW);
    printf("[b5] seed %llu parents both A %.4f B %.4f TEST A %.4f B %.4f child both %.4f TEST %.4f rounds %d same %d peer %s\n",
           (unsigned long long)seed, (double)pa, (double)pb, (double)ta, (double)tb,
           (double)ca, (double)ct, merged, same, peer ? peer : "plain");
    st_free(&A); st_free(&B); st_free(&I);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    windows();
    if (argc > 1 && strcmp(argv[1], "gates") == 0) return gates();
    if (argc > 2 && strcmp(argv[1], "run") == 0) return run(strtoull(argv[2], NULL, 0));
    fprintf(stderr, "usage: student_round_test gates | run <seed>\n");
    return 2;
}
