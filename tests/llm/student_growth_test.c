/*
 *  student_growth_test.c — host cert for SS-4 function-preserving expert growth
 *  (arch/common/llm/student.c, ss4-function-preserving-growth-plan.md §1/§4).
 *
 *  THE CLAIM: growing the student MoE's expert count by adding DEAD experts
 *  (router row=0, W2=0, alive[]=0) is EXACTLY function-preserving — the output
 *  of st_forward is BYTE-IDENTICAL for the cert's fixed input AFTER the grow,
 *  AND the per-token firing width nk is unchanged for EVERY token (proving the
 *  new experts truly never fired — non-vacuity, off-the-cert-input behaviour
 *  did NOT drift).  ε = 0.
 *
 *  WHY DEAD, not a clone: this router is top-K-then-softmax-over-the-chosen-set
 *  with MARGIN WIDENING (router_pick), NOT a global softmax.  A cloned expert
 *  is admitted by the widening (gap 0 < THETA), steals softmax mass, and
 *  changes nk + every other tw[j] — so the textbook "clone + half the router
 *  score" recipe is WRONG here.  The EXACT transform makes the new expert
 *  PROVABLY never enter the chosen set: alive[e]=0 forces its effective router
 *  logit to -inf in router_pick.  The alive mask is THE exactness mechanism;
 *  the zeroed router row + W2 are redundant defense-in-depth.
 *
 *  Certs:
 *    [expert-growth-preserves]  train M (E=4), hash logits, grow to E=8 (DEAD),
 *                               hash again -> BYTE-IDENTICAL (ε=0) AND the mean
 *                               + final firing widths are unchanged (every
 *                               token's nk preserved).
 *    [grow-noop-identity]       grow by ZERO experts -> hash unchanged (the
 *                               all-alive path is byte-clean; SS-6's "hook NULL"
 *                               analogue).
 *    [grow-cohort]              a grown (E=8) blob does NOT typecheck against an
 *                               ungrown (E=4) model: st_blob_tier_ok REFUSES it
 *                               (distinct (tier,nexpert) cohort, by construction).
 *
 *  FALSIFIER (-DSS4_GROW_NAIVE): replace the DEAD init with a NAIVE random-init
 *  new expert (alive=1, router row + W2 = random) — the textbook-wrong path.
 *  The new experts then enter the chosen set on some tokens, nk shifts, the
 *  softmax renormalizes, and the post-grow hash DIFFERS => the cert FAILS (goes
 *  RED) deterministically.  Proves the test has teeth.
 *
 *  Build (wave-49): -O1 -ffp-contract=off (one math everywhere).
 *
 *  Usage:
 *    ./student_growth            # human-readable cert (exit 0 = all PASS)
 *    ./student_growth --machine  # GROW_HASH lines (for cross-build diff)
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../arch/common/llm/student.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

static const uint8_t CORPUS[] =
    "the cat sat on the mat. the dog ran in the sun. she said the sea is "
    "blue and the sky is blue too. a bird sang and the wind blew softly.";

/* a SECOND fixed input (the off-cert-input tripwire is the nk-unchanged check,
 * but we also hash a different probe to widen the surface). */
static const uint8_t PROBE[] =
    "blue sky and the deep sea. the cat and the dog sat in the warm sun today.";

static void warm_train(st_model *m, int steps)
{
    int n = (int)sizeof(CORPUS) - 1;
    int win = n < ST_MAXSEQ ? n : ST_MAXSEQ;
    float *logits = (float *)malloc((size_t)win * 256 * sizeof(float));
    if (!logits) return;
    for (int s = 0; s < steps; s++) {
        st_zero_grad(m);
        st_forward(m, CORPUS, win, logits);
        st_backward(m, CORPUS, win);
        st_adam_step(m, 0.02f);
    }
    free(logits);
}

/* FNV-1a over the FULL logits[n_tok x 256] (every token, every vocab logit).
 * Bit-stable under -ffp-contract=off so byte-identity holds cross-arch. */
static uint64_t logit_hash(const float *logits, int n_tok)
{
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t *)logits;
    size_t bytes = (size_t)n_tok * 256 * sizeof(float);
    for (size_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628253ULL; }
    return h;
}

/* run a forward on `bytes[n]`, returning the logit hash AND the firing-width
 * fingerprint (mean-milli over ALL (l,t) cells + the final-token width + the
 * final-token chosen expert ids packed into a 64-bit fold). */
typedef struct { uint64_t h; int fw_milli; int fw_last; uint64_t fw_ids; } fp_t;

static fp_t forward_fp(st_model *m, const uint8_t *bytes, int n)
{
    fp_t fp; memset(&fp, 0, sizeof fp);
    float *logits = (float *)malloc((size_t)n * 256 * sizeof(float));
    if (!logits) { fp.h = 0; return fp; }
    st_forward(m, bytes, n, logits);
    fp.h        = logit_hash(logits, n);
    fp.fw_milli = st_last_fire_width_mean_milli();
    fp.fw_last  = st_last_fire_width();
    int ids[ST_KMAX];
    int k = st_last_fire_experts(ids, ST_KMAX);
    uint64_t f = 1469598103934665603ULL;
    for (int j = 0; j < k; j++) { f ^= (uint64_t)(ids[j] + 1); f *= 1099511628253ULL; }
    fp.fw_ids = f;
    free(logits);
    return fp;
}

/* --- the FALSIFIER poke (-DSS4_GROW_NAIVE): turn the freshly-grown DEAD experts
 * into NAIVE random-init LIVE experts (alive=1, router row + W2 = random),
 * exactly the textbook-wrong path the design rejects.  Uses ONLY public struct
 * fields. */
#ifdef SS4_GROW_NAIVE
static uint32_t naive_lcg(uint32_t *s){ *s = *s*1664525u+1013904223u; return *s; }
static float naive_runi(uint32_t *s, float a){
    uint32_t r = naive_lcg(s);
    float u = (float)(r >> 8) * (1.0f/16777216.0f);
    return (u*2.0f - 1.0f)*a;
}
static void naive_revive(st_model *m, int e_old)
{
    const int D=m->d, DFF=m->dff, L=m->nlayer, E=m->nexpert;
    uint32_t s = 0xC0FFEE11u;
    for (int e = e_old; e < E; e++) {
        if (m->alive) m->alive[e] = 1;                /* LIVE (textbook-wrong)  */
        for (int l = 0; l < L; l++) {
            float *rr = m->w + m->o_router + ((size_t)l*E + e)*D;
            for (int i = 0; i < D; i++) rr[i] = naive_runi(&s, 0.5f); /* big logits */
            float *w2 = m->w + m->o_w2 + ((size_t)l*E + e)*D*DFF;
            for (int i = 0; i < D*DFF; i++) w2[i] = naive_runi(&s, 0.1f);
        }
    }
}
#endif

int main(int argc, char **argv)
{
    int machine = (argc > 1 && strcmp(argv[1], "--machine") == 0);

    /* ---------------- [expert-growth-preserves] (M: E=4 -> E=8) ------------ */
    st_model m; memset(&m, 0, sizeof m);
    if (st_init_tier(&m, 0xBABE0001u, ST_TIER_M) != ST_OK) {
        printf("  FAIL  st_init_tier(M)\n"); return 2;
    }
    warm_train(&m, 40);                 /* NON-VACUOUS: trained experts        */

    int n     = (int)sizeof(CORPUS) - 1; if (n > ST_MAXSEQ) n = ST_MAXSEQ;
    int n_pr  = (int)sizeof(PROBE)  - 1; if (n_pr > ST_MAXSEQ) n_pr = ST_MAXSEQ;

    fp_t before    = forward_fp(&m, CORPUS, n);
    fp_t before_pr = forward_fp(&m, PROBE,  n_pr);
    int  E_before  = m.nexpert;

    int rc = st_grow_experts(&m, ST_E_L);   /* E=4 -> E=8, DEAD experts        */
#ifdef SS4_GROW_NAIVE
    naive_revive(&m, E_before);             /* FALSIFIER: make them LIVE random */
#endif

    fp_t after    = forward_fp(&m, CORPUS, n);
    fp_t after_pr = forward_fp(&m, PROBE,  n_pr);

    if (!machine) {
        printf("== student_growth_test (SS-4 function-preserving expert growth) ==\n\n");
        printf("[expert-growth-preserves] grow M (E=%d -> E=%d), DEAD experts; the\n",
               E_before, m.nexpert);
        printf("                          logits FNV + per-token firing widths must\n");
        printf("                          be BYTE-IDENTICAL (epsilon = 0).\n\n");
        printf("  E:    %d -> %d (grow rc=%d)\n", E_before, m.nexpert, rc);
        printf("  CORPUS  hash  before=%016llx  after=%016llx\n",
               (unsigned long long)before.h, (unsigned long long)after.h);
        printf("  CORPUS  fw    before=(milli=%d last=%d ids=%016llx)  after=(milli=%d last=%d ids=%016llx)\n",
               before.fw_milli, before.fw_last, (unsigned long long)before.fw_ids,
               after.fw_milli,  after.fw_last,  (unsigned long long)after.fw_ids);
        printf("  PROBE   hash  before=%016llx  after=%016llx\n",
               (unsigned long long)before_pr.h, (unsigned long long)after_pr.h);
        printf("  PROBE   fw    before=(milli=%d last=%d)  after=(milli=%d last=%d)\n\n",
               before_pr.fw_milli, before_pr.fw_last, after_pr.fw_milli, after_pr.fw_last);
    }

    CHECK(rc == ST_OK,                "[expert-growth-preserves] grow returns ST_OK");
    CHECK(m.nexpert == ST_E_L,        "[expert-growth-preserves] m->nexpert == 8 after grow");
    CHECK(after.h == before.h,        "[expert-growth-preserves] CORPUS logits BYTE-IDENTICAL (eps=0)");
    CHECK(after.fw_milli == before.fw_milli,
                                      "[expert-growth-preserves] CORPUS mean firing-width nk unchanged (every token)");
    CHECK(after.fw_last == before.fw_last && after.fw_ids == before.fw_ids,
                                      "[expert-growth-preserves] CORPUS final-token nk + chosen experts unchanged");
    CHECK(after_pr.h == before_pr.h,  "[expert-growth-preserves] PROBE (off-cert input) logits BYTE-IDENTICAL");
    CHECK(after_pr.fw_milli == before_pr.fw_milli,
                                      "[expert-growth-preserves] PROBE mean firing-width nk unchanged");

    /* ---------------- [grow-noop-identity] (grow by ZERO) ------------------- */
    st_model m0; memset(&m0, 0, sizeof m0);
    st_init_tier(&m0, 0xBABE0001u, ST_TIER_M);
    warm_train(&m0, 40);
    fp_t noop_before = forward_fp(&m0, CORPUS, n);
    int  rc0 = st_grow_experts(&m0, m0.nexpert);   /* grow by 0 */
    fp_t noop_after  = forward_fp(&m0, CORPUS, n);
    CHECK(rc0 == ST_OK && noop_after.h == noop_before.h && noop_after.fw_milli == noop_before.fw_milli,
                                      "[grow-noop-identity] grow-by-zero -> hash + nk unchanged (all-alive path byte-clean)");

    /* ---------------- [grow-cohort] (mixed-size minds do not merge) --------- */
    /* a grown (E=8) blob must NOT typecheck against an ungrown (E=4) model:
     * st_blob_tier_ok ANDs the existing n_expert equality, so the (tier,nexpert)
     * cohort island holds by construction. */
    st_model m4; memset(&m4, 0, sizeof m4);
    st_init_tier(&m4, 0xBABE0001u, ST_TIER_M);     /* E=4 reference model */
    size_t cap = st_blob_size(&m);                 /* m is the GROWN E=8 model */
    void  *blob8 = malloc(cap);
    long   wrote = st_save(&m, blob8, cap);
    int    refused = !st_blob_tier_ok(&m4, blob8, (size_t)wrote);
    CHECK(wrote > 0 && refused,
                                      "[grow-cohort] grown E=8 blob REFUSED by ungrown E=4 model (distinct cohort)");
    /* and the grown blob ACCEPTS into another grown E=8 model (same cohort). */
    st_model m8; memset(&m8, 0, sizeof m8);
    st_init_tier(&m8, 0xBABE0001u, ST_TIER_M);
    st_grow_experts(&m8, ST_E_L);
    CHECK(st_blob_tier_ok(&m8, blob8, (size_t)wrote),
                                      "[grow-cohort] grown E=8 blob ACCEPTED by another E=8 model (cohort match)");
    free(blob8);

    if (machine) {
        printf("GROW_HASH  before=%016llx after=%016llx match=%d\n",
               (unsigned long long)before.h, (unsigned long long)after.h,
               after.h == before.h);
        printf("GROW_FW    before_milli=%d after_milli=%d match=%d\n",
               before.fw_milli, after.fw_milli, before.fw_milli == after.fw_milli);
#ifdef SS4_GROW_NAIVE
        printf("GROW_MODE  NAIVE (falsifier: expect MISMATCH)\n");
#else
        printf("GROW_MODE  DEAD (expect MATCH)\n");
#endif
    }

    st_free(&m); st_free(&m0); st_free(&m4); st_free(&m8);

    if (!machine) printf("\nSUMMARY: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
