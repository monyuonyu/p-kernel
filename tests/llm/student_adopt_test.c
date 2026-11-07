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
static const char *kp_tag = "b5s4";   /* s5 prints [b5s5] */
static void keep_print(keeper_t *k, const char *name, st_model *last, const char *extra)
{
    printf("[%s] arm=%s last=%.4f kept=%.4f at %d (gate %.4f) peek=%.4f at %d lastgate=%.4f %s\n",
           kp_tag, name, bpb(last), k->best_b, k->best_p, k->best_g, k->peek_b, k->peek_p, s4_gate(last), extra);
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

/* B5 step 5 (research/b5-step5-design.md), stage "s5": four nodes, each a
 * quarter of the first D bytes, step 4's form (ST_LR, A's best gate pass,
 * gate + apprentice). The candidate of each honest node is made from all four
 * current models: mean (st_round_merge, the three peers) or trim
 * (st_round_trimmed, t = 1). Poison: the last 1 or 2 nodes send garbage
 * (another init x8) every round. Ported from pk-scratch/a149/s5probe2.c.
 * -DB5S5_NO_GATE (NC2): every candidate replaces live. Judge b5_step5_check.py. */
#define K5 4
static void pass_q(st_model *m, int q)
{
    int a = q * (D / K5), b = a + D / K5;
    for (int o = a; o + SEQ <= b; o += SEQ) ST_TRAIN_STEP(m, fx + o, SEQ, lg, lr);
}
static void s5_cand(st_model *c, st_model *models[K5], int me, int trim, uint32_t gen)
{
    clone(c, models[me]);
    const void *bs[K5 - 1]; size_t ls[K5 - 1]; uint32_t gs[K5 - 1]; void *own[K5 - 1]; int j = 0;
    for (int k = 0; k < K5; k++) if (k != me) { own[j] = blob(models[k], &ls[j]); bs[j] = own[j]; gs[j] = gen; j++; }
    int r = trim ? st_round_trimmed(c, gen, bs, ls, gs, K5 - 1, 1) : st_round_merge(c, gen, bs, ls, gs, K5 - 1);
    if (r != K5 - 1) { printf("  FAIL  [b5s5-io] merge took %d of %d\n", r, K5 - 1); exit(1); }
    for (int i = 0; i < K5 - 1; i++) free(own[i]);
}
static void s5_rounds(uint32_t seed, int trim, int poison, const char *name)
{
    st_model n[K5]; apprentice_t ap[K5]; st_model *mp[K5];
    for (int k = 0; k < K5; k++) { st_init_tier(&n[k], seed, ST_TIER_S); ap[k].has = 0; mp[k] = &n[k]; }
    keeper_t kp; keep_init(&kp);
    int took = 0, tries = 0; uint32_t gen = 0;
    apprentice_late = 0;
    int honest = K5 - poison;
    for (int p = 1; p <= PASSES; p++) {
        for (int k = 0; k < honest; k++) { pass_q(&n[k], k); if (ap[k].has) pass_q(&ap[k].m, k); }
        if (p % EVERY == 0) {
            gen++;
            for (int k = honest; k < K5; k++) { st_free(&n[k]); garbage(&n[k], seed + (uint32_t)p + 977u * (uint32_t)k); }
            st_model c[K5];
            for (int k = 0; k < honest; k++) s5_cand(&c[k], mp, k, trim, gen);   /* all from the same round */
            for (int k = 0; k < honest; k++) {
#if defined(B5S5_NO_GATE)
                st_free(&n[k]); n[k] = c[k];
                if (k == 0) { took++; tries++; }
#else
                apprentice_judge(&n[k], &ap[k], gen);
                int r = st_round_adopt(&n[k], &c[k], fx + fx_end, fxn - fx_end, SEQ, NULL, NULL) == 1;
                if (k == 0) { took += r; tries++; }
                if (!r && st_model_finite(&c[k])) { ap[k].m = c[k]; ap[k].has = 1; } else st_free(&c[k]);
#endif
            }
        }
        keep_see(&kp, &n[0], p);
    }
    char ex[64]; snprintf(ex, sizeof ex, "took=%d/%d app=%d", took, tries, apprentice_late);
    keep_print(&kp, name, &n[0], ex);
    for (int k = 0; k < K5; k++) { st_free(&n[k]); if (ap[k].has) st_free(&ap[k].m); }
}
static void s5_alone_q(uint32_t seed, const char *name)
{
    st_model m; st_init_tier(&m, seed, ST_TIER_S);
    keeper_t k; keep_init(&k);
    for (int p = 1; p <= PASSES; p++) { pass_q(&m, 0); keep_see(&k, &m, p); }
    keep_print(&k, name, &m, "");
    st_free(&m);
}
/* one node on its quarter, the candidate live x0.5 through the same gate */
static void s5_shrink_q(uint32_t seed, const char *name)
{
    st_model a; st_init_tier(&a, seed, ST_TIER_S);
    apprentice_t ap = { .has = 0 };
    int took = 0; uint32_t gen = 0;
    keeper_t k; keep_init(&k);
    apprentice_late = 0;
    for (int p = 1; p <= PASSES; p++) {
        pass_q(&a, 0);
        if (ap.has) pass_q(&ap.m, 0);
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
/* which: "s5" every arm; "s5t1" the two arms of T1 (NC1); "s5t4" those of T4 (NC2) */
static int s5(uint32_t seed, const char *which)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    lr = ST_LR;
    kp_tag = "b5s5";
#if defined(B5S5_TRIM_OFF)
    const char *bld = "trimoff";
#elif defined(B5S5_NO_GATE)
    const char *bld = "nogate";
#else
    const char *bld = "plain";
#endif
    int all = strcmp(which, "s5") == 0, t1 = all || strcmp(which, "s5t1") == 0, t4 = all || strcmp(which, "s5t4") == 0;
    printf("[b5s5] seed %u lr %g build %s stage %s quarter %d B gate windows %d held windows %d\n",
           (unsigned)seed, (double)lr, bld, which, D / K5, (fxn - fx_end) / SEQ, hn / SEQ);
    if (all || t4) s5_alone_q(seed, "alone-A");
    if (all) {
        s4_alone(seed, -1, PASSES / K5, "full");
        s5_shrink_q(seed, "shrink-app");
        s5_rounds(seed, 0, 0, "mean4");
        s5_rounds(seed, 1, 0, "trim4");
    }
    if (t1) { s5_rounds(seed, 0, 1, "mean4-poison"); s5_rounds(seed, 1, 1, "trim4-poison"); }
    if (t4) s5_rounds(seed, 1, 2, "trim4-poison2");
    printf("[b5s5] done\n");
    return 0;
}

/* ---- the self-test of st_merge_trimmed (audit-150's note on the design) ---- */
static int cmp_total(const void *x, const void *y)   /* value, then -0 before +0 */
{
    float a = *(const float *)x, b = *(const float *)y;
    if (a < b) return -1;
    if (a > b) return 1;
    uint32_t ua, ub; memcpy(&ua, &a, 4); memcpy(&ub, &b, 4);
    return (int)(ub >> 31) - (int)(ua >> 31);
}
/* the reference, written apart from student.c: qsort, the middle in ascending order */
static float ref_trim(const float *in, int n, int t)
{
    float v[ST_COHORT_MAX + 1];
    memcpy(v, in, (size_t)n * sizeof v[0]);
    qsort(v, (size_t)n, sizeof v[0], cmp_total);
    float s = v[t];
    for (int i = t + 1; i < n - t; i++) s += v[i];
    return s / (float)(n - 2 * t);
}
static int same_f(float a, float b) { return memcmp(&a, &b, 4) == 0; }
/* merge into a clone of models[me] the blobs of the others listed in order[] */
static int s5u_merge(st_model *out, st_model *models, int me, const int *order, int no, int t)
{
    clone(out, &models[me]);
    const void *bs[8]; size_t ls[8]; void *own[8];
    for (int j = 0; j < no; j++) { own[j] = blob(&models[order[j]], &ls[j]); bs[j] = own[j]; }
    int r = st_merge_trimmed(out, bs, ls, no, t);
    for (int j = 0; j < no; j++) free(own[j]);
    return r;
}
static int s5unit(void)
{
    enum { NM = 6 };
    st_model m[NM];
    for (int k = 0; k < NM; k++) st_init_tier(&m[k], 11u + (uint32_t)k, ST_TIER_S);
    /* hand values, members A B C D (index 0..3); E F only in the 5 and 6 cases */
    static const float hv[][4] = {
        {    1.0f,  2.0f,  3.0f, 100.0f },  /* outlier above            -> 2.5  */
        { -100.0f,  1.0f,  2.0f,   3.0f },  /* outlier below            -> 1.5  */
        {    5.0f,  5.0f,  5.0f,   5.0f },  /* all the same             -> 5    */
        { 1000.0f,  1.0f,  2.0f,   3.0f },  /* into is the outlier      -> 2.5  */
        {   -0.0f, -0.0f,  0.0f,   5.0f },  /* signed zero tie          -> +0   */
        {   -7.0f, -1.0f, -3.0f,   9.0f },  /* negatives                -> -2   */
        {  -50.0f, 0.25f, 0.75f,  60.0f },  /* both ends                -> 0.5  */
    };
    static const float want4[] = { 2.5f, 1.5f, 5.0f, 2.5f, 0.0f, -2.0f, 0.5f };
    enum { NH = sizeof want4 / sizeof want4[0] };
    for (int c = 0; c < NH; c++) for (int k = 0; k < 4; k++) m[k].w[c] = hv[c][k];
    m[4].w[0] = 4.0f;    /* 5 members, c0: 1 2 3 100 4  -> (2+3+4)/3 = 3 */
    m[5].w[0] = -9.0f;   /* 6 members t=2, c0 with E 4: 1 2 3 100 4 -9 -> (2+3)/2 = 2.5 */
    for (int i = 0; i < m[0].n_params; i++) { m[0].mu[i] = 0.25f; m[0].vu[i] = 0.5f; }
    m[0].adam_t = 9;     /* so [s5-adam] sees a restart, not a fresh init */
    int abc[] = { 1, 2, 3 };
    st_model r4; int rc = s5u_merge(&r4, m, 0, abc, 3, 1);
    int hand = rc == 3;
    for (int c = 0; c < NH; c++) {
        int ok = same_f(r4.w[c], want4[c]);
        printf("    hand c%d: got %g want %g %s\n", c, (double)r4.w[c], (double)want4[c], ok ? "ok" : "WRONG");
        hand &= ok;
    }
    check(hand, "s5-hand", "4 members t=1: hand values with outliers above, below, into, ties, signed zero");
    /* every weight against the reference */
    int refok = 1;
    for (int i = 0; i < r4.n_params; i++) {
        float v[4] = { m[0].w[i], m[1].w[i], m[2].w[i], m[3].w[i] };
        if (!same_f(r4.w[i], ref_trim(v, 4, 1))) { refok = 0; printf("    ref: w[%d] %g vs %g\n", i, (double)r4.w[i], (double)ref_trim(v, 4, 1)); break; }
    }
    check(refok, "s5-ref", "4 members t=1: every weight equals the qsort reference to the byte");
    int z = 1; for (int i = 0; i < r4.n_params; i++) z &= r4.mu[i] == 0.0f && r4.vu[i] == 0.0f;
    check(z && r4.adam_t == 0, "s5-adam", "a merge restarts Adam (mu vu 0, adam_t 0)");
    /* order: the same four in other orders and from another member, all bytes */
    int o1[] = { 3, 1, 2 }, o2[] = { 3, 0, 1 }, o3[] = { 1, 2, 0 };
    st_model q1, q2, q3;
    s5u_merge(&q1, m, 0, o1, 3, 1); s5u_merge(&q2, m, 2, o2, 3, 1); s5u_merge(&q3, m, 3, o3, 3, 1);
    uint64_t h = hash(&r4);
    printf("    order: %016llx %016llx %016llx %016llx\n", (unsigned long long)h, (unsigned long long)hash(&q1),
           (unsigned long long)hash(&q2), (unsigned long long)hash(&q3));
    check(hash(&q1) == h && hash(&q2) == h && hash(&q3) == h, "s5-order",
          "merge(A,{B,C,D}) = merge(A,{D,B,C}) = merge(C,{D,A,B}) = merge(D,{B,C,A}), every byte");
    st_free(&q1); st_free(&q2); st_free(&q3);
    /* 5 members t=1, 6 members t=2 */
    int ab4[] = { 1, 2, 3, 4 }, ab5[] = { 1, 2, 3, 4, 5 };
    st_model r5, r6;
    int rc5 = s5u_merge(&r5, m, 0, ab4, 4, 1), rc6 = s5u_merge(&r6, m, 0, ab5, 5, 2);
    int r56 = rc5 == 4 && rc6 == 5 && same_f(r5.w[0], 3.0f) && same_f(r6.w[0], 2.5f);
    for (int i = 0; r56 && i < r5.n_params; i++) {
        float v[6]; for (int k = 0; k < 6; k++) v[k] = m[k].w[i];
        r56 = same_f(r5.w[i], ref_trim(v, 5, 1)) && same_f(r6.w[i], ref_trim(v, 6, 2));
    }
    printf("    5 members c0 %g (want 3), 6 members t=2 c0 %g (want 2.5)\n", (double)r5.w[0], (double)r6.w[0]);
    check(r56, "s5-hand56", "5 members t=1 and 6 members t=2: hand c0 and every weight equals the reference");
    st_free(&r5); st_free(&r6);
    /* refusals: counted after the tier and finite checks; the model unchanged */
    st_model other; st_init_tier(&other, 99u, ST_TIER_M);
    st_model nanp; clone(&nanp, &m[3]); nanp.w[5] = 0.0f / 0.0f;
    size_t lo, ln, l1, l2; void *bo = blob(&other, &lo), *bn = blob(&nanp, &ln), *b1 = blob(&m[1], &l1), *b2 = blob(&m[2], &l2);
    /* each case on a fresh clone of A, so one red does not colour the next */
    st_model a; uint64_t ha = hash(&m[0]);
#define FRESH() do { st_free(&a); clone(&a, &m[0]); } while (0)
    clone(&a, &m[0]);
    const void *s1[] = { b1, b2, bo }; size_t z1[] = { l1, l2, lo };
    int f1 = st_merge_trimmed(&a, s1, z1, 3, 1); int u1 = hash(&a) == ha; FRESH();
    const void *s2[] = { b1, bn, b2 }; size_t z2[] = { l1, ln, l2 };
    int f2 = st_merge_trimmed(&a, s2, z2, 3, 1); int u2 = hash(&a) == ha; FRESH();
    int f3 = st_merge_trimmed(&a, s1, z1, 2, 1); int u3 = hash(&a) == ha; FRESH();
    printf("    few: other tier rc %d, NaN peer rc %d, two peers rc %d\n", f1, f2, f3);
    check(f1 == 0 && f2 == 0 && f3 == 0 && u1 && u2 && u3, "s5-few",
          "3 members after the tier / finite checks (or 2 offered) is refused: 0, every byte unchanged (no mean)");
    /* the middle sums to Inf: refused */
    st_model big[3];
    for (int k = 0; k < 3; k++) clone(&big[k], &m[1 + k]);
    big[0].w[7] = 3.0e38f; big[1].w[7] = 3.0e38f; big[2].w[7] = 3.1e38f;   /* with A's w[7]: middle 3e38+3e38 */
    size_t lb[3]; void *bb[3]; for (int k = 0; k < 3; k++) bb[k] = blob(&big[k], &lb[k]);
    const void *s3[] = { bb[0], bb[1], bb[2] };
    int f4 = st_merge_trimmed(&a, s3, lb, 3, 1); int u4 = hash(&a) == ha; FRESH();
    check(f4 == 0 && u4, "s5-inf", "a middle that sums to Inf is refused, every byte unchanged");
    /* generations: one peer of another round leaves 3 members */
    const void *s4b[] = { b1, b2, bb[2] }; size_t z4[] = { l1, l2, lb[2] };
    uint32_t g1[] = { 5, 5, 4 }, g2[] = { 5, 5, 5 };
    int f5 = st_round_trimmed(&a, 5, s4b, z4, g1, 3, 1); int u5 = hash(&a) == ha; FRESH();
    st_model a2; clone(&a2, &m[0]);
    int f6 = st_round_trimmed(&a2, 5, s4b, z4, g2, 3, 1);
    check(f5 == 0 && u5 && f6 == 3, "s5-gen", "a peer of another generation is not a member (refused), all of one generation merge");
    int e1 = st_merge_trimmed(&a, s1, z1, 2, 0), e2 = st_merge_trimmed(&a, s1, z1, ST_COHORT_MAX + 1, 1),
        e3 = st_merge_trimmed(NULL, s1, z1, 2, 1);
    check(e1 < 0 && e2 < 0 && e3 < 0 && hash(&a) == ha, "s5-arg", "t 0, count over the max, NULL: ST_E_ARG, unchanged");
#undef FRESH
    free(bo); free(bn); free(b1); free(b2); for (int k = 0; k < 3; k++) { free(bb[k]); st_free(&big[k]); }
    st_free(&a); st_free(&a2); st_free(&other); st_free(&nanp); st_free(&r4);
    for (int k = 0; k < NM; k++) st_free(&m[k]);
    printf("[b5s5] unit %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
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
    if (argc > 1 && strcmp(argv[1], "s5unit") == 0) return s5unit();
    if (argc < 3) { fprintf(stderr, "usage: %s unit | <b1_held.txt> <seed>\n", argv[0]); return 2; }
    uint32_t seed = (uint32_t)strtoul(argv[2], NULL, 10);
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    hn = (int)fread(held, 1, sizeof held, f); fclose(f);
    if (argc > 3 && strcmp(argv[3], "s4") == 0) return s4(seed);
    if (argc > 3 && strncmp(argv[3], "s5", 2) == 0) return s5(seed, argv[3]);
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
