/*
 *  student_nan_test.c - a non-finite model must never read as a good one
 *  (ROADMAP B4; audit-114 found st_logf(NaN) = +89.13, a NaN model's loss
 *  = -89.13, the best loss there is).
 *
 *  Links the REAL student_shell.c (#include, like student_ema_test.c) and
 *  drives the production student_dmn_consolidate() on the fixture.
 *
 *   [logf-*]    st_logf: NaN and +-Inf give the floor -69 (the worst loss), and
 *               every other input is bit-equal to the recipe before the fix
 *   [loss-*]    a model with one NaN / Inf in a used weight does not read
 *               better than chance (st_eval_loss, st_span_ce)
 *   [guarded-*] st_merge_guarded does not pick a poisoned peer
 *   [gate-*]    the DMN save and the explicit save write no non-finite model:
 *               the first save (it has no loss to compare with), a save after
 *               a good one, an unused row (the loss cannot see it), Inf, the
 *               Adam state. A clean model is still written
 *   [gate-ema]  (argv "ema", PKERNEL_STUDENT_EMA=1) the saved copy too
 *
 *  Falsifiers (run_st_nan.sh): -DST_LOGF_NO_FINITE, -DST_SAVE_NO_FINITE, both.
 *  Build: -std=c11 -O1 -Wall -Wextra -ffp-contract=off.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../arch/common/llm/student_shell.c"

/* ---------- kernel-seam stubs (as in student_ema_test.c) ---------- */
static unsigned g_cap_count = 0;    /* writes */
static unsigned g_cap_bad   = 0;    /* writes holding a non-finite value */
static unsigned g_cap_ok    = 0;    /* writes that load and are finite */

static int nonfinite_f(const float *p, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, &p[i], sizeof u);
        c += (u & 0x7F800000u) == 0x7F800000u;
    }
    return c;
}
static int nonfinite_m(const st_model *m)
{
    return nonfinite_f(m->w, m->n_params) + nonfinite_f(m->mu, m->n_params) +
           nonfinite_f(m->vu, m->n_params);
}

int pfs_dur_active(void) { return 1; }
int pfs_dur_write(const char *f, const void *d, unsigned n)
{
    (void)f;
    st_model c;
    g_cap_count++;
    if (st_init_tier(&c, STUDENT_SEED, ST_TIER_S) != ST_OK) return -1;
    if (st_load(&c, d, n) != ST_OK) g_cap_bad++;
    else if (nonfinite_m(&c)) g_cap_bad++;
    else g_cap_ok++;
    st_free(&c);
    return 0;
}
int pfs_dur_read (const char *f, void *b, unsigned m) { (void)f;(void)b;(void)m; return -1; }
/* tier S: a tick in seconds; the gate is the same code on every tier */
int st_init_device(st_model *m, uint32_t seed) { return st_init_tier(m, seed, ST_TIER_S); }
int  gguf_open (gguf_file *gf, const char *p) { (void)gf;(void)p; return -1; }
void gguf_close(gguf_file *gf)                { (void)gf; }
int  lm_load   (lm_model *m, const gguf_file *gf) { (void)m;(void)gf; return -1; }
void lm_free   (lm_model *m)                  { (void)m; }
void     ss6_live_install(void *m)   { (void)m; }
void     ss6_live_uninstall(void)    { }
void     ss6_live_set_enabled(int o) { (void)o; }
unsigned ss6_live_req_sent(void)     { return 0; }
unsigned ss6_live_req_served(void)   { return 0; }

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

static float from_u(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }
static uint32_t to_u(float f)   { uint32_t u; memcpy(&u, &f, sizeof u); return u; }

/* the recipe as it was before the fix, frozen here */
static float ref_logf(float x)
{
    union { float f; uint32_t u; } v;
    if (x < 1e-30f) return -69.0f;
    v.f = x;
    int e = (int)((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFFU) | 0x3F800000U;
    float m  = v.f;
    float z  = (m - 1.0f) / (m + 1.0f);
    float z2 = z * z;
    float l  = 2.0f * z * (1.0f + z2 * (0.33333333f + z2 * (0.2f +
               z2 * (0.14285714f + z2 * 0.11111111f))));
    return l + (float)e * 0.69314718f;
}

static void logf_checks(void)
{
    static const uint32_t nf[] = {
        0x7FC00000u, 0x7F800001u, 0x7FFFFFFFu,   /* NaN, signalling, payload */
        0xFFC00000u, 0xFF800001u, 0xFFFFFFFFu,   /* the same with the sign set */
        0x7F800000u, 0xFF800000u                 /* +Inf, -Inf */
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof nf / sizeof nf[0]; i++) {
        float y = st_logf(from_u(nf[i]));
        if (to_u(y) != to_u(-69.0f)) {
            printf("    st_logf(0x%08X) = %f\n", nf[i], (double)y);
            bad++;
        }
    }
    CHECK(bad == 0, "[logf-nonfinite] NaN (8 patterns with Inf) -> -69, the worst loss");

    /* a stride over the whole range: everything but NaN and +Inf is unchanged */
    unsigned long diff = 0, n = 0;
    for (uint64_t u = 0; u <= 0xFFFFFFFFull; u += 1009) {
        uint32_t b = (uint32_t)u;
        if ((b & 0x7F800000u) == 0x7F800000u) continue;
        n++;
        if (to_u(st_logf(from_u(b))) != to_u(ref_logf(from_u(b)))) diff++;
    }
    static const float pin[] = { 1e-30f, 1.0f, 2.0f, 256.0f, 0.9f, 0.999f, 3.0e38f,
                                 3.4028235e38f, 1e-45f, 0.0f, -0.0f, -1.0f };
    for (unsigned i = 0; i < sizeof pin / sizeof pin[0]; i++) {
        n++;
        if (to_u(st_logf(pin[i])) != to_u(ref_logf(pin[i]))) diff++;
    }
    printf("    compared %lu finite inputs, %lu differ\n", n, diff);
    CHECK(diff == 0 && n > 4000000ul, "[logf-finite-bits] finite inputs are bit-equal to the old recipe");
}

#define SEQ 24
static const uint8_t TXT[] =
    "the cat sat on the mat. the dog ran in the sun. she said the sea is blue "
    "and the sky is blue too. a bird sang on the old oak tree at dawn. the cat "
    "and the dog sat by the sea and saw the sun set red. the old man ran to the ";

static void loss_checks(void)
{
    const float chance = st_logf(256.0f);
    st_model m;
    if (st_init_tier(&m, 2, ST_TIER_S) != ST_OK) { CHECK(0, "[loss] init"); return; }
    int np = 0;
    float clean = st_eval_loss(&m, TXT, SEQ, &np);
    float *w = m.w + m.o_out + 'e' * m.d;
    float keep = *w;
    static const struct { uint32_t u; const char *n; } P[] = {
        { 0x7FC00000u, "NaN" }, { 0x7F800000u, "+Inf" }, { 0xFF800000u, "-Inf" }
    };
    int bad = 0;
    for (unsigned i = 0; i < 3; i++) {
        *w = from_u(P[i].u);
        uint64_t h = 0;
        float l = st_eval_loss(&m, TXT, SEQ, &np);
        float s = st_span_ce(&m, TXT, SEQ, 1, SEQ, &h);
        printf("    %s in a used weight: st_eval_loss %f, st_span_ce %f (clean %f, chance %f)\n",
               P[i].n, (double)l, (double)s, (double)clean, (double)chance);
        /* !(l >= chance) so that a NaN loss counts as bad */
        if (!(l >= chance) || !(s >= chance)) bad++;
    }
    *w = keep;
    CHECK(bad == 0, "[loss-not-better] one non-finite used weight reads no better than chance");
    st_free(&m);
}

static void guarded_checks(void)
{
    st_model a, p, i, s;
    const uint8_t *val[2] = { TXT, TXT + SEQ };
    if (st_init_tier(&a, 2, ST_TIER_S) != ST_OK || st_init_tier(&p, 3, ST_TIER_S) != ST_OK ||
        st_init_tier(&i, 2, ST_TIER_S) != ST_OK || st_init_tier(&s, 2, ST_TIER_S) != ST_OK) {
        CHECK(0, "[guarded] init"); return;
    }
    p.w[p.o_out + 'e' * p.d] = from_u(0x7FC00000u);
    float cand[ST_MERGE_NALPHA + 1];
    int r = st_merge_guarded(&a, &p, &i, &s, val, 2, SEQ, cand);
    int nf = nonfinite_f(a.w, a.n_params);
    printf("    st_merge_guarded rc %d, non-finite in the child %d, cand[0] %f cand[1] %f\n",
           r, nf, (double)cand[0], (double)cand[1]);
    CHECK(r >= 0 && nf == 0, "[guarded-nan] a NaN peer does not win the pick");
    st_free(&a); st_free(&p); st_free(&i); st_free(&s);
}

/* ---------- the resident baby ---------- */
static int fresh(void)
{
    g_have_student = 0; g_consol_active = 0; g_consol_idx = 0;
    g_dmn_saved = 0; g_dmn_saved_loss = 0.0f; g_dmn_saved_adam_t = 0;
    g_ema_have = 0; g_ema_saves = 0;
    return student_ensure(0);
}
/* one whole batch; the writes it made */
static unsigned tick(void)
{
    unsigned before = g_cap_count;
    int rc = 0, calls = 0;
    do { rc = student_dmn_consolidate(); calls++; } while (rc == 0 && calls < 100000);
    return g_cap_count - before;
}
static void quiet(const char *s) { (void)s; }

static void poison(float *p) { *p = from_u(0x7FC00000u); }

static void gate_checks(void)
{
    const float pinf = from_u(0x7F800000u);
    unsigned w, b0;

    /* control: a clean baby is written */
    if (fresh() != 0) { CHECK(0, "[gate] student_ensure"); return; }
    b0 = g_cap_bad;
    unsigned w1 = tick(), w2 = tick();
    printf("    clean baby: writes %u %u, held %f\n", w1, w2, (double)student_dmn_heldout_loss());
    CHECK(w1 == 1 && w2 == 1 && g_cap_bad == b0, "[gate-clean] a clean baby is still written, twice");

    /* the first save has no loss to compare with */
    fresh(); b0 = g_cap_bad;
    poison(g_student.w + g_student.o_out + 'e' * g_student.d);
    w = tick();
    printf("    NaN, first save: writes %u (non-finite %u), held %f\n", w, g_cap_bad - b0,
           (double)student_dmn_heldout_loss());
    CHECK(g_cap_bad == b0, "[gate-first] a NaN baby is not written on the first save");

    /* after a good save: the loss comparison itself */
    fresh(); tick(); b0 = g_cap_bad;
    float sv = g_dmn_saved_loss;
    poison(g_student.w + g_student.o_out + 'e' * g_student.d);
    w = tick();
    printf("    NaN, after a good save (saved loss %f): writes %u (non-finite %u), held %f\n",
           (double)sv, w, g_cap_bad - b0, (double)student_dmn_heldout_loss());
    CHECK(g_cap_bad == b0, "[gate-after-good] a NaN baby does not pass for an improvement");

    /* an unused row: the byte 0xFF is in no text, the loss cannot see it */
    fresh(); tick(); b0 = g_cap_bad;
    poison(g_student.w + g_student.o_embed + 255 * g_student.d);
    w = tick();
    printf("    NaN in the unused row: writes %u (non-finite %u), non-finite in live %d\n",
           w, g_cap_bad - b0, nonfinite_f(g_student.w, g_student.n_params));
    CHECK(g_cap_bad == b0, "[gate-unused-nan] a NaN the loss cannot see is not written");

    fresh(); tick(); b0 = g_cap_bad;
    g_student.w[g_student.o_embed + 255 * g_student.d] = pinf;
    w = tick();
    printf("    Inf in the unused row: writes %u (non-finite %u)\n", w, g_cap_bad - b0);
    CHECK(g_cap_bad == b0, "[gate-unused-inf] an Inf the loss cannot see is not written");

    /* the Adam state is in the blob too */
    fresh(); tick(); b0 = g_cap_bad;
    poison(g_student.vu + g_student.o_embed + 255 * g_student.d);
    w = tick();
    printf("    NaN in Adam's second moment: writes %u (non-finite %u)\n", w, g_cap_bad - b0);
    CHECK(g_cap_bad == b0, "[gate-adam] a NaN in the Adam state is not written");

    /* the explicit save */
    fresh(); b0 = g_cap_bad;
    poison(g_student.w + g_student.o_embed + 255 * g_student.d);
    int rc = student_persist(quiet);
    printf("    student_persist with a NaN: rc %d, non-finite writes %u\n", rc, g_cap_bad - b0);
    CHECK(g_cap_bad == b0 && rc != 0, "[gate-persist] the explicit save refuses a NaN");
    fresh(); b0 = g_cap_count;
    rc = student_persist(quiet);
    CHECK(rc == 0 && g_cap_count == b0 + 1, "[gate-persist-clean] the explicit save writes a clean baby");
}

/* PKERNEL_STUDENT_EMA=1: the copy wins from about tick 4; poison the copy only,
 * in a row the loss cannot see */
static void ema_checks(void)
{
    if (fresh() != 0) { CHECK(0, "[gate-ema] student_ensure"); return; }
    int t;
    for (t = 1; t <= 12 && g_ema_saves == 0; t++) tick();
    printf("    the copy first written at tick %d (copy writes %u)\n", t - 1, g_ema_saves);
    CHECK(g_ema_saves > 0, "[gate-ema-wins] the copy does win (else the next check says nothing)");
    unsigned b0 = g_cap_bad, s0 = g_ema_saves, w0 = g_cap_count;
    poison(g_ema + g_student.o_embed + 255 * g_student.d);
    for (t = 0; t < 8; t++) tick();
    printf("    after the poison: writes %u, copy writes %u, non-finite writes %u\n",
           g_cap_count - w0, g_ema_saves - s0, g_cap_bad - b0);
    CHECK(g_cap_bad == b0, "[gate-ema] a non-finite copy is not written");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1 && strcmp(argv[1], "ema") == 0) {
        printf("[nan] the saved copy\n");
        ema_checks();
    } else {
        printf("[nan] st_logf\n");     logf_checks();
        printf("[nan] the loss\n");    loss_checks();
        printf("[nan] the merge\n");   guarded_checks();
        printf("[nan] the save\n");    gate_checks();
    }
    printf("[result] %s (%d pass, %d fail)\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
