/*
 * student_outer_test.c - [b5-outer] B5 step 11 (research/b5-outer.md): the
 * round's merge as an outer gradient with Nesterov momentum (DiLoCo, Douillard
 * et al. arXiv:2311.08105 v3, Algorithm 1 and Sec. 3.1), st_round_outer.
 * Written before the function (test first): on a tree without it this file
 * does not build (the #error below), which run_b5_outer.sh reports as RED.
 * Lines fixed in the design before any judgement seed; judge b5_outer_check.py.
 *
 *   student_outer_test unit                   O4: the function's own cases
 *   student_outer_test <b1_held.txt> <seed>   one seed of the arms below
 *   student_outer_test <b1_held.txt> <seed> nc   the outer4 arm only (NC builds)
 *
 * Arms (S, ST_LR, common init, four nodes each a quarter of the first D bytes of
 * the fixture's train side, 32 passes, a round every 4 = 8 outer steps). The
 * product's form: candidate = st_round_trimmed (t = 1) of self and the three
 * others into a copy, then st_round_adopt on the fixture's held side (the gate);
 * no apprentice (student_region_round has none). Each arm's answer is node 0's
 * model at its best gate pass ("kept", step 4's keeper); the ruler is
 * b1_held.txt in bits per byte and never chooses.
 *   alone-A        one node, quarter 0
 *   trim4          the product's round (st_region_round's merge and gate)
 *   outer4         trim4, then st_round_outer(anchor = the node's live at the
 *                  start of the round, ST_OUTER_LR, ST_OUTER_MU) before the gate;
 *                  a refused candidate zeroes the node's momentum
 *   outer4-poison  outer4 with node 3 sending garbage (another init x8) every round
 *   look1          reading only: one node, quarter 0, the candidate is its own
 *                  live (Adam restarted) through st_round_outer and the gate
 *                  (DiLoCo Fig. 9, k = 1: is the gain the momentum or the swarm?)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../../arch/common/llm/student.h"
#include "../../arch/common/llm/held_indep.h"
#include "../../arch/common/llm/teacher_fixture.h"

#if !defined(ST_OUTER_LR) || !defined(ST_OUTER_MU)
#error "st_round_outer is not in student.h yet (B5 step 11, research/b5-outer.md)"
#endif

#define SEQ 32
#define D 3072
#define PASSES 32
#define EVERY 4
#define K 4
#define LN2 0.69314718055994530942

static uint8_t held[65536];
static int hn;
static const uint8_t *fx;
static int fxn, fx_end;
static float *lg;
static int fails;

static void check(int ok, const char *id, const char *what)
{
    printf("  %s  [%s] %s\n", ok ? "PASS" : "FAIL", id, what);
    if (!ok) fails++;
}
static double bpb(st_model *m)
{
    double s = 0; int got = 0;
    for (int w = 0; (w + 1) * SEQ <= hn; w++) {
        int np = 0; float l = st_eval_loss(m, held + w * SEQ, SEQ, &np);
        if (np) { s += l; got++; }
    }
    return got ? s / got / LN2 : 0;
}
static double gate_loss(st_model *m)
{
    double s = 0; int got = 0;
    for (int o = fx_end; o + SEQ <= fxn; o += SEQ) {
        int np = 0; float l = st_eval_loss(m, fx + o, SEQ, &np);
        if (np) { s += l; got++; }
    }
    return got ? s / got : 1e9;
}
static void pass_q(st_model *m, int q)
{
    int a = q * (D / K), b = a + D / K;
    for (int o = a; o + SEQ <= b; o += SEQ) ST_TRAIN_STEP(m, fx + o, SEQ, lg, ST_LR);
}
static void *blob(st_model *m, size_t *len)
{
    size_t cap = st_blob_size(m);
    void *b = malloc(cap);
    long n = b ? st_save(m, b, cap) : -1;
    if (n < 0) { printf("  FAIL  [b5o-io] save\n"); exit(1); }
    *len = (size_t)n;
    return b;
}
static void clone(st_model *dst, st_model *src)
{
    size_t n; void *b = blob(src, &n);
    if (st_init_tier(dst, 1u, src->tier) != 0 || st_load(dst, b, n) != 0) { printf("  FAIL  [b5o-io] clone\n"); exit(1); }
    free(b);
}
static void garbage(st_model *g, uint32_t seed)
{
    st_init_tier(g, seed ^ 0xBADu, ST_TIER_S);
    for (int i = 0; i < g->n_params; i++) g->w[i] *= 8.0f;
}
static int same_bytes(const void *a, const void *b, size_t n) { return memcmp(a, b, n) == 0; }

/* ---- O4: the function's own cases ---------------------------------------- */
static int unit(void)
{
    st_model a, c, c0; float *mom, *mom0, *anc;
    st_init_tier(&a, 7u, ST_TIER_S); st_init_tier(&c, 8u, ST_TIER_S);
    int n = a.n_params; size_t nb = (size_t)n * sizeof(float);
    mom = calloc((size_t)n, sizeof(float)); mom0 = malloc(nb); anc = malloc(nb);
    float *w0 = malloc(nb);   /* a snapshot of the candidate w (a model holding NaN cannot be cloned: st_load refuses it) */
    memcpy(anc, a.w, nb);
    for (int i = 0; i < n; i++) { c.mu[i] = 0.0f; c.vu[i] = 0.0f; }
    c.adam_t = 0;

    /* o4-id: lr 1, mu 0 is the plain merge (FedAvg, DiLoCo Sec. 2): every value equal */
    clone(&c0, &c);
    int r = st_round_outer(&c, anc, mom, 1.0f, 0.0f);
    int eq = r == 1;
    for (int i = 0; eq && i < n; i++) eq = c.w[i] == c0.w[i] && mom[i] == (float)((double)anc[i] - (double)c0.w[i]);
    check(eq, "o4-id", "lr 1 mu 0: the candidate's values are the merge's, mom = anchor - merge");
    st_free(&c0);

    /* o4-ref: the formula of student.h, in double per element, stored as float */
    for (int i = 0; i < n; i++) mom[i] = 0.01f * (float)((i % 7) - 3);
    memcpy(mom0, mom, nb);
    clone(&c0, &c);
    r = st_round_outer(&c, anc, mom, 0.7f, 0.9f);
    int ok = r == 1;
    for (int i = 0; ok && i < n; i++) {
        double d = (double)anc[i] - (double)c0.w[i];
        double m = (double)0.9f * (double)mom0[i] + d;   /* mu and lr as the floats passed in */
        float mf = (float)m;
        float wf = (float)((double)anc[i] - (double)0.7f * (d + (double)0.9f * (double)mf));
        ok = same_bytes(&mf, &mom[i], 4) && same_bytes(&wf, &c.w[i], 4);
        if (!ok) printf("        i=%d mom %a want %a, w %a want %a\n", i, (double)mom[i], (double)mf, (double)c.w[i], (double)wf);
    }
    check(ok, "o4-ref", "lr 0.7 mu 0.9: mom and w to the byte as the double formula (Nesterov on the stored mom)");
    ok = 1;
    for (int i = 0; ok && i < n; i++) ok = c.mu[i] == 0.0f && c.vu[i] == 0.0f;
    check(ok && c.adam_t == 0, "o4-adam", "the candidate's Adam moments and adam_t are not touched");
    st_free(&c0);

    /* o4-nan: any non-finite in anchor, candidate or mom is a refusal, nothing changes */
    const char *what[3] = { "anchor", "candidate", "mom" };
    for (int k = 0; k < 3; k++) {
        float *anc2 = malloc(nb); memcpy(anc2, anc, nb);
        float *p = k == 0 ? anc2 : k == 1 ? c.w : mom;
        float keep = p[n / 2];
        p[n / 2] = k == 2 ? INFINITY : NAN;
        memcpy(w0, c.w, nb); memcpy(mom0, mom, nb);   /* the snapshot holds the poisoned value */
        r = st_round_outer(&c, anc2, mom, 0.7f, 0.9f);
        int un = r == 0 && same_bytes(c.w, w0, nb) && same_bytes(mom, mom0, nb);
        char id[32], msg[96]; snprintf(id, sizeof id, "o4-nan-%d", k);
        snprintf(msg, sizeof msg, "a non-finite %s: refused (0), candidate and mom byte-unchanged", what[k]);
        check(un, id, msg);
        p[n / 2] = keep;
        free(anc2);
    }

    /* o4-ovf: a finite input whose result overflows is a refusal, nothing changes */
    memcpy(w0, c.w, nb); memcpy(mom0, mom, nb);
    float *anc3 = malloc(nb); memcpy(anc3, anc, nb);
    anc3[3] = 3.0e38f; float cw = c.w[3]; c.w[3] = -3.0e38f; w0[3] = -3.0e38f;
    r = st_round_outer(&c, anc3, mom, 0.7f, 0.9f);
    check(r == 0 && same_bytes(c.w, w0, nb) && same_bytes(mom, mom0, nb), "o4-ovf",
          "finite inputs with an overflowing result: refused, byte-unchanged");
    c.w[3] = cw; free(anc3);

    /* o4-arg: NULL and out-of-range lr (0 < lr <= 2) and mu (0 <= mu < 1) */
    memcpy(w0, c.w, nb); memcpy(mom0, mom, nb);
    int bad = 0;
    bad += st_round_outer(NULL, anc, mom, 0.7f, 0.9f) == ST_E_ARG;
    bad += st_round_outer(&c, NULL, mom, 0.7f, 0.9f) == ST_E_ARG;
    bad += st_round_outer(&c, anc, NULL, 0.7f, 0.9f) == ST_E_ARG;
    bad += st_round_outer(&c, anc, mom, 0.0f, 0.9f) == ST_E_ARG;
    bad += st_round_outer(&c, anc, mom, 2.5f, 0.9f) == ST_E_ARG;
    bad += st_round_outer(&c, anc, mom, 0.7f, -0.1f) == ST_E_ARG;
    bad += st_round_outer(&c, anc, mom, 0.7f, 1.0f) == ST_E_ARG;
    bad += st_round_outer(&c, anc, mom, NAN, 0.9f) == ST_E_ARG;
    check(bad == 8 && same_bytes(c.w, w0, nb) && same_bytes(mom, mom0, nb), "o4-arg",
          "NULL, lr outside (0, 2], mu outside [0, 1), NaN lr: ST_E_ARG, byte-unchanged");
   

    /* o4-def: the defaults are DiLoCo's (Sec. 3.1: outer lr 0.7, momentum 0.9) */
    check(ST_OUTER_LR == 0.7f && ST_OUTER_MU == 0.9f, "o4-def", "ST_OUTER_LR 0.7, ST_OUTER_MU 0.9");

    free(mom); free(mom0); free(anc); free(w0); st_free(&a); st_free(&c);
    printf("[b5o-unit] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

/* ---- the arms ------------------------------------------------------------ */
typedef struct { double best_g, best_b; int best_p; } keeper_t;
static void keep_see(keeper_t *k, st_model *m, int p)
{
    double g = gate_loss(m);
    if (g < k->best_g) { k->best_g = g; k->best_p = p; k->best_b = bpb(m); }
}
static void keep_print(keeper_t *k, const char *name, st_model *last, const char *extra)
{
    printf("[b5o] arm=%s last=%.4f kept=%.4f at %d (gate %.4f) %s\n",
           name, bpb(last), k->best_b, k->best_p, k->best_g, extra);
}
static void alone(uint32_t seed)
{
    st_model m; st_init_tier(&m, seed, ST_TIER_S);
    keeper_t k = { 1e9, 0, 0 };
    for (int p = 1; p <= PASSES; p++) { pass_q(&m, 0); keep_see(&k, &m, p); }
    keep_print(&k, "alone-A", &m, "");
    st_free(&m);
}
/* outer: 0 trim4, 1 outer4; poison: node 3 sends garbage */
static void rounds(uint32_t seed, int outer, int poison, const char *name)
{
    st_model n[K]; float *anc[K], *mom[K];
    for (int k = 0; k < K; k++) {
        st_init_tier(&n[k], seed, ST_TIER_S);
        size_t nb = (size_t)n[k].n_params * sizeof(float);
        anc[k] = malloc(nb); memcpy(anc[k], n[k].w, nb);
        mom[k] = calloc((size_t)n[k].n_params, sizeof(float));
    }
    keeper_t kp = { 1e9, 0, 0 };
    int took = 0, tries = 0, orefused = 0; uint32_t gen = 0;
    int honest = K - poison;
    for (int p = 1; p <= PASSES; p++) {
        for (int k = 0; k < honest; k++) pass_q(&n[k], k);
        if (p % EVERY == 0) {
            gen++;
            for (int k = honest; k < K; k++) { st_free(&n[k]); garbage(&n[k], seed + (uint32_t)p + 977u * (uint32_t)k); }
            st_model c[K];
            for (int k = 0; k < honest; k++) {          /* all candidates from the same round */
                clone(&c[k], &n[k]);
                const void *bs[K - 1]; size_t ls[K - 1]; uint32_t gs[K - 1]; void *own[K - 1]; int j = 0;
                for (int q = 0; q < K; q++) if (q != k) { own[j] = blob(&n[q], &ls[j]); bs[j] = own[j]; gs[j] = gen; j++; }
                int r = st_round_trimmed(&c[k], gen, bs, ls, gs, K - 1, 1);
                if (r != K - 1) { printf("  FAIL  [b5o-io] merge took %d of %d\n", r, K - 1); exit(1); }
                for (int i = 0; i < K - 1; i++) free(own[i]);
            }
            for (int k = 0; k < honest; k++) {
                int ok = 1;
                if (outer) ok = st_round_outer(&c[k], anc[k], mom[k], ST_OUTER_LR, ST_OUTER_MU) == 1;
                int r = ok && st_round_adopt(&n[k], &c[k], fx + fx_end, fxn - fx_end, SEQ, NULL, NULL) == 1;
                if (!ok && k == 0) orefused++;
                if (!r) memset(mom[k], 0, (size_t)n[k].n_params * sizeof(float));
                if (k == 0) { took += r; tries++; }
                st_free(&c[k]);
                memcpy(anc[k], n[k].w, (size_t)n[k].n_params * sizeof(float));   /* the next round starts here */
            }
        }
        keep_see(&kp, &n[0], p);
    }
    char ex[64]; snprintf(ex, sizeof ex, "took=%d/%d orefused=%d", took, tries, orefused);
    keep_print(&kp, name, &n[0], ex);
    for (int k = 0; k < K; k++) { st_free(&n[k]); free(anc[k]); free(mom[k]); }
}
static void look1(uint32_t seed)
{
    st_model a; st_init_tier(&a, seed, ST_TIER_S);
    size_t nb = (size_t)a.n_params * sizeof(float);
    float *anc = malloc(nb), *mom = calloc((size_t)a.n_params, sizeof(float));
    memcpy(anc, a.w, nb);
    keeper_t kp = { 1e9, 0, 0 };
    int took = 0, tries = 0;
    for (int p = 1; p <= PASSES; p++) {
        pass_q(&a, 0);
        if (p % EVERY == 0) {
            st_model c; clone(&c, &a);
            for (int i = 0; i < c.n_params; i++) { c.mu[i] = 0.0f; c.vu[i] = 0.0f; }
            c.adam_t = 0;
            int r = st_round_outer(&c, anc, mom, ST_OUTER_LR, ST_OUTER_MU) == 1
                    && st_round_adopt(&a, &c, fx + fx_end, fxn - fx_end, SEQ, NULL, NULL) == 1;
            if (!r) memset(mom, 0, nb);
            took += r; tries++;
            st_free(&c);
            memcpy(anc, a.w, nb);
        }
        keep_see(&kp, &a, p);
    }
    char ex[48]; snprintf(ex, sizeof ex, "took=%d/%d", took, tries);
    keep_print(&kp, "look1", &a, ex);
    st_free(&a); free(anc); free(mom);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    fx = (const uint8_t *)TEACHER_FIXTURE; fxn = (int)sizeof(TEACHER_FIXTURE) - 1;
    fx_end = st_held_split(fxn, SEQ).train_end;
    lg = malloc((size_t)SEQ * ST_VOCAB * sizeof(float));
    if (!lg || fx_end < D) { printf("  FAIL  [b5o-io] setup (fx_end %d)\n", fx_end); return 1; }
    if (argc > 1 && strcmp(argv[1], "unit") == 0) return unit();
    if (argc < 3) { fprintf(stderr, "usage: %s unit | <b1_held.txt> <seed> [nc]\n", argv[0]); return 2; }
    uint32_t seed = (uint32_t)strtoul(argv[2], NULL, 10);
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    hn = (int)fread(held, 1, sizeof held, f); fclose(f);
#if defined(B5O_NC_PLAIN)
    const char *bld = "ncplain";
#elif defined(B5O_NC_FLIP)
    const char *bld = "ncflip";
#else
    const char *bld = "plain";
#endif
    int nc = argc > 3 && strcmp(argv[3], "nc") == 0;
    printf("[b5o] seed %u lr %g outer %g/%g build %s quarter %d gate windows %d held windows %d\n",
           (unsigned)seed, (double)ST_LR, (double)ST_OUTER_LR, (double)ST_OUTER_MU, bld, D / K,
           (fxn - fx_end) / SEQ, hn / SEQ);
    if (nc) { rounds(seed, 0, 0, "trim4"); rounds(seed, 1, 0, "outer4"); printf("[b5o] done\n"); return 0; }
    alone(seed);
    rounds(seed, 0, 0, "trim4");
    rounds(seed, 1, 0, "outer4");
    rounds(seed, 1, 1, "outer4-poison");
    look1(seed);
    printf("[b5o] done\n");
    return 0;
}
