/*
 * student_region_test.c - [b5-wire] B5 wire, stage 1 (research/b5-wire.md):
 * the resident student merges with its region (st_region_round: fetch the
 * peers, same generation only, st_round_trimmed t = 1 into a copy,
 * st_round_adopt on the node's held windows). Lines fixed in the design
 * (§3, §4) before the code; the judge is tests/llm/b5_wire_check.py.
 *
 *   student_region_test unit                       W0 W3 W4 W5 W6 (the real
 *       pfs and gl_student_publish / _fetch, one process, SOLO; seconds)
 *   student_region_test <b1_held.txt> <seed> [all|w2]   the arms of W1 W2
 *       (four nodes, a quarter each, the blobs from a table in memory: W0
 *       shows the table and the transport give the same bytes)
 *
 * Arms (S, common init, a quarter of the first D bytes each, one pass = a
 * node's quarter once, 32 passes, a round every 4, kept = node 0's model at
 * its lowest gate pass, the ruler b1_held never used to choose):
 *   alone            node 0, its quarter only
 *   region4          four honest nodes, st_region_round            (W1)
 *   region4-garb1    node 3 sends garbage (another init x8) a round (W2)
 *   mean4-garb1      the same, the candidate the plain mean (st_round_merge)
 *                    through the same gate: W2's comparator, made here
 *   region4-garb2    nodes 2 and 3 garbage (a reading, W6 is the unit's)
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "student.h"
#include "held_indep.h"
#include "teacher_fixture.h"

extern int  gl_student_publish(unsigned short node, const void *blob, unsigned int len);
extern int  gl_student_fetch(unsigned short node, void *out, unsigned int cap);
extern void gl_student_test_drop_chunk(int idx);
extern int  pfs_durable_restore(void (*emit)(const char *));
extern int  pfs_durable_active(void);

#define SEQ 32
#define D 3072
#define K 4
#define PASSES 32
#define EVERY 4
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
static void emit(const char *s) { fputs(s, stdout); }
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
static double gate(st_model *m)   /* the gate's own windows (the fixture's held side) */
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
static void *blob(const st_model *m, size_t *len)
{
    size_t cap = st_blob_size(m);
    void *b = malloc(cap);
    long n = b ? st_save(m, b, cap) : -1;
    if (n < 0) { printf("  FAIL  [b5w-io] save\n"); exit(1); }
    *len = (size_t)n;
    return b;
}
static void clone(st_model *dst, const st_model *src)
{
    size_t n; void *b = blob(src, &n);
    if (st_init_tier(dst, 1u, src->tier) != 0 || st_load(dst, b, n) != 0) { printf("  FAIL  [b5w-io] clone\n"); exit(1); }
    free(b);
}
static void garbage(st_model *g, uint32_t seed)
{
    st_init_tier(g, seed ^ 0xBADu, ST_TIER_S);
    for (int i = 0; i < g->n_params; i++) g->w[i] *= 8.0f;
}
static void *wrap(const st_model *m, uint32_t gen, size_t *len)
{
    size_t cap = st_rg_cap(m);
    void *b = malloc(cap);
    long n = b ? st_rg_wrap(m, gen, b, cap) : -1;
    if (n < ST_RG_HDR) { printf("  FAIL  [b5w-io] wrap %ld\n", n); exit(1); }
    *len = (size_t)n;
    return b;
}

/* the blobs of a round in memory, by node id */
#define TAB 64
typedef struct { void *b[TAB]; size_t n[TAB]; } tab_t;
static int tab_fetch(void *ctx, uint16_t node, void *out, size_t cap)
{
    tab_t *t = (tab_t *)ctx;
    if (node >= TAB || !t->b[node] || t->n[node] > cap) return -1;
    memcpy(out, t->b[node], t->n[node]);
    return (int)t->n[node];
}
static void tab_free(tab_t *t) { for (int i = 0; i < TAB; i++) { free(t->b[i]); t->b[i] = NULL; t->n[i] = 0; } }
static int pfs_fetch(void *ctx, uint16_t node, void *out, size_t cap)
{
    (void)ctx;
    return gl_student_fetch(node, out, (unsigned int)cap);
}
static int publish(uint16_t node, const st_model *m, uint32_t gen)
{
    size_t n; void *b = wrap(m, gen, &n);
    int r = gl_student_publish(node, b, (unsigned int)n);
    free(b);
    return r;
}
static int round_on(st_model *live, uint32_t gen, uint16_t self, const uint16_t *mem, int cnt,
                    st_rg_fetch_fn f, void *ctx, st_rg_report *rep)
{
    return st_region_round(live, gen, self, mem, cnt, f, ctx, fx + fx_end, fxn - fx_end, SEQ, rep);
}
static void say(const char *what, int r, const st_rg_report *p)
{
    printf("  %s: r=%d members %d fetched %d same-gen %d accepted %d adopted %d held %.4f -> %.4f\n",
           what, r, p->members, p->fetched, p->same_gen, p->accepted, p->adopted,
           (double)p->held_live, (double)p->held_cand);
}

/* ---- stage 2 (research/b5-wire2.md §3): W7 W8 W9 W10, from tables ---- */

/* W7: the envelope says n, the transport hands back 8 bytes less (the tail of
 * the blob is missing). The cap is the exact blob size, so "8 bytes MORE" can
 * not reach the round; the tooth is the same check (h[2] == got - HDR). */
static int w7_fetch(void *ctx, uint16_t node, void *out, size_t cap)
{
    tab_t *t = (tab_t *)ctx;
    memset(out, 0, cap);
    int r = tab_fetch(ctx, node, out, cap);
    if (r > 0 && node == 3 && t->n[3] > ST_RG_HDR + 8) r -= 8;
    return r;
}
/* W9: which nodes were fetched in one round */
typedef struct { tab_t *t; int calls; unsigned long seen; } w9_t;
static int w9_fetch(void *ctx, uint16_t node, void *out, size_t cap)
{
    w9_t *w = (w9_t *)ctx;
    w->calls++;
    if (node < 64) w->seen |= 1ul << node;
    return tab_fetch(w->t, node, out, cap);
}
/* W10: the hook is in student.c from stage 2; weak here so the test-only
 * commit links and W10 is red by its own line */
extern void st_rg_test_fail_alloc(int nth) __attribute__((weak));

static void w7_w10(st_model *n, tab_t *t, uint32_t G)
{
    st_model a; st_rg_report rp; uint64_t h0;

    /* W7 */
    clone(&a, &n[0]); h0 = hash(&a);
    const uint16_t all4[4] = { 0, 1, 2, 3 };
    int r7 = round_on(&a, G, 0, all4, 4, w7_fetch, t, &rp); say("W7", r7, &rp);
    check(rp.fetched == 3 && rp.same_gen == 3 && rp.accepted == 0 && r7 == 0 && hash(&a) == h0,
          "b5w-W7", "the blob's last 8 bytes missing, the envelope's length right: not counted, refused, live byte-unchanged");
    st_free(&a);

    /* W8 */
    clone(&a, &n[0]); h0 = hash(&a);
    const uint16_t dup[4] = { 0, 1, 2, 2 };
    int r8 = round_on(&a, G, 0, dup, 4, tab_fetch, t, &rp); say("W8", r8, &rp);
    check(rp.members == 3 && rp.same_gen == 3 && rp.accepted == 0 && r8 == 0 && hash(&a) == h0,
          "b5w-W8", "members [self,1,2,2]: two peers (members 3), refused, live byte-unchanged");
    st_free(&a);

    /* W9: nine peers 1..9, two generations */
    {
        tab_t g0, g1; memset(&g0, 0, sizeof g0); memset(&g1, 0, sizeof g1);
        for (int k = 1; k <= 9; k++) {
            g0.b[k] = wrap(&n[1 + (k - 1) % 3], G, &g0.n[k]);
            g1.b[k] = wrap(&n[1 + (k - 1) % 3], G + 1, &g1.n[k]);
        }
        const uint16_t ten[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        w9_t w0 = { &g0, 0, 0 }, w1 = { &g1, 0, 0 };
        st_rg_report p0, p1;
        clone(&a, &n[0]);
        int r0 = round_on(&a, G, 0, ten, 10, w9_fetch, &w0, &p0); say("W9 g", r0, &p0);
        st_free(&a); clone(&a, &n[0]);
        int r1 = round_on(&a, G + 1, 0, ten, 10, w9_fetch, &w1, &p1); say("W9 g+1", r1, &p1);
        st_free(&a);
        printf("  W9: fetched per round %d %d, seen %#lx %#lx\n", w0.calls, w1.calls, w0.seen, w1.seen);
        check(w0.calls <= 7 && w1.calls <= 7 && p0.fetched <= 7 && p1.fetched <= 7,
              "b5w-W9", "nine peers: at most 7 fetched (and held at once) a round");
        check((w0.seen | w1.seen) == 0x3FEul && p0.members == 10 && p1.members == 10,
              "b5w-W9", "generations g and g+1 together fetch all nine (members 10)");
        tab_free(&g0); tab_free(&g1);
    }

    /* W10: the second peer buffer can not be had -> the round is refused */
    {
        tab_t t5; memset(&t5, 0, sizeof t5);
        for (int k = 1; k <= 4; k++) t5.b[k] = wrap(&n[1 + (k - 1) % 3], G, &t5.n[k]);
        const uint16_t five[5] = { 0, 1, 2, 3, 4 };
        clone(&a, &n[0]); h0 = hash(&a);
        if (!st_rg_test_fail_alloc) {
            check(0, "b5w-W10", "st_rg_test_fail_alloc is not there (stage 2 not built)");
        } else {
            st_rg_test_fail_alloc(2);
            int r10 = round_on(&a, G, 0, five, 5, tab_fetch, &t5, &rp); say("W10", r10, &rp);
            check(r10 == 0 && rp.accepted == 0 && rp.adopted == 0 && hash(&a) == h0,
                  "b5w-W10", "the second peer buffer fails: the whole round refused, live byte-unchanged");
            st_free(&a); clone(&a, &n[0]);
            int r10b = round_on(&a, G, 0, five, 5, tab_fetch, &t5, &rp); say("W10 after", r10b, &rp);
            check(rp.fetched == 4 && rp.accepted == 4, "b5w-W10", "the hook is one-shot: the next round takes the four");
        }
        st_free(&a);
        tab_free(&t5);
    }
}

/* W0 W3 W4 W5 W6 with the real pfs (ARK-backed, SOLO) */
static int unit(void)
{
    printf("=== [b5-wire] unit (research/b5-wire.md §3 §4) ===\n");
    pfs_durable_restore(emit);
    if (!pfs_durable_active()) { printf("  FAIL  [b5w-io] durable backend not active (PKERNEL_PFS_BACKEND=ark)\n"); return 2; }
    st_model n[K];
    for (int k = 0; k < K; k++) { st_init_tier(&n[k], 181u, ST_TIER_S); for (int p = 0; p < 4; p++) pass_q(&n[k], k); }
    const uint32_t G = 7;
    for (int k = 1; k < K; k++)
        if (publish((uint16_t)k, &n[k], G) <= 0) { printf("  FAIL  [b5w-io] publish %d\n", k); return 1; }
    tab_t t; memset(&t, 0, sizeof t);
    for (int k = 1; k < K; k++) t.b[k] = wrap(&n[k], G, &t.n[k]);
    const uint16_t all4[4] = { 0, 1, 2, 3 };
    st_rg_report rp, rm;

    /* W0: the transport and the table give the same bytes; four honest nodes are taken */
    st_model a, b; clone(&a, &n[0]); clone(&b, &n[0]);
    int ra = round_on(&a, G, 0, all4, 4, pfs_fetch, NULL, &rp);
    int rb = round_on(&b, G, 0, all4, 4, tab_fetch, &t, &rm);
    say("W0 pfs", ra, &rp); say("W0 table", rb, &rm);
    check(ra >= 0 && ra == rb && hash(&a) == hash(&b), "b5w-W0", "the round through the real pfs == the round from the table (bytes)");
    check(rp.members == 4 && rp.fetched == 3 && rp.same_gen == 4 && rp.accepted == 3,
          "b5w-W0", "four honest nodes: members 4 fetched 3 same-gen 4 accepted 3");
    st_free(&a); st_free(&b);

    /* W3: three nodes are refused, live byte-unchanged */
    clone(&a, &n[0]); uint64_t h0 = hash(&a);
    const uint16_t three[3] = { 0, 1, 2 };
    int r3 = round_on(&a, G, 0, three, 3, pfs_fetch, NULL, &rp); say("W3", r3, &rp);
    check(r3 == 0 && rp.accepted == 0 && rp.adopted == 0 && hash(&a) == h0, "b5w-W3", "three nodes: refused, live byte-unchanged");
    st_free(&a);

    /* W4: one peer of an old generation is not counted (same-gen 3, refused) */
    if (publish(23, &n[3], G - 1) <= 0) { printf("  FAIL  [b5w-io] publish 23\n"); return 1; }
    const uint16_t m4[4] = { 0, 1, 2, 23 };
    clone(&a, &n[0]); h0 = hash(&a);
    int r4 = round_on(&a, G, 0, m4, 4, pfs_fetch, NULL, &rp); say("W4", r4, &rp);
    check(rp.fetched == 3 && rp.same_gen == 3 && rp.accepted == 0 && r4 == 0 && hash(&a) == h0,
          "b5w-W4", "an old generation: fetched 3, same-gen 3, refused, live byte-unchanged");
    st_free(&a);

    /* W5: a peer with a chunk missing (chunk 5: inside w, §4 ④) is not counted */
    {
        st_model c; clone(&c, &n[3]); pass_q(&c, 3);   /* new bytes, so chunk 5 is not in the store */
        gl_student_test_drop_chunk(5);
        if (publish(33, &c, G) <= 0) { printf("  FAIL  [b5w-io] publish 33\n"); return 1; }
        const uint16_t m5[4] = { 0, 1, 2, 33 };
        clone(&a, &n[0]); h0 = hash(&a);
        int r5 = round_on(&a, G, 0, m5, 4, pfs_fetch, NULL, &rp); say("W5", r5, &rp);
        check(rp.members == 4 && rp.fetched == 2 && rp.same_gen == 3 && rp.accepted == 0 && r5 == 0 && hash(&a) == h0,
              "b5w-W5", "a chunk missing: fetched 2, refused, live byte-unchanged");
        st_free(&a); st_free(&c);
    }

    /* W6: two garbage peers: taken by the trim, refused by the gate, live byte-unchanged */
    {
        st_model g1, g2; garbage(&g1, 4101u); garbage(&g2, 4202u);
        if (publish(41, &g1, G) <= 0 || publish(42, &g2, G) <= 0) { printf("  FAIL  [b5w-io] publish 41/42\n"); return 1; }
        const uint16_t m6[4] = { 0, 1, 41, 42 };
        clone(&a, &n[0]); h0 = hash(&a);
        int r6 = round_on(&a, G, 0, m6, 4, pfs_fetch, NULL, &rp); say("W6", r6, &rp);
        check(rp.accepted == 3, "b5w-W6", "two garbage peers reach the merge (accepted 3)");
        check(r6 == 0 && rp.adopted == 0 && hash(&a) == h0, "b5w-W6", "two garbage peers: adopted 0, live byte-unchanged");
        st_free(&a); st_free(&g1); st_free(&g2);
    }

    w7_w10(n, &t, G);

    /* the arguments */
    clone(&a, &n[0]);
    check(st_region_round(&a, G, 0, all4, ST_COHORT_MAX + 1, tab_fetch, &t, fx + fx_end, fxn - fx_end, SEQ, NULL) == ST_E_ARG
          && st_region_round(&a, G, 0, all4, 4, NULL, NULL, fx + fx_end, fxn - fx_end, SEQ, NULL) == ST_E_ARG
          && st_region_round(NULL, G, 0, all4, 4, tab_fetch, &t, fx + fx_end, fxn - fx_end, SEQ, NULL) == ST_E_ARG,
          "b5w-arg", "bad arguments are ST_E_ARG");
    char tiny[8];
    check(st_rg_wrap(&a, G, tiny, sizeof tiny) < 0, "b5w-arg", "st_rg_wrap refuses a buffer too small");
    st_free(&a);
    tab_free(&t);
    for (int k = 0; k < K; k++) st_free(&n[k]);
    printf("[b5-wire] unit %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

/* ---- the arms (W1 W2) ---- */
typedef struct { double best_g, best_b; int best_p; } keeper_t;
static void keep_init(keeper_t *k) { k->best_g = 1e9; k->best_b = 0; k->best_p = 0; }
static void keep_see(keeper_t *k, st_model *m, int p)
{
    double g = gate(m);
    if (g < k->best_g) { k->best_g = g; k->best_p = p; k->best_b = bpb(m); }
}
static void keep_print(keeper_t *k, const char *name, st_model *last, const char *extra)
{
    printf("[b5w] arm=%s last=%.4f kept=%.4f at %d (gate %.4f) lastgate=%.4f %s\n",
           name, bpb(last), k->best_b, k->best_p, k->best_g, gate(last), extra);
}
static void alone(uint32_t seed)
{
    st_model m; st_init_tier(&m, seed, ST_TIER_S);
    keeper_t k; keep_init(&k);
    for (int p = 1; p <= PASSES; p++) { pass_q(&m, 0); keep_see(&k, &m, p); }
    keep_print(&k, "alone", &m, "");
    st_free(&m);
}
/* mean = 1: the comparator (st_round_merge into a copy, the same gate) */
static void rounds(uint32_t seed, int poison, int mean, const char *name)
{
    st_model n[K];
    for (int k = 0; k < K; k++) st_init_tier(&n[k], seed, ST_TIER_S);
    keeper_t kp; keep_init(&kp);
    int took = 0, tries = 0, accsum = 0; uint32_t gen = 0;
    int honest = K - poison;
    const uint16_t all4[4] = { 0, 1, 2, 3 };
    for (int p = 1; p <= PASSES; p++) {
        for (int k = 0; k < honest; k++) pass_q(&n[k], k);
        if (p % EVERY == 0) {
            gen++;
            for (int k = honest; k < K; k++) { st_free(&n[k]); garbage(&n[k], seed + (uint32_t)p + 977u * (uint32_t)k); }
            tab_t t; memset(&t, 0, sizeof t);
            for (int k = 0; k < K; k++) t.b[k] = wrap(&n[k], gen, &t.n[k]);   /* one snapshot */
            for (int k = 0; k < honest; k++) {
                int r;
                if (!mean) {
                    st_rg_report rp;
                    r = round_on(&n[k], gen, (uint16_t)k, all4, 4, tab_fetch, &t, &rp);
                    if (k == 0) accsum += rp.accepted;
                } else {
                    st_model c; clone(&c, &n[k]);
                    const void *bs[K - 1]; size_t ls[K - 1]; uint32_t gs[K - 1]; int j = 0;
                    for (int q = 0; q < K; q++) if (q != k) {
                        bs[j] = (const uint8_t *)t.b[q] + ST_RG_HDR; ls[j] = t.n[q] - ST_RG_HDR; gs[j] = gen; j++;
                    }
                    int a = st_round_merge(&c, gen, bs, ls, gs, K - 1);
                    if (k == 0) accsum += a > 0 ? a : 0;
                    r = a > 0 ? st_round_adopt(&n[k], &c, fx + fx_end, fxn - fx_end, SEQ, NULL, NULL) : 0;
                    st_free(&c);
                }
                if (k == 0) { took += r == 1; tries++; }
            }
            tab_free(&t);
        }
        keep_see(&kp, &n[0], p);
    }
    char ex[64]; snprintf(ex, sizeof ex, "took=%d/%d acc=%d", took, tries, accsum);
    keep_print(&kp, name, &n[0], ex);
    for (int k = 0; k < K; k++) st_free(&n[k]);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    fx = (const uint8_t *)TEACHER_FIXTURE; fxn = (int)sizeof(TEACHER_FIXTURE) - 1;
    fx_end = st_held_split(fxn, SEQ).train_end;
    lg = malloc((size_t)SEQ * ST_VOCAB * sizeof(float));
    if (!lg || fx_end < D) { printf("  FAIL  [b5w-io] setup (fx_end %d)\n", fx_end); return 1; }
    if (argc > 1 && strcmp(argv[1], "unit") == 0) return unit();
    if (argc < 3) { fprintf(stderr, "usage: %s unit | <b1_held.txt> <seed> [all|w2]\n", argv[0]); return 2; }
    uint32_t seed = (uint32_t)strtoul(argv[2], NULL, 10);
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    hn = (int)fread(held, 1, sizeof held, f); fclose(f);
    const char *st = argc > 3 ? argv[3] : "all";
#if defined(B5W_NC_MEAN)
    const char *bld = "nomean";
#elif defined(B5W_NC_NO_GATE)
    const char *bld = "nogate";
#elif defined(B5W_NC_NO_GEN)
    const char *bld = "nogen";
#else
    const char *bld = "plain";
#endif
    printf("[b5w] seed %u build %s stage %s quarter %d B gate windows %d held windows %d\n",
           (unsigned)seed, bld, st, D / K, (fxn - fx_end) / SEQ, hn / SEQ);
    int all = strcmp(st, "all") == 0;
    if (all) { alone(seed); rounds(seed, 0, 0, "region4"); }
    rounds(seed, 1, 0, "region4-garb1");
    rounds(seed, 1, 1, "mean4-garb1");
    if (all) rounds(seed, 2, 0, "region4-garb2");
    printf("[b5w] done\n");
    return 0;
}
