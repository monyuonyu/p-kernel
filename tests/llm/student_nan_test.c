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
 *   [loss-*]    a model with one NaN / Inf in a weight of a row the windows use
 *               does not read better than chance (st_eval_loss, st_span_ce).
 *               Only there: an Inf in a row the windows miss can read better
 *               than a clean model (audit-116). The value check holds that
 *               line, not the loss
 *   [finite-*]  st_model_finite on its own: w, mu, vu, first and last element,
 *               the second half (the [gate-*] alone do not tell: Adam's next
 *               step carries a NaN from vu into w)
 *   [guarded-*] st_merge_guarded and st_merge_distill refuse a non-finite parent,
 *               in a row the windows use or not, and leave the model as it was
 *   [load-*]    st_load refuses a blob with a NaN or Inf in w, mu or vu (audit-116:
 *               rc 0), the model as it was; a clean blob still loads
 *   [cohort-*]  st_merge_cohort skips a peer with a non-finite weight, refuses
 *               a sum that overflows, leaves the model as it was (research/
 *               b5-region-rounds.md G4); a finite merge is what it always was
 *   [gate-*]    the DMN save and the explicit save write no non-finite model:
 *               the first save (it has no loss to compare with), a save after
 *               a good one, an unused row (the loss cannot see it), Inf, the
 *               Adam state, with no tick between (no step to carry it). A
 *               clean model is still written
 *   [refused-*] a refused save is said, in the probe: nothing else reads it
 *   [gate-ema]  (argv "ema", PKERNEL_STUDENT_EMA=1) the saved copy too
 *
 *  Falsifiers (run_st_nan.sh): -DST_LOGF_NO_FINITE, -DST_SAVE_NO_FINITE, both,
 *  -DST_MERGE_NO_FINITE.
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
    CHECK(bad == 0, "[loss-not-better] one non-finite weight in a row the windows use reads no better than chance");
    st_free(&m);
}

static void finite_checks(void)
{
    st_model m;
    if (st_init_tier(&m, 2, ST_TIER_S) != ST_OK) { CHECK(0, "[finite] init"); return; }
    CHECK(st_model_finite(&m), "[finite-clean] a fresh model is finite");
    float *a[3] = { m.w, m.mu, m.vu };
    const int n = m.n_params;
    const int at[] = { 0, n / 2 + 1, n - 2, n - 1 };
    static const uint32_t pat[] = { 0x7FC00000u, 0x7F800000u, 0xFF800000u, 0xFFC00001u };
    int missed = 0, tried = 0;
    for (int k = 0; k < 3; k++)
        for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++)
            for (unsigned p = 0; p < sizeof pat / sizeof pat[0]; p++) {
                float keep = a[k][at[i]];
                a[k][at[i]] = from_u(pat[p]);
                tried++;
                if (st_model_finite(&m)) {
                    printf("    missed: array %d element %d pattern 0x%08X\n", k, at[i], pat[p]);
                    missed++;
                }
                a[k][at[i]] = keep;
            }
    printf("    %d placements, %d missed\n", tried, missed);
    CHECK(missed == 0 && st_model_finite(&m),
          "[finite-everywhere] w, mu, vu; first, late and last element; NaN and both Inf");
    /* the largest finite value is not a reason to refuse */
    float keep = m.vu[n - 1];
    m.vu[n - 1] = from_u(0x7F7FFFFFu);
    CHECK(st_model_finite(&m), "[finite-big] the largest finite float is finite");
    m.vu[n - 1] = keep;
    st_free(&m);
}

/* a model's whole state, to say "unchanged, all bytes" */
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

static void guarded_checks(void)
{
    st_model a, p, i, s;
    const uint8_t *val[2] = { TXT, TXT + SEQ };
    if (st_init_tier(&a, 2, ST_TIER_S) != ST_OK || st_init_tier(&p, 3, ST_TIER_S) != ST_OK ||
        st_init_tier(&i, 2, ST_TIER_S) != ST_OK || st_init_tier(&s, 2, ST_TIER_S) != ST_OK) {
        CHECK(0, "[guarded] init"); return;
    }
    /* a row the windows use, a row they miss, the last element */
    const int at[] = { p.o_out + 'e' * p.d, p.o_embed + 255 * p.d, p.n_params - 1 };
    static const uint32_t pat[] = { 0x7FC00000u, 0x7F800000u, 0xFF800000u };
    const uint64_t h0 = model_hash(&a);
    int bad = 0, tried = 0;
    for (unsigned k = 0; k < 3; k++)
        for (unsigned q = 0; q < 3; q++) {
            float keep = p.w[at[k]];
            p.w[at[k]] = from_u(pat[q]);
            float cand[ST_MERGE_NALPHA + 1];
            int r = st_merge_guarded(&a, &p, &i, &s, val, 2, SEQ, cand);
            tried++;
            if (r >= 0 || model_hash(&a) != h0) {
                printf("    peer element %d pattern 0x%08X: rc %d, non-finite in the child %d\n",
                       at[k], pat[q], r, nonfinite_f(a.w, a.n_params));
                bad++;
                memcpy(a.w, i.w, (size_t)a.n_params * sizeof(float));
                for (int j = 0; j < a.n_params; j++) { a.mu[j] = 0.0f; a.vu[j] = 0.0f; }
                a.adam_t = 0;
            }
            p.w[at[k]] = keep;
        }
    printf("    %d poisoned peers, %d not refused\n", tried, bad);
    CHECK(bad == 0, "[guarded-nan] a non-finite peer is refused, row used or not, the model unchanged");

    float keepa = a.w[a.n_params - 1];
    a.w[a.n_params - 1] = from_u(0x7FC00000u);
    uint64_t h1 = model_hash(&a);
    int r = st_merge_guarded(&a, &p, &i, &s, val, 2, SEQ, NULL);
    CHECK(r < 0 && model_hash(&a) == h1, "[guarded-into] a non-finite model is not merged into");
    a.w[a.n_params - 1] = keepa;

    const uint8_t *xfer[1] = { TXT };
    p.w[at[1]] = from_u(0x7FC00000u);
    r = st_merge_distill(&a, &p, &i, &s, xfer, 1, val, 2, SEQ, 4, 1e-3f, ST_DISTILL_START_PICK, NULL);
    printf("    st_merge_distill with a NaN peer: rc %d\n", r);
    CHECK(r < 0 && model_hash(&a) == h0, "[guarded-distill] st_merge_distill refuses it too");
    p.w[at[1]] = 0.0f;

    /* the control: finite parents still merge */
    r = st_merge_guarded(&a, &p, &i, &s, val, 2, SEQ, NULL);
    CHECK(r >= 0 && nonfinite_f(a.w, a.n_params) == 0, "[guarded-clean] finite parents still merge");
    st_free(&a); st_free(&p); st_free(&i); st_free(&s);
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

static int cohort_with(st_model *a, const st_model *peer)
{
    size_t l = 0;
    void *b = blob_of(peer, &l);
    if (!b) return -99;
    const void *pb[1] = { b };
    size_t pl[1] = { l };
    int r = st_merge_cohort(a, pb, pl, 1);
    free(b);
    return r;
}

static void load_checks(void)
{
    st_model a, p;
    if (st_init_tier(&a, 2, ST_TIER_S) != ST_OK || st_init_tier(&p, 3, ST_TIER_S) != ST_OK) {
        CHECK(0, "[load] init"); return;
    }
    const int n = p.n_params;
    float *arr[3] = { p.w, p.mu, p.vu };
    const int at[] = { 0, n / 2 + 1, n - 1 };
    static const uint32_t pat[] = { 0x7FC00000u, 0x7F800000u, 0xFF800000u };
    const uint64_t h0 = model_hash(&a);
    int bad = 0;
    for (int k = 0; k < 3; k++)
        for (unsigned i = 0; i < 3; i++)
            for (unsigned q = 0; q < 3; q++) {
                float keep = arr[k][at[i]];
                arr[k][at[i]] = from_u(pat[q]);
                size_t l = 0;
                void *b = blob_of(&p, &l);
                arr[k][at[i]] = keep;
                int r = b ? st_load(&a, b, l) : ST_OK;
                free(b);
                if (r == ST_OK || model_hash(&a) != h0) bad++;
                if (model_hash(&a) != h0) {
                    st_free(&a);
                    if (st_init_tier(&a, 2, ST_TIER_S) != ST_OK) { CHECK(0, "[load] init"); return; }
                }
            }
    printf("    27 poisoned blobs, %d loaded\n", bad);
    CHECK(bad == 0, "[load-nonfinite] NaN or Inf in w, mu, vu: refused, the model as it was");
    size_t l = 0;
    void *b = blob_of(&p, &l);
    int r = b ? st_load(&a, b, l) : ST_E_ARG;
    free(b);
    CHECK(r == ST_OK && model_hash(&a) == model_hash(&p), "[load-clean] a clean blob loads, every byte");
    st_free(&a); st_free(&p);
}

static void cohort_checks(void)
{
    st_model a, p, q;
    if (st_init_tier(&a, 2, ST_TIER_S) != ST_OK || st_init_tier(&p, 3, ST_TIER_S) != ST_OK ||
        st_init_tier(&q, 4, ST_TIER_S) != ST_OK) { CHECK(0, "[cohort] init"); return; }
    const int n = a.n_params;
    a.mu[0] = 0.5f; a.vu[0] = 0.25f; a.adam_t = 7;    /* so that "unchanged" means it */
    const uint64_t h0 = model_hash(&a);

    const int at[] = { a.o_out + 'e' * a.d, a.o_embed + 255 * a.d, n - 1 };
    static const uint32_t pat[] = { 0x7FC00000u, 0x7F800000u, 0xFF800000u };
    int bad = 0;
    for (unsigned k = 0; k < 3; k++)
        for (unsigned r = 0; r < 3; r++) {
            float keep = p.w[at[k]];
            p.w[at[k]] = from_u(pat[r]);
            int acc = cohort_with(&a, &p);
            if (acc != 0 || model_hash(&a) != h0) {
                printf("    peer element %d pattern 0x%08X: accepted %d, non-finite in the child %d\n",
                       at[k], pat[r], acc, nonfinite_f(a.w, n));
                bad++;
            }
            p.w[at[k]] = keep;
            if (model_hash(&a) != h0) break;      /* the rest would say nothing */
        }
    CHECK(bad == 0, "[cohort-nonfinite] a peer with NaN or Inf, used row, unused row, last element: skipped, all bytes the same");

    /* an overflow from finite parts: the mean would be Inf */
    if (model_hash(&a) == h0) {
        float ka = a.w[5], kp = p.w[5];
        a.w[5] = 3e38f; p.w[5] = 3e38f;
        uint64_t h = model_hash(&a);
        int acc = cohort_with(&a, &p);
        printf("    an overflowing sum: accepted %d, non-finite in the child %d\n", acc, nonfinite_f(a.w, n));
        CHECK(acc == 0 && model_hash(&a) == h, "[cohort-overflow] a sum that overflows is refused, all bytes the same");
        a.w[5] = ka; p.w[5] = kp;
    } else {
        CHECK(0, "[cohort-overflow] the model had already changed");
    }

    /* the model itself: nothing to average */
    if (model_hash(&a) == h0) {
        float ka = a.w[n - 1];
        a.w[n - 1] = from_u(0x7FC00000u);
        uint64_t h = model_hash(&a);
        int acc = cohort_with(&a, &p);
        CHECK(acc == 0 && model_hash(&a) == h, "[cohort-into] a non-finite model is not merged into");
        a.w[n - 1] = ka;
    } else {
        CHECK(0, "[cohort-into] the model had already changed");
    }

    /* two peers, the first poisoned: the mean is over the good one, the same bits */
    {
        st_model b1, b2;
        size_t lg = 0, lb = 0;
        void *good = blob_of(&q, &lg);
        float keep = p.w[n - 1];
        p.w[n - 1] = from_u(0x7F800000u);
        void *badb = blob_of(&p, &lb);
        p.w[n - 1] = keep;
        if (!good || !badb || st_init_tier(&b1, 2, ST_TIER_S) != ST_OK ||
            st_init_tier(&b2, 2, ST_TIER_S) != ST_OK) { CHECK(0, "[cohort-mixed] init"); return; }
        const void *one[1] = { good }; size_t ol[1] = { lg };
        const void *two[2] = { badb, good }; size_t tl[2] = { lb, lg };
        int a1 = st_merge_cohort(&b1, one, ol, 1);
        int a2 = st_merge_cohort(&b2, two, tl, 2);
        printf("    good peer alone: accepted %d; bad and good: accepted %d\n", a1, a2);
        CHECK(a1 == 1 && a2 == 1 && model_hash(&b1) == model_hash(&b2),
              "[cohort-mixed] of a bad and a good peer the good one is averaged, same bits as alone");
        free(good); free(badb); st_free(&b1); st_free(&b2);
    }

    /* more peers than the mask can hold: an error, not a silent drop */
    {
        const void *many[ST_COHORT_MAX + 1];
        size_t ml[ST_COHORT_MAX + 1];
        size_t l = 0;
        void *g = blob_of(&q, &l);
        for (int k = 0; k <= ST_COHORT_MAX; k++) { many[k] = g; ml[k] = l; }
        uint64_t h = model_hash(&a);
        int acc = st_merge_cohort(&a, many, ml, ST_COHORT_MAX + 1);
        CHECK(acc == ST_E_ARG && model_hash(&a) == h, "[cohort-too-many] more peers than ST_COHORT_MAX is an error");
        free(g);
    }

    /* the control: a finite merge is the plain mean, bit for bit, and resets Adam */
    {
        float *w0 = (float *)malloc((size_t)n * sizeof(float));
        memcpy(w0, a.w, (size_t)n * sizeof(float));
        int acc = cohort_with(&a, &p);
        int diff = 0;
        for (int i = 0; i < n; i++) {
            float want = (w0[i] + p.w[i]) / 2.0f;
            if (to_u(a.w[i]) != to_u(want)) diff++;
        }
        printf("    clean merge: accepted %d, %d of %d elements differ from the mean\n", acc, diff, n);
        CHECK(acc == 1 && diff == 0 && a.adam_t == 0 && a.mu[0] == 0.0f,
              "[cohort-clean] finite peers: the plain mean, every bit, Adam reset");
        free(w0);
    }
    st_free(&a); st_free(&p); st_free(&q);
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
    CHECK(w1 == 1 && w2 == 1 && g_cap_bad == b0 && student_nonfinite_refused() == 0,
          "[gate-clean] a clean baby is still written, twice, nothing refused");

    /* the first save has no loss to compare with */
    fresh(); b0 = g_cap_bad;
    poison(g_student.w + g_student.o_out + 'e' * g_student.d);
    w = tick();
    printf("    NaN, first save: writes %u (non-finite %u), held %f\n", w, g_cap_bad - b0,
           (double)student_dmn_heldout_loss());
    CHECK(g_cap_bad == b0, "[gate-first] a NaN baby is not written on the first save");
    CHECK(student_nonfinite_refused() >= 1, "[gate-refused-count] the refusal is counted");

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

    /* no tick here: nothing moves a poison from the Adam state into w */
    fresh(); b0 = g_cap_bad; unsigned c0 = g_cap_count;
    poison(g_student.mu + g_student.o_embed + 255 * g_student.d);
    int rm = student_persist(quiet);
    fresh();
    poison(g_student.vu + g_student.o_embed + 255 * g_student.d);
    int rv = student_persist(quiet);
    fresh();
    poison(g_student.w + g_student.n_params - 1);
    int rl = student_persist(quiet);
    printf("    student_persist, NaN in mu %d, in vu %d, in the last weight %d, writes %u\n",
           rm, rv, rl, g_cap_count - c0);
    CHECK(rm != 0 && rv != 0 && rl != 0 && g_cap_count == c0 && g_cap_bad == b0,
          "[gate-persist-adam] mu, vu, the last weight: no write, with no tick to carry it");
    fresh();
}

/* the probe a person can run says it; a clean run prints no such line */
static char g_said[1024];
static void said(const char *s)
{
    size_t n = strlen(g_said);
    snprintf(g_said + n, sizeof g_said - n, "%s", s);
}
static void refused_before(void)
{
    fresh(); g_said[0] = 0;
    cradle_live_probe(said);
    CHECK(strstr(g_said, "saves refused") == NULL, "[refused-silent] nothing refused, no line");
}
static void refused_after(void)
{
    char want[64];
    g_said[0] = 0;
    cradle_live_probe(said);
    snprintf(want, sizeof want, "%u saves refused", student_nonfinite_refused());
    printf("    the probe says: %s", strstr(g_said, "[baby]") ? strstr(g_said, "[baby]") : "(nothing)\n");
    CHECK(student_nonfinite_refused() > 0 && strstr(g_said, want) != NULL,
          "[refused-said] the probe says how many saves were refused");
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
    if (argc > 1 && strcmp(argv[1], "merge") == 0) {
        printf("[nan] the merge\n");   guarded_checks(); load_checks(); cohort_checks();
    } else if (argc > 1 && strcmp(argv[1], "ema") == 0) {
        printf("[nan] the saved copy\n");
        ema_checks();
    } else {
        printf("[nan] st_logf\n");     logf_checks();
        printf("[nan] the loss\n");    loss_checks();
        printf("[nan] the check\n");   finite_checks();
        printf("[nan] the merge\n");   guarded_checks(); load_checks(); cohort_checks();
        printf("[nan] the save\n");    refused_before(); gate_checks(); refused_after();
    }
    printf("[result] %s (%d pass, %d fail)\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
