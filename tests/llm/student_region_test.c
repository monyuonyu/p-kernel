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
 *
 * B5 q8 (research/b5-q8-copy.md §4): unit Q0 Q4 (the BRG2 copy); stage q8
 * runs alone, region4 and region4-garb1 with the fp copy (BRG1) and region4-q8,
 * region4-garb1-q8, region4-garb2-q8 with the int8 copy (BRG2) on the same
 * seed; the judge is tests/llm/b5q8_check.py (Q1 Q2 Q3).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
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
/* B5 q8: weak, so the test-first commit links and Q0 Q4 are red by their lines */
#ifndef ST_RG_MAGIC2
#define ST_RG_MAGIC2 0x32475242u  /* "BRG2" (student.h from q8) */
#endif
extern size_t st_rg_q8_cap(const st_model *m) __attribute__((weak));
extern long   st_rg_wrap_q8(const st_model *m, uint32_t gen, void *buf, size_t cap) __attribute__((weak));
extern long   st_rg_q8_open(const st_model *live, const void *q, size_t n, void *out, size_t cap) __attribute__((weak));
static int g_q8 = 0;          /* 1: rounds() publishes the int8 copy (BRG2) */
static int have_q8(void) { return st_rg_q8_cap && st_rg_wrap_q8 && st_rg_q8_open; }
static void *wrap_q8(const st_model *m, uint32_t gen, size_t *len)
{
    if (!have_q8()) { printf("  FAIL  [b5q-io] st_rg_wrap_q8 is not there\n"); exit(1); }
    size_t cap = st_rg_q8_cap(m);
    void *b = malloc(cap);
    long n = b ? st_rg_wrap_q8(m, gen, b, cap) : -1;
    if (n < ST_RG_HDR) { printf("  FAIL  [b5q-io] wrap_q8 %ld\n", n); exit(1); }
    *len = (size_t)n;
    return b;
}
static void *wrap(const st_model *m, uint32_t gen, size_t *len)
{
    if (g_q8) return wrap_q8(m, gen, len);
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
/* ---- D6b (research/b5-m-round.md §10): W14, the round's members latched.
 * Weak, so the test-first commit links and W14 is red by its own line. ---- */
extern void     st_rg_latch_see(st_rg_latch *l, uint32_t gen, uint64_t now) __attribute__((weak));
extern uint64_t st_rg_latch_union(const st_rg_latch *l, uint32_t gen, uint64_t now) __attribute__((weak));
static void w14(void)
{
    if (!st_rg_latch_see || !st_rg_latch_union) {
        check(0, "b5w-W14", "st_rg_latch_see / _union are not there (D6b not built)");
        return;
    }
    st_rg_latch l; memset(&l, 0, sizeof l);
    const uint64_t head = (1ull << 0) | (1ull << 2) | (1ull << 3) | (1ull << 4);   /* 4 peers at the gen's head */
    const uint64_t start = (1ull << 0) | (1ull << 2);                            /* n3, n4 out at the merge's start */
    check(st_rg_latch_union(&l, 0, start) == start, "b5w-W14", "a zeroed latch adds nothing (gen 0)");
    st_rg_latch_see(&l, 100, head);
    st_rg_latch_see(&l, 100, head & ~(1ull << 3));                               /* n3 over tau mid-round */
    st_rg_latch_see(&l, 100, start);
    check(st_rg_latch_union(&l, 100, start) == head, "b5w-W14",
          "peers that left the region mid-round stay in the round's members (n3, n4)");
    check(st_rg_latch_union(&l, 100, start | (1ull << 5)) == (head | (1ull << 5)), "b5w-W14",
          "a peer first seen at the merge's start is in too");
    check(st_rg_latch_union(&l, 101, start) == start, "b5w-W14", "another gen: the view at the start only");
    st_rg_latch_see(&l, 101, start);
    check(st_rg_latch_union(&l, 101, start) == start && st_rg_latch_union(&l, 100, start) == start, "b5w-W14",
          "a new gen clears the old marks");
    st_rg_latch_see(&l, 102, 1ull << 63);
    check(st_rg_latch_union(&l, 102, 0) == (1ull << 63), "b5w-W14", "id 63 (the top narrow id) is kept");
}

/* ---- B5 ⓪ (research/b5-m-default.md §3 2, G5): W15, the published copy
 * carries the weights and zero Adam moments. The merge reads a peer's w only
 * (st_round_trimmed / cohort reset mu, vu), so mu and vu were 2/3 of the bytes
 * on the wire for nothing; zero, they are the same chunk on every node. The
 * live model keeps its own moments. Falsifier -DB5W_NC_SEND_MOMENTS. ---- */
static void w15(const st_model *m, uint32_t gen)
{
    int np = m->n_params, mu0 = 0;
    for (int i = 0; i < np; i++) mu0 += m->mu[i] != 0.0f || m->vu[i] != 0.0f;
    check(mu0 > 0, "b5w-W15", "precondition: the trained model has non-zero moments");
    size_t len; uint8_t *env = wrap(m, gen, &len);
    st_model c;
    int ld = st_init_tier(&c, 1u, m->tier) == ST_OK && st_load(&c, env + ST_RG_HDR, len - ST_RG_HDR) == ST_OK;
    check(ld, "b5w-W15", "the published copy loads");
    if (ld) {
        int wsame = memcmp(c.w, m->w, (size_t)np * sizeof(float)) == 0, zero = 1;
        for (int i = 0; i < np; i++) if (c.mu[i] != 0.0f || c.vu[i] != 0.0f) { zero = 0; break; }
        check(wsame, "b5w-W15", "the published copy carries the weights byte for byte");
        check(zero, "b5w-W15", "the published copy carries zero mu and vu");
        check(c.adam_t == m->adam_t, "b5w-W15", "the published copy keeps adam_t");
        st_free(&c);
    }
    int mu1 = 0;
    for (int i = 0; i < np; i++) mu1 += m->mu[i] != 0.0f || m->vu[i] != 0.0f;
    check(mu1 == mu0, "b5w-W15", "the live model keeps its own moments");
    free(env);
}

/* ---- B5 q8 (research/b5-q8-copy.md §4): Q0 and Q4 ---- */
static void q0(st_model *n, uint32_t G)
{
    if (!have_q8()) { check(0, "b5q-Q0", "st_rg_wrap_q8 / _q8_cap / _q8_open are not there (q8 not built)"); return; }
    const st_model *m = &n[0];
    int np = m->n_params;
    size_t len; uint8_t *env = wrap_q8(m, G, &len);
    uint32_t h[4]; memcpy(h, env, sizeof h);
    check(h[0] == ST_RG_MAGIC2 && h[1] == G && (size_t)h[2] == len - ST_RG_HDR, "b5q-Q0", "the envelope: BRG2, the gen, the payload length");
    size_t bs = st_blob_size(m);
    uint8_t *ob = malloc(bs);
    long on = st_rg_q8_open(m, env + ST_RG_HDR, len - ST_RG_HDR, ob, bs);
    st_model c, f;
    int ld = on == (long)bs && st_init_tier(&c, 1u, m->tier) == ST_OK && st_load(&c, ob, bs) == ST_OK;
    check(ld, "b5q-Q0", "the opened copy loads (st_save bytes)");
    st_init_tier(&f, 1u, m->tier);
    st_quant_fake(m, &f, 8, ST_QG_ROW, NULL);
    if (ld) {
        check(memcmp(c.w, f.w, (size_t)np * sizeof(float)) == 0, "b5q-Q0", "the opened w == st_quant_fake(8, ROW) byte for byte");
        int zero = 1;
        for (int i = 0; i < np; i++) if (c.mu[i] != 0.0f || c.vu[i] != 0.0f) { zero = 0; break; }
        check(zero && c.adam_t == m->adam_t, "b5q-Q0", "zero mu and vu, adam_t kept");
        int diff = 0;
        for (int i = 0; i < np; i++) diff += c.w[i] != m->w[i];
        printf("  Q0: %d of %d weights moved by the rounding, payload %zu bytes\n", diff, np, len - ST_RG_HDR);
        check(diff > 0, "b5q-Q0", "precondition: the rounding moved some weights (the copy is not the fp one)");
        st_free(&c);
    }
    /* a bad scale, a short payload: not opened */
    uint8_t *bad = malloc(len); memcpy(bad, env, len);
    float nanv = 0.0f / 0.0f;
    memcpy(bad + ST_RG_HDR + 40, &nanv, sizeof nanv);           /* the first row's scale (after the 40-byte ST_BLOB_HDR) */
    check(st_rg_q8_open(m, bad + ST_RG_HDR, len - ST_RG_HDR, ob, bs) < 0, "b5q-Q0", "a NaN scale is not opened");
    check(st_rg_q8_open(m, env + ST_RG_HDR, len - ST_RG_HDR - 1, ob, bs) < 0, "b5q-Q0", "a payload one byte short is not opened");
    free(bad); free(ob); free(env);

    /* the round: three BRG2 peers == three BRG1 peers that carry st_quant_fake(8, ROW) */
    tab_t tq, tf; memset(&tq, 0, sizeof tq); memset(&tf, 0, sizeof tf);
    for (int k = 1; k < K; k++) {
        tq.b[k] = wrap_q8(&n[k], G, &tq.n[k]);
        st_model qk; st_init_tier(&qk, 1u, n[k].tier); st_quant_fake(&n[k], &qk, 8, ST_QG_ROW, NULL);
        qk.adam_t = n[k].adam_t;
        tf.b[k] = wrap(&qk, G, &tf.n[k]);
        st_free(&qk);
    }
    const uint16_t all4[4] = { 0, 1, 2, 3 };
    st_model a, b; st_rg_report ra, rb;
    clone(&a, &n[0]); clone(&b, &n[0]);
    int r1 = round_on(&a, G, 0, all4, 4, tab_fetch, &tq, &ra); say("Q0 BRG2", r1, &ra);
    int r2 = round_on(&b, G, 0, all4, 4, tab_fetch, &tf, &rb); say("Q0 BRG1 of the fake", r2, &rb);
    check(ra.fetched == 3 && ra.accepted == 3 && r1 == r2 && hash(&a) == hash(&b), "b5q-Q0",
          "a round of three BRG2 peers == the round of their st_quant_fake copies as BRG1 (bytes)");
    st_free(&a); st_free(&b);
    /* a swarm in the middle of the change: node 1 BRG1, nodes 2 3 BRG2 */
    free(tq.b[1]); tq.b[1] = wrap(&n[1], G, &tq.n[1]);
    clone(&a, &n[0]);
    int r3 = round_on(&a, G, 0, all4, 4, tab_fetch, &tq, &ra); say("Q0 mixed", r3, &ra);
    check(ra.fetched == 3 && ra.same_gen == 4 && ra.accepted == 3, "b5q-Q0", "BRG1 and BRG2 in one round: all three counted");
    st_free(&a);
    tab_free(&tq); tab_free(&tf);
    st_free(&f);
}

static void q4(void)
{
    if (!have_q8()) { check(0, "b5q-Q4", "st_rg_wrap_q8 is not there (q8 not built)"); return; }
    st_model m; st_init_tier(&m, 7u, ST_TIER_M);
    size_t w = (size_t)m.n_params * sizeof(float);
    size_t len; void *e = wrap_q8(&m, 1, &len);
    printf("  Q4: M copy %zu bytes (fp32 w %zu, ratio %.4f; the BRG1 copy %zu)\n", len, w, (double)len / (double)w, st_rg_cap(&m));
    check(len <= 2060000u && (double)len <= 0.27 * (double)w, "b5q-Q4", "the M copy <= 2.06 MB and <= 0.27 x its w");
    free(e); st_free(&m);
    st_model l; st_init_tier(&l, 7u, ST_TIER_L);
    e = wrap_q8(&l, 1, &len);
    int pq = gl_student_publish(60, e, (unsigned int)len);
    printf("  Q4: L copy %zu bytes, publish %d chunks\n", len, pq);
    check(pq > 0, "b5q-Q4", "the L copy (BRG2) passes publish");
    free(e);
    size_t fc = st_rg_cap(&l);
    void *big = malloc(fc);                   /* never written: publish refuses by the length first */
    int pf = big ? gl_student_publish(61, big, (unsigned int)fc) : -2;
    printf("  Q4: L fp copy %zu bytes, publish %d\n", fc, pf);
    check(pf < 0, "b5q-Q4", "the L fp copy (BRG1) is still refused by publish");
    free(big); st_free(&l);
}

/* ---- B5 q8 2 (research/b5-recv-mem.md §3): Q5, the bytes a round holds at
 * once. Weak, so the test-first commit links; the lines are the design's. ---- */
extern size_t st_rg_test_bytes(void) __attribute__((weak));
static int fail_fetch(void *ctx, uint16_t node, void *out, size_t cap)
{
    (void)ctx; (void)node; (void)out; (void)cap;
    return -1;
}
static void q5(uint32_t G)
{
    if (!have_q8() || !st_rg_test_bytes) { check(0, "b5r-Q5", "st_rg_test_bytes is not there"); return; }
    const size_t fmax = 32u * 1024u * 1024u + ST_RG_HDR;      /* publish: 8192 chunks x 4096 B */
    /* M, three BRG2 peers: the peers' w, one fetch buffer, the scratch, the candidate */
    st_model mk[K];
    for (int k = 0; k < K; k++) st_init_tier(&mk[k], 191u + (uint32_t)k, ST_TIER_M);
    size_t np = (size_t)mk[0].n_params, bs = st_blob_size(&mk[0]);
    size_t wsz = bs - 2u * np * sizeof(float) - sizeof(int32_t);   /* header + w */
    size_t fc = st_rg_cap(&mk[0]) < fmax ? st_rg_cap(&mk[0]) : fmax;
    tab_t tq; memset(&tq, 0, sizeof tq);
    for (int k = 1; k < K; k++) tq.b[k] = wrap_q8(&mk[k], G, &tq.n[k]);
    const uint16_t all4[4] = { 0, 1, 2, 3 };
    st_rg_report rp;
    round_on(&mk[0], G, 0, all4, 4, tab_fetch, &tq, &rp); say("Q5 M", 0, &rp);
    size_t got = st_rg_test_bytes();
    size_t line = fc + 3u * wsz + st_rg_q8_cap(&mk[0]) + bs;
    printf("  Q5: M three peers held %zu bytes (line %zu; header+w %zu, the full copy %zu)\n", got, line, wsz, st_rg_cap(&mk[0]));
    check(rp.fetched == 3 && rp.accepted == 3, "b5r-Q5", "precondition: the M round took its three peers");
    check(got > 0 && got <= line, "b5r-Q5", "M, three BRG2 peers: the round holds <= one fetch buffer + 3 x (header + w) + scratch + candidate");
    /* RG-FETCH (research/b5-recv-mem.md §6): the count is the real malloc. The
     * candidate is an st_init_tier model, w g mu vu (4 x n_params floats), held
     * with the three peers' header + w; audit-219 measured 12 MB more than a
     * count of bs (w mu vu) */
    size_t floor_ = 3u * wsz + 4u * np * sizeof(float);
    printf("  Q5: M counted %zu, the merge phase allocates at least %zu\n", got, floor_);
    check(got >= floor_, "b5r-Q5", "M: the count is not below the real allocation of the merge phase (3 x (header + w) + the candidate's 4 x n_params floats)");
    tab_free(&tq);
    for (int k = 0; k < K; k++) st_free(&mk[k]);
    /* L, seven peers that do not answer: the buffers are had before the first fetch */
    st_model l; st_init_tier(&l, 7u, ST_TIER_L);
    const uint16_t eight[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    int rl = st_region_round(&l, G, 0, eight, 8, fail_fetch, NULL, fx + fx_end, fxn - fx_end, SEQ, &rp);
    got = st_rg_test_bytes();
    printf("  Q5: L seven peers held %zu bytes (line 700000000; the full copy %zu)\n", got, st_rg_cap(&l));
    check(rl == 0 && rp.members == 8 && rp.fetched == 0, "b5r-Q5", "precondition: the L round asked seven peers, none answered");
    check(got > 0 && got <= 700000000u, "b5r-Q5", "L, seven peers: the round holds <= 700 MB");
    st_free(&l);
}

static int unit(void)
{
    printf("=== [b5-wire] unit (research/b5-wire.md §3 §4) ===\n");
    w14();
    pfs_durable_restore(emit);
    if (!pfs_durable_active()) { printf("  FAIL  [b5w-io] durable backend not active (PKERNEL_PFS_BACKEND=ark)\n"); return 2; }
    st_model n[K];
    for (int k = 0; k < K; k++) { st_init_tier(&n[k], 181u, ST_TIER_S); for (int p = 0; p < 4; p++) pass_q(&n[k], k); }
    w15(&n[0], 7);
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
    q0(n, G);
    q4();
    q5(G);

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
/* W2b (research/b5-wire2.md §3): a weak poison. The node trains honestly and
 * publishes its weights + s x (the std of each weight family) x N(0,1). The
 * families are the 13 offsets of st_model (embed, attn_norm, wq..wo,
 * ffn_norm, router, w1 w3 w2, out_norm, out). */
static uint64_t wp_state;
static double wp_unif(void)
{
    wp_state ^= wp_state << 13; wp_state ^= wp_state >> 7; wp_state ^= wp_state << 17;
    return ((double)(wp_state >> 11) + 0.5) / 9007199254740992.0;
}
static double wp_gauss(void)
{
    double u = wp_unif(), v = wp_unif();
    return sqrt(-2.0 * log(u)) * cos(6.283185307179586 * v);
}
static void weak_poison(st_model *dst, const st_model *src, double s, uint32_t seed)
{
    clone(dst, src);
    int o[14] = { src->o_embed, src->o_attn_norm, src->o_wq, src->o_wk, src->o_wv, src->o_wo,
                  src->o_ffn_norm, src->o_router, src->o_w1, src->o_w3, src->o_w2,
                  src->o_out_norm, src->o_out, src->n_params };
    for (int i = 1; i < 14; i++)            /* in offset order (insertion sort) */
        for (int j = i; j > 0 && o[j - 1] > o[j]; j--) { int x = o[j]; o[j] = o[j - 1]; o[j - 1] = x; }
    wp_state = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0x100000001B3ull);
    for (int f = 0; f + 1 < 14; f++) {
        int a = o[f], b = o[f + 1];
        if (b <= a) continue;
        double m = 0, q = 0;
        for (int i = a; i < b; i++) m += src->w[i];
        m /= (double)(b - a);
        for (int i = a; i < b; i++) q += ((double)src->w[i] - m) * ((double)src->w[i] - m);
        double sd = sqrt(q / (double)(b - a));
        for (int i = a; i < b; i++) dst->w[i] = (float)((double)src->w[i] + s * sd * wp_gauss());
    }
}
static double g_weak = 0.0;   /* > 0: rounds() poisons by weak_poison, node K-1 */

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
            for (int k = 0; k < K; k++) {                                      /* one snapshot */
                if (g_weak > 0.0 && k == K - 1) {
                    st_model wp; weak_poison(&wp, &n[k], g_weak, seed * 131u + (uint32_t)p);
                    t.b[k] = wrap(&wp, gen, &t.n[k]);
                    st_free(&wp);
                } else t.b[k] = wrap(&n[k], gen, &t.n[k]);
            }
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
    if (strncmp(st, "w2b:", 4) == 0) {      /* W2b: alone, trim and mean against a weak poison */
        g_weak = strtod(st + 4, NULL);
        if (!(g_weak > 0.0)) { fprintf(stderr, "w2b:<s>, s > 0\n"); return 2; }
        printf("[b5w] seed %u stage w2b s %.3f held windows %d\n", (unsigned)seed, g_weak, hn / SEQ);
        alone(seed);
        rounds(seed, 0, 0, "region4-weak");
        rounds(seed, 0, 1, "mean4-weak");
        printf("[b5w] done\n");
        return 0;
    }
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
    if (strcmp(st, "q8nc") == 0) {          /* the NC build: Q2 only needs these two */
        if (!have_q8()) { printf("  FAIL  [b5q-io] q8 not built\n"); return 1; }
        alone(seed);
        g_q8 = 1;
        rounds(seed, 0, 0, "region4-q8");
        printf("[b5w] done\n");
        return 0;
    }
    if (strcmp(st, "q8") == 0) {            /* B5 q8 (research/b5-q8-copy.md §4) */
        if (!have_q8()) { printf("  FAIL  [b5q-io] q8 not built\n"); return 1; }
        alone(seed);
        rounds(seed, 0, 0, "region4");
        rounds(seed, 1, 0, "region4-garb1");
        g_q8 = 1;
        rounds(seed, 0, 0, "region4-q8");
        rounds(seed, 1, 0, "region4-garb1-q8");
        rounds(seed, 2, 0, "region4-garb2-q8");
        printf("[b5w] done\n");
        return 0;
    }
    int all = strcmp(st, "all") == 0;
    if (all) { alone(seed); rounds(seed, 0, 0, "region4"); }
    rounds(seed, 1, 0, "region4-garb1");
    rounds(seed, 1, 1, "mean4-garb1");
    if (all) rounds(seed, 2, 0, "region4-garb2");
    printf("[b5w] done\n");
    return 0;
}
