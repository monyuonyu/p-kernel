/*
 *  student_math_test.c - [b4-math] the student's own sqrt, exp, log against
 *  libm, and Adam's step against its formula (ROADMAP B4, stage 1).
 *  Lines fixed before any run: docs/architecture/30-module/research/b4-numerics.md.
 *
 *  #includes student.c: st_sqrtf is file-local.
 *
 *   student_math_test <stride>     M1, M2, M4; stride over the bit patterns
 *   student_math_test hash         M3: one line, FNV-1a of the three recipes on
 *                                  a fixed table, to compare across machines
 *
 *  Build: -std=c11 -O1 -Wall -Wextra -ffp-contract=off (no -ffast-math).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "../../arch/common/llm/student.c"

/* stage 1 of the design: the eps Adam really had, said out loud. Stage 2 moves
 * this to 1e-8 with the learning rate. */
#ifndef WANT_ADAM_EPS
#define WANT_ADAM_EPS 0.015625
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

static float from_u(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }
static uint32_t to_u(float f)   { uint32_t u; memcpy(&u, &f, sizeof u); return u; }

static void m1_sqrt(uint64_t stride)
{
    double worst = 0.0, wx = 0.0;
    uint64_t n = 0;
    /* all positive floats, subnormals included */
    for (uint64_t u = 1; u < 0x7F800000ull; u += stride) {
        float x = from_u((uint32_t)u);
        double t = sqrt((double)x);
        double r = fabs((double)st_sqrtf(x) - t) / t;
        n++;
        if (r > worst) { worst = r; wx = x; }
    }
    printf("    sqrt: %llu inputs, worst relative error %.3e at %g\n",
           (unsigned long long)n, worst, wx);
    CHECK(worst <= 2.4e-7, "[m1-sqrt] st_sqrtf within 2.4e-7 on all positive floats");
    int edges = st_sqrtf(0.0f) == 0.0f && st_sqrtf(-0.0f) == 0.0f && st_sqrtf(-1.0f) == 0.0f &&
                st_sqrtf(from_u(0xFF800000u)) == 0.0f;
    float nn = st_sqrtf(from_u(0x7FC00000u));
    CHECK(edges && nn != nn, "[m1-edges] 0 and negatives give 0, NaN gives NaN");
    /* audit-116: the design's M1 did not say. libm's answer: Adam's denominator
     * turns infinite and the step 0, not a NaN weight (the old root gave NaN). */
    float inf = st_sqrtf(from_u(0x7F800000u));
    CHECK(to_u(inf) == 0x7F800000u, "[m1-inf] +Inf gives +Inf");
}

static void m2_exp_log(uint64_t stride)
{
    double we = 0.0, wex = 0.0, wa = 0.0, wax = 0.0, wr = 0.0, wrx = 0.0;
    for (uint64_t u = 0; u <= 0xFFFFFFFFull; u += stride) {
        float x = from_u((uint32_t)u);
        if (x >= -87.0f && x <= 88.0f) {
            double r = fabs((double)st_expf(x) / exp((double)x) - 1.0);
            if (r > we) { we = r; wex = x; }
        }
        if (x >= 1e-30f && x <= 3.4028235e38f) {
            double t = log((double)x), e = fabs((double)st_logf(x) - t);
            if (fabs(t) < 1.0) { if (e > wa) { wa = e; wax = x; } }
            else { double r = e / fabs(t); if (r > wr) { wr = r; wrx = x; } }
        }
    }
    printf("    exp: worst relative %.3e at %g; log: worst absolute %.3e at %g (|ln x| < 1), "
           "worst relative %.3e at %g\n", we, wex, wa, wax, wr, wrx);
    CHECK(we <= 5e-6, "[m2-exp] st_expf within 5e-6 on [-87, 88]");
    CHECK(wa <= 2e-6 && wr <= 2e-6, "[m2-log] st_logf within 2e-6 (absolute under |ln x| = 1, else relative)");
}

/* one step from a clean state: t = 1, so the corrected moments are mh = g and
 * vh = g^2, and the step is lr * g / (|g| + eps) */
static void m4_adam(void)
{
    static const float G[] = { 100.0f, 1.0f, 0.01f, 1e-4f, 1e-6f, -0.01f };
    enum { NG = sizeof G / sizeof G[0] };
    const float lr = 3e-3f;
    st_model m;
    if (st_init_tier(&m, 1, ST_TIER_S) != ST_OK) { CHECK(0, "[m4-adam] init"); return; }
    float w0[NG];
    for (int i = 0; i < m.n_params; i++) { m.g[i] = 0.0f; m.mu[i] = 0.0f; m.vu[i] = 0.0f; }
    for (int i = 0; i < NG; i++) { w0[i] = m.w[i]; m.g[i] = G[i]; }
    m.adam_t = 0;
    st_adam_step(&m, lr);
    int bad = 0;
    for (int i = 0; i < NG; i++) {
        double g = G[i], want = lr * g / (fabs(g) + WANT_ADAM_EPS);
        double got = (double)w0[i] - (double)m.w[i];
        double r = fabs(got / want - 1.0);
        printf("    g %-8g step %.6e want %.6e off by %.2f%%\n", g, got, want, r * 100.0);
        if (r > 0.01) bad++;
    }
    CHECK(bad == 0, "[m4-adam] one step is the formula within 1%");
    st_free(&m);
}

static void m3_hash(void)
{
    uint64_t h = 1469598103934665603ULL;
#define MIX(v) do { uint32_t b_ = to_u(v); \
        for (int k_ = 0; k_ < 4; k_++) { h ^= (b_ >> (8 * k_)) & 0xFF; h *= 1099511628211ULL; } } while (0)
    unsigned long n = 0;
    for (uint64_t u = 0; u <= 0xFFFFFFFFull; u += 8191) {
        float x = from_u((uint32_t)u);
        /* NaN results may differ in payload and sign by machine: skip them */
        if (x > 0.0f && x <= 3.4028235e38f) { MIX(st_sqrtf(x)); MIX(st_logf(x)); n += 2; }
        if (x >= -87.0f && x <= 88.0f)      { MIX(st_expf(x)); n++; }
    }
    printf("[b4-math] hash %016llx over %lu results\n", (unsigned long long)h, n);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "hash") == 0) { m3_hash(); return 0; }
    uint64_t stride = argc > 1 ? strtoull(argv[1], NULL, 0) : 17;
    if (stride < 1) stride = 1;
    printf("[b4-math] stride %llu\n", (unsigned long long)stride);
    m1_sqrt(stride);
    m2_exp_log(stride);
    m4_adam();
    printf("[result] %s (%d pass, %d fail)\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
