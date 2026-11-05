/*
 * student_adopt_test.c - [b5-adopt] B5 step 2 (research/b5-step2.md): the
 * round's average goes into a copy and replaces the live model only if the
 * copy is finite and its loss on the node's held windows is lower
 * (st_round_adopt). Lines fixed in the design before the code; the judge is
 * tests/llm/b5_adopt_check.py.
 *
 *   student_adopt_test unit                  H4: the gate's own cases
 *   student_adopt_test <b1_held.txt> <seed>  one seed of the arms below
 *   student_adopt_test <b1_held.txt> <seed> s4   B5 step 4 (s4() below, run_b5_step4.sh)
 *
 * Arms (S, common init, the first 3072 B of the fixture's train side in two
 * halves, one pass = one node's half once, 32 passes, a merge every 4):
 *   alone-A, alone-B   one node, its half only
 *   full               one node, both halves, the same number of steps
 *   always             step 1's form: st_round_merge straight into live
 *   adopt              into a copy, st_round_adopt on the fixture's held side
 *   poison-always/-adopt  node B sends garbage (another init, x8, finite)
 * The ruler is b1_held.txt (bits per byte); it is never used to choose.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../arch/common/llm/student.h"
#include "../../arch/common/llm/held_indep.h"
#include "../../arch/common/llm/teacher_fixture.h"

#define SEQ 32
#define D 3072
#define PASSES 32
#define EVERY 4
#define LR 3e-3f
#define LN2 0.69314718055994530942

static uint8_t held[65536];
static int hn;
static const uint8_t *fx;
static int fxn, fx_end;
static float *lg;
static int fails;
static float mild = 1.3f;  /* diag: the mild poison's scale (argv[4]) */
static int diag;    /* argv[3] == "diag": the gate's two losses per round, always and adopt only */

static void check(int ok, const char *id, const char *what)
{
    printf("  %s  [%s] %s\n", ok ? "PASS" : "FAIL", id, what);
    if (!ok) fails++;
}
static uint64_t hash(const st_model *m)
{
    uint64_t h = 1469598103934665603ULL;
    const float *a[3] = { m->w, m->mu, m->vu };
    for (int k = 0; k < 3; k++) {
        const unsigned char *p = (const unsigned char *)a[k];
        for (size_t i = 0; i < (size_t)m->n_params * sizeof(float); i++) { h ^= p[i]; h *= 1099511628211ULL; }
    }
    h ^= (uint64_t)(uint32_t)m->adam_t; h *= 1099511628211ULL;
    return h;
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
static float lr = LR;   /* steps 2-3: LR (their judgement seeds were run on it); s4: ST_LR */
static void pass(st_model *m, int half)
{
    int a = half < 0 ? 0 : half * (D / 2), b = half < 0 ? D : a + D / 2;
    for (int o = a; o + SEQ <= b; o += SEQ) ST_TRAIN_STEP(m, fx + o, SEQ, lg, lr);
}
static void *blob(st_model *m, size_t *len)
{
    size_t cap = st_blob_size(m);
    void *b = malloc(cap);
    long n = b ? st_save(m, b, cap) : -1;
    if (n < 0) { printf("  FAIL  [b5a-io] save\n"); exit(1); }
    *len = (size_t)n;
    return b;
}
static void clone(st_model *dst, st_model *src)
{
    size_t n; void *b = blob(src, &n);
    if (st_init_tier(dst, 1u, src->tier) != 0 || st_load(dst, b, n) != 0) { printf("  FAIL  [b5a-io] clone\n"); exit(1); }
    free(b);
}
static void garbage(st_model *g, uint32_t seed)
{
    st_init_tier(g, seed ^ 0xBADu, ST_TIER_S);
    for (int i = 0; i < g->n_params; i++) g->w[i] *= 8.0f;
}
/* B5 step 3 (research/b5-step3.md): a copy the gate refused trains one round
 * alongside live as an apprentice, then replaces live only if lower then. */
typedef struct { st_model m; int has; } apprentice_t;
static int apprentice_late;   /* apprentices taken, this arm */

/* before a new merge: the apprentice's turn at the gate, then it is gone */
static void apprentice_judge(st_model *me, apprentice_t *ap, uint32_t gen)
{
    if (!ap || !ap->has) return;
    float ll = 0, cl = 0;
#if defined(B5A_APP_ALWAYS)          /* falsifier for H5: the apprentice always replaces live */
    st_model *keep = &ap->m;
    int r = st_round_adopt(me, keep, fx + fx_end, fxn - fx_end, SEQ, &ll, &cl);
    if (r == 0) { for (int i = 0; i < me->n_params; i++) { me->w[i] = keep->w[i]; me->mu[i] = keep->mu[i]; me->vu[i] = keep->vu[i]; }
                  me->adam_t = keep->adam_t; r = 1; }
#else
    int r = st_round_adopt(me, &ap->m, fx + fx_end, fxn - fx_end, SEQ, &ll, &cl);
#endif
    if (diag) printf("[b5a-diag] gen %u apprentice live %.4f cand %.4f d %+.4f %s\n", (unsigned)gen,
                     (double)ll, (double)cl, (double)(cl - ll), r == 1 ? "took" : "dropped");
    if (r == 1) apprentice_late++;
    st_free(&ap->m); ap->has = 0;
}

/* one node's merge with one peer blob; gated: through a copy and the gate;
 * ap != NULL: a refused copy becomes the apprentice */
static int merge_one(st_model *me, const void *pb, size_t pl, uint32_t gen, int gated, apprentice_t *ap)
{
    const void *bs[1] = { pb }; size_t ls[1] = { pl }; uint32_t gs[1] = { gen };
    if (!gated) return st_round_merge(me, gen, bs, ls, gs, 1) > 0;
    apprentice_judge(me, ap, gen);
    st_model c; clone(&c, me);
    int r = 0;
    if (st_round_merge(&c, gen, bs, ls, gs, 1) > 0)
    {
        float ll = 0, cl = 0;
        r = st_round_adopt(me, &c, fx + fx_end, fxn - fx_end, SEQ, &ll, &cl) == 1;
        if (diag) printf("[b5a-diag] gen %u live %.4f cand %.4f d %+.4f %s\n", (unsigned)gen, (double)ll, (double)cl, (double)(cl - ll), r ? "took" : "refused");
        if (!r && ap && st_model_finite(&c)) { ap->m = c; ap->has = 1; return 0; }
    }
    st_free(&c);
    return r;
}
static void rounds(uint32_t seed, int gated, int poison, const char *name)
{
    st_model a, b;
    st_init_tier(&a, seed, ST_TIER_S); st_init_tier(&b, seed, ST_TIER_S);
    int took = 0, tries = 0; uint32_t gen = 0;
    apprentice_t apa = { .has = 0 }, apb = { .has = 0 };
    apprentice_t *pa = gated == 2 ? &apa : NULL, *pbp = gated == 2 ? &apb : NULL;
    apprentice_late = 0;
    for (int p = 1; p <= PASSES; p++) {
        pass(&a, 0);
        if (apa.has) pass(&apa.m, 0);
        if (poison != 1) { pass(&b, 1); if (apb.has) pass(&apb.m, 1); }
        if (p % EVERY) continue;
        gen++;
        if (poison == 1) { st_free(&b); garbage(&b, seed + (uint32_t)p); }
        size_t la, lb; void *ba = blob(&a, &la), *bb;
        if (poison == 2) {   /* diag: a mild poison, B's real weights scaled by mild */
            st_model sc; clone(&sc, &b);
            for (int i = 0; i < sc.n_params; i++) sc.w[i] *= mild;
            bb = blob(&sc, &lb); st_free(&sc);
        } else bb = blob(&b, &lb);
        tries++;
        took += merge_one(&a, bb, lb, gen, gated, pa);
        if (!poison) merge_one(&b, ba, la, gen, gated, pbp);
        free(ba); free(bb);
    }
    apprentice_judge(&a, pa, gen + 1);   /* the last apprentice still gets its turn */
    int late_a = apprentice_late;
    printf("[b5a] arm=%s bpb=%.4f took=%d/%d", name, bpb(&a), took, tries);
    if (gated == 2) printf(" apprentices=%d", late_a);
    printf("\n");
    if (apb.has) st_free(&apb.m);
    st_free(&a); st_free(&b);
}

/* B5 step 3, the reading of research/b5-step3.md §4 (diag "sp", exploration
 * seeds only): what in a garbage peer's merge helps one node? One node, its
 * half, the apprentice flow of the arms above; at each merge the candidate is
 * live transformed, with no peer:
 *   0 reset    w unchanged, Adam restarted (st_merge_cohort restarts Adam)
 *   1 shrink   w * 0.5, Adam restarted
 *   2 perturb  w + 0.5 * garbage.w, Adam restarted
 *   3 garbage  st_round_merge with the garbage peer (= poison-apprentice)
 * and the alone curve: bpb after every pass (does one node overfit?). */
static void sp_cand(st_model *c, st_model *live, int mode, uint32_t seed, uint32_t gen, int p)
{
    clone(c, live);
    st_model g; garbage(&g, seed + (uint32_t)p);
    if (mode == 3) {
        size_t lb; void *bb = blob(&g, &lb);
        const void *bs[1] = { bb }; size_t ls[1] = { lb }; uint32_t gs[1] = { gen };
        (void)st_round_merge(c, gen, bs, ls, gs, 1);
        free(bb);
    } else {
        for (int i = 0; i < c->n_params; i++) {
            if (mode == 1) c->w[i] *= 0.5f;
            if (mode == 2) c->w[i] += 0.5f * g.w[i];
            c->mu[i] = 0.0f; c->vu[i] = 0.0f;
        }
        c->adam_t = 0;
    }
    st_free(&g);
}
static void sp_rounds(uint32_t seed, int mode, const char *name)
{
    st_model a; st_init_tier(&a, seed, ST_TIER_S);
    apprentice_t ap = { .has = 0 };
    int took = 0; uint32_t gen = 0;
    apprentice_late = 0;
    for (int p = 1; p <= PASSES; p++) {
        pass(&a, 0);
        if (ap.has) pass(&ap.m, 0);
        if (p % EVERY) continue;
        gen++;
        apprentice_judge(&a, &ap, gen);
        st_model c; sp_cand(&c, &a, mode, seed, gen, p);
        float ll = 0, cl = 0;
        int r = st_round_adopt(&a, &c, fx + fx_end, fxn - fx_end, SEQ, &ll, &cl) == 1;
        took += r;
        if (!r && st_model_finite(&c)) { ap.m = c; ap.has = 1; } else st_free(&c);
    }
    apprentice_judge(&a, &ap, gen + 1);
    printf("[b5a] arm=sp-%s bpb=%.4f took=%d/%d apprentices=%d\n", name, bpb(&a), took, (int)gen, apprentice_late);
    st_free(&a);
}
static void sp_curve(uint32_t seed)
{
    st_model m; st_init_tier(&m, seed, ST_TIER_S);
    double best = 1e9; int bp = 0;
    printf("[b5a] curve alone-A:");
    for (int p = 1; p <= PASSES; p++) {
        pass(&m, 0);
        double b = bpb(&m);
        if (b < best) { best = b; bp = p; }
        printf(" %.3f", b);
    }
    printf("\n[b5a] arm=sp-curve best=%.4f at pass %d last=%.4f\n", best, bp, bpb(&m));
    st_free(&m);
}

/* B5 step 4 (research/b5-step4-design.md), stage "s4": the product's rate
 * (ST_LR), and every arm stops where node A's loss on the gate window (the
 * fixture's held side, the gate's own windows) was lowest; that pass's model
 * is the arm's answer ("kept"). The ruler (b1_held) is measured only when a
 * pass is kept and every 4th pass (a peek, for reading; never to choose).
 * Ported from the exploration probe pk-scratch/a129/s4probe.c. Judge:
 * tests/llm/b5_step4_check.py. -DB5S4_PEER_SAME: the honest peer trains on
 * A's own half shifted by SEQ/2 (no new bytes), the NC of K1 and the pair of K4. */
static double s4_gate(st_model *m)
{
    double s = 0; int got = 0;
    for (int o = fx_end; o + SEQ <= fxn; o += SEQ) {
        int np = 0; float l = st_eval_loss(m, fx + o, SEQ, &np);
        if (np) { s += l; got++; }
    }
    return got ? s / got : 1e9;
}
static void pass_peer(st_model *m)
{
#if defined(B5S4_PEER_SAME)   /* A's own half, windows shifted by SEQ/2 */
    for (int o = SEQ / 2; o + SEQ <= D / 2; o += SEQ) ST_TRAIN_STEP(m, fx + o, SEQ, lg, lr);
#else
    pass(m, 1);
#endif
}
typedef struct { double best_g, best_b, peek_b; int best_p, peek_p, has; st_model m; } keeper_t;
static void keep_init(keeper_t *k) { memset(k, 0, sizeof *k); k->best_g = 1e9; k->peek_b = 1e9; }
static void keep_see(keeper_t *k, st_model *m, int p)
{
    double g = s4_gate(m), b = 0;
    int need = g < k->best_g || p % 4 == 0;
    if (need) b = bpb(m);
    if (need && b < k->peek_b) { k->peek_b = b; k->peek_p = p; }
    if (g < k->best_g) {
        k->best_g = g; k->best_p = p; k->best_b = b;
        if (k->has) st_free(&k->m);
        clone(&k->m, m); k->has = 1;
    }
}
static void keep_print(keeper_t *k, const char *name, st_model *last, const char *extra)
{
    printf("[b5s4] arm=%s last=%.4f kept=%.4f at %d (gate %.4f) peek=%.4f at %d lastgate=%.4f %s\n",
           name, bpb(last), k->best_b, k->best_p, k->best_g, k->peek_b, k->peek_p, s4_gate(last), extra);
    if (k->has) st_free(&k->m);
}
static void s4_alone(uint32_t seed, int half, int passes, const char *name)
{
    st_model m; st_init_tier(&m, seed, ST_TIER_S);
    keeper_t k; keep_init(&k);
    printf("[b5s4] curve %s gate:", name);
    for (int p = 1; p <= passes; p++) { pass(&m, half); keep_see(&k, &m, p); printf(" %.3f", s4_gate(&m)); }
    printf("\n");
    keep_print(&k, name, &m, "");
    st_free(&m);
}
/* one node, no peer: at each merge point the candidate is live x0.5 with Adam
 * restarted, through the same gate and apprentice flow */
static void s4_shrink(uint32_t seed, const char *name)
{
    st_model a; st_init_tier(&a, seed, ST_TIER_S);
    apprentice_t ap = { .has = 0 };
    int took = 0; uint32_t gen = 0;
    keeper_t k; keep_init(&k);
    apprentice_late = 0;
    for (int p = 1; p <= PASSES; p++) {
        pass(&a, 0);
        if (ap.has) pass(&ap.m, 0);
        if (p % EVERY == 0) {
            gen++;
            apprentice_judge(&a, &ap, gen);
            st_model c; clone(&c, &a);
            for (int i = 0; i < c.n_params; i++) { c.w[i] *= 0.5f; c.mu[i] = 0.0f; c.vu[i] = 0.0f; }
            c.adam_t = 0;
            int r = st_round_adopt(&a, &c, fx + fx_end, fxn - fx_end, SEQ, NULL, NULL) == 1;
            took += r;
            if (!r && st_model_finite(&c)) { ap.m = c; ap.has = 1; } else st_free(&c);
        }
        keep_see(&k, &a, p);
    }
    apprentice_judge(&a, &ap, gen + 1);
    char ex[64]; snprintf(ex, sizeof ex, "took=%d/%d app=%d", took, (int)gen, apprentice_late);
    keep_print(&k, name, &a, ex);
    st_free(&a);
}
/* two nodes, gate + apprentice; poison: B is garbage (another init x8) */
static void s4_rounds(uint32_t seed, int poison, const char *name)
{
    st_model a, b;
    st_init_tier(&a, seed, ST_TIER_S); st_init_tier(&b, seed, ST_TIER_S);
    int took = 0, tries = 0; uint32_t gen = 0;
    apprentice_t apa = { .has = 0 }, apb = { .has = 0 };
    keeper_t k; keep_init(&k);
    apprentice_late = 0;
    for (int p = 1; p <= PASSES; p++) {
        pass(&a, 0);
        if (apa.has) pass(&apa.m, 0);
        if (!poison) { pass_peer(&b); if (apb.has) pass_peer(&apb.m); }
        if (p % EVERY == 0) {
            gen++;
            if (poison) { st_free(&b); garbage(&b, seed + (uint32_t)p); }
            size_t la, lb; void *ba = blob(&a, &la), *bb = blob(&b, &lb);
            tries++;
            took += merge_one(&a, bb, lb, gen, 2, &apa);
            if (!poison) merge_one(&b, ba, la, gen, 2, &apb);
            free(ba); free(bb);
        }
        keep_see(&k, &a, p);
    }
    char ex[64]; snprintf(ex, sizeof ex, "took=%d/%d app=%d", took, tries, apprentice_late);
    keep_print(&k, name, &a, ex);
    if (apa.has) st_free(&apa.m);
    if (apb.has) st_free(&apb.m);
    st_free(&a); st_free(&b);
}
static int s4(uint32_t seed)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    lr = ST_LR;
#if defined(B5S4_PEER_SAME)
    const char *peer = "same";
#else
    const char *peer = "plain";
#endif
    printf("[b5s4] seed %u lr %g peer %s gate windows %d held windows %d\n",
           (unsigned)seed, (double)lr, peer, (fxn - fx_end) / SEQ, hn / SEQ);
    s4_alone(seed, 0, PASSES, "alone-A");
    s4_alone(seed, 1, PASSES, "alone-B");
    s4_alone(seed, -1, PASSES / 2, "full");
    s4_shrink(seed, "shrink-app");
    s4_rounds(seed, 0, "apprentice");
    s4_rounds(seed, 1, "poison-app");
    printf("[b5s4] done\n");
    return 0;
}

static int unit(void)
{
    st_model live, cand, other;
    st_init_tier(&live, 7u, ST_TIER_S);
    for (int p = 0; p < 4; p++) pass(&live, 0);
    const uint8_t *win = fx + fx_end; int wn = fxn - fx_end;
    float ll = 0, cl = 0;
    /* a worse copy (garbage) is refused, live all bytes the same */
    garbage(&cand, 7u);
    uint64_t h0 = hash(&live);
    int r = st_round_adopt(&live, &cand, win, wn, SEQ, &ll, &cl);
    printf("    garbage: rc %d live %.4f cand %.4f\n", r, (double)ll, (double)cl);
    check(r == 0 && hash(&live) == h0 && cl > ll, "b5a-worse", "a worse copy is refused, live byte-unchanged");
    st_free(&cand);
    /* a better copy is taken, live becomes it exactly */
    clone(&cand, &live);
    for (int p = 0; p < 4; p++) pass(&cand, 0);
    uint64_t hc = hash(&cand);
    r = st_round_adopt(&live, &cand, win, wn, SEQ, &ll, &cl);
    printf("    better: rc %d live %.4f cand %.4f\n", r, (double)ll, (double)cl);
    check(r == 1 && hash(&live) == hc && cl < ll, "b5a-better", "a better copy is taken, live equals it byte for byte");
    /* the same model again: not lower, kept */
    h0 = hash(&live);
    r = st_round_adopt(&live, &cand, win, wn, SEQ, NULL, NULL);
    check(r == 0 && hash(&live) == h0, "b5a-equal", "an equal copy is not taken (strictly lower only)");
    /* a NaN in an unused row of a better copy: refused */
    for (int p = 0; p < 2; p++) pass(&cand, 0);
    cand.w[cand.n_params - 1] = 0.0f / 0.0f;
    h0 = hash(&live);
    r = st_round_adopt(&live, &cand, win, wn, SEQ, NULL, &cl);
    check(r == 0 && hash(&live) == h0, "b5a-nan", "a copy with a NaN is refused, live byte-unchanged");
    st_free(&cand);
    /* a NaN at w[0] of a better copy: the windows never read it (its loss
     * stays finite and lower), so only the finite check refuses it. The last
     * element above gives the floor loss and is refused by the comparison too
     * (audit-119: a gate without the finite check stayed green there).
     * The pair is [b5a-better]'s (4 passes, then 4 more on a copy), so the
     * copy is better by the comparison alone; the NaN is what must refuse. */
    {
        st_model l2;
        st_init_tier(&l2, 7u, ST_TIER_S);
        for (int p = 0; p < 4; p++) pass(&l2, 0);
        clone(&cand, &l2);
        for (int p = 0; p < 4; p++) pass(&cand, 0);
        cand.w[0] = 0.0f / 0.0f;
        h0 = hash(&l2);
        r = st_round_adopt(&l2, &cand, win, wn, SEQ, &ll, &cl);
        printf("    nan at w[0]: rc %d live %.4f cand %f\n", r, (double)ll, (double)cl);
        check(r == 0 && hash(&l2) == h0, "b5a-nan0",
              "a better copy with a NaN the windows never read (w[0]) is refused, live byte-unchanged");
        st_free(&cand); st_free(&l2);
    }
    /* another tier, no window, NULL: ST_E_ARG and unchanged */
    st_init_tier(&other, 7u, ST_TIER_M);
    h0 = hash(&live);
    int e1 = st_round_adopt(&live, &other, win, wn, SEQ, NULL, NULL);
    int e2 = st_round_adopt(&live, &live, win, SEQ - 1, SEQ, NULL, NULL);
    int e3 = st_round_adopt(&live, NULL, win, wn, SEQ, NULL, NULL);
    check(e1 < 0 && e2 < 0 && e3 < 0 && hash(&live) == h0, "b5a-arg", "another tier, no window, NULL: refused, unchanged");
    st_free(&other); st_free(&live);
    printf("[b5a] unit %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    fx = (const uint8_t *)TEACHER_FIXTURE; fxn = (int)sizeof(TEACHER_FIXTURE) - 1;
    fx_end = st_held_split(fxn, SEQ).train_end;
    lg = malloc((size_t)SEQ * ST_VOCAB * sizeof(float));
    if (!lg || fx_end < D) { printf("  FAIL  [b5a-io] setup (fx_end %d)\n", fx_end); return 1; }
    if (argc > 1 && strcmp(argv[1], "unit") == 0) return unit();
    if (argc < 3) { fprintf(stderr, "usage: %s unit | <b1_held.txt> <seed>\n", argv[0]); return 2; }
    uint32_t seed = (uint32_t)strtoul(argv[2], NULL, 10);
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    hn = (int)fread(held, 1, sizeof held, f); fclose(f);
    if (argc > 3 && strcmp(argv[3], "s4") == 0) return s4(seed);
    printf("[b5a] seed %u passes %d every %d held windows %d (fixture held %d B)\n",
           (unsigned)seed, PASSES, EVERY, hn / SEQ, fxn - fx_end);
    diag = argc > 3 && strcmp(argv[3], "diag") == 0;
    if (argc > 4) mild = strtof(argv[4], NULL);
    if (diag && argc > 4) { printf("[b5a] mild %.2f\n", (double)mild); rounds(seed, 0, 2, "mild-always"); rounds(seed, 1, 2, "mild-adopt"); rounds(seed, 2, 2, "mild-apprentice"); rounds(seed, 2, 1, "poison-apprentice"); return 0; }
    if (argc > 3 && strcmp(argv[3], "sp") == 0) {
        sp_curve(seed);
        sp_rounds(seed, 0, "reset"); sp_rounds(seed, 1, "shrink");
        sp_rounds(seed, 2, "perturb"); sp_rounds(seed, 3, "garbage");
        return 0;
    }
    if (diag) { rounds(seed, 0, 0, "always"); rounds(seed, 1, 0, "adopt"); rounds(seed, 2, 0, "apprentice"); return 0; }
    for (int h = 0; h < 2; h++) {
        st_model m; st_init_tier(&m, seed, ST_TIER_S);
        for (int p = 1; p <= PASSES; p++) pass(&m, h);
        printf("[b5a] arm=alone-%c bpb=%.4f\n", "AB"[h], bpb(&m));
        st_free(&m);
    }
    {
        st_model m; st_init_tier(&m, seed, ST_TIER_S);
        for (int p = 1; p <= PASSES / 2; p++) pass(&m, -1);
        printf("[b5a] arm=full bpb=%.4f\n", bpb(&m));
        st_free(&m);
    }
    rounds(seed, 0, 0, "always");
    rounds(seed, 1, 0, "adopt");
    rounds(seed, 0, 1, "poison-always");
    rounds(seed, 1, 1, "poison-adopt");
    printf("[b5a] done\n");
    return 0;
}
