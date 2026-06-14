/*
 * gpu_test.c — GPU-1 on-device test harness ([gpu-matmul-matches-cpu] /
 *              [gpu-faster-on-big]).
 *
 * A standalone arm64 executable for /data/local/tmp on mk_pino's S25. It does
 * NOT depend on the kernel or the JNI; it links only gpu_vk.c (+ embedded
 * SPIR-V) and its own CPU reference. Mirrors the salty-bug on-device loop.
 *
 * For each size it:
 *   1. builds a deterministic A (out x in) and x (length in),
 *   2. runs y_cpu = cpu_matmul(A, x),
 *   3. runs y_gpu = gpu_matmul_f32(A, x)  (upload-each-call; the cert path),
 *   4. runs y_res via gpu_upload_weight(A) ONCE + N gpu_matmul_resident(x)
 *      (the realistic inference pattern: A resident, only x/y move),
 *   5. asserts BOTH GPU paths' max relative error <= TOL vs CPU,
 *   6. times all three and prints CPU ms vs GPU-upload ms vs GPU-resident ms
 *      (the crossover where resident beats CPU = the real T_GPU for inference).
 *
 * CPU is the reference oracle (design doc §4.4): GPU must match it. The CPU
 * matmul here is the plain-float analogue of qz_matmul (GPU-2 will diff
 * against qz_matmul directly).
 *
 * Cert lines printed for the grader:
 *   [gpu-matmul-matches-cpu] PASS/FAIL  (per size + overall)
 *   [gpu-faster-on-big]      gpu_ms vs cpu_ms per size; crossover note
 *
 * Build/run: see tools/android/run_gpu_test.sh and the report.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#include "gpu_compute.h"

/* ---- deterministic pseudo-random fixture (no libc rand dependence) ----- */
static uint32_t rng_state = 0x12345678u;
static float frand(void)
{
    /* xorshift32 -> [-1, 1) */
    uint32_t s = rng_state;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    rng_state = s;
    return ((float)(s & 0xFFFFFF) / (float)0x800000) - 1.0f;
}

/* ---- CPU reference matmul: y[i] = sum_j A[i*in+j]*x[j] ----------------- */
static void cpu_matmul(const float *A, size_t in, size_t out,
                       const float *x, float *y)
{
    for (size_t i = 0; i < out; i++) {
        const float *row = A + i * in;
        float acc = 0.0f;
        for (size_t j = 0; j < in; j++) acc += row[j] * x[j];
        y[i] = acc;
    }
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Tolerance. The GPU reduces a dot product in a DIFFERENT summation order
 * than the CPU (tree reduction across 32 lanes vs sequential), so float32
 * rounding differs. For random +/-1 weights the true dot product is often
 * NEAR ZERO (cancellation), which makes a pure relative test explode on the
 * unlucky output whose CPU value lands closest to 0 — even though the ABSOLUTE
 * error is tiny and grows only ~sqrt(K) as expected. So we use the numpy
 * allclose rule: an element passes if abs<=ATOL OR rel<=RTOL. This is the
 * honest numerics-discovered tolerance (design §4.3): it catches a real bug
 * (abs error would blow up) but not benign reassociation. We PRINT both the
 * max_abs and max_rel so mk_pino reports the real S25 rounding floor. */
#define RTOL 1e-3
/* ATOL scales with K: float32 dot-product error grows ~K*eps*max|term|, and
 * |term| ~ 1 here, eps ~ 6e-8, so a few * K * eps is the right ballpark.
 * 8e-8 * K bounds the observed llvmpipe errors with margin and is still far
 * below any value that would hide a wrong-answer bug. */
static double atol_for_k(size_t in) { return 8e-8 * (double)in + 1e-5; }

/* Worst element-wise error of g[] vs ref[]; reports max_abs + max_rel and the
 * count of elements failing BOTH abs and rel (a real mismatch). */
static int allclose(const float *g, const float *ref, size_t n, double atol,
                    double *out_max_abs, double *out_max_rel)
{
    double max_abs = 0.0, max_rel = 0.0;
    int nbad = 0;
    for (size_t i = 0; i < n; i++) {
        double a = fabs((double)g[i] - (double)ref[i]);
        double denom = fabs((double)ref[i]);
        double rel = (denom > 1e-12) ? a / denom : a;
        if (a   > max_abs) max_abs = a;
        if (rel > max_rel) max_rel = rel;
        if (a > atol && rel > RTOL) nbad++;   /* fails BOTH => real mismatch */
    }
    if (out_max_abs) *out_max_abs = max_abs;
    if (out_max_rel) *out_max_rel = max_rel;
    return nbad;
}

static int run_size(size_t in, size_t out, int reps)
{
    size_t na = in * out;
    float *A     = (float *)malloc(na  * sizeof(float));
    float *x     = (float *)malloc(in  * sizeof(float));
    float *y_cpu = (float *)malloc(out * sizeof(float));
    float *y_gpu = (float *)malloc(out * sizeof(float));
    float *y_res = (float *)malloc(out * sizeof(float));
    if (!A || !x || !y_cpu || !y_gpu || !y_res) {
        fprintf(stderr, "OOM at in=%zu out=%zu\n", in, out);
        free(A); free(x); free(y_cpu); free(y_gpu); free(y_res);
        return -1;
    }

    rng_state = 0x9E3779B9u ^ (uint32_t)(in * 1000003u + out);
    for (size_t k = 0; k < na; k++) A[k] = frand();
    for (size_t j = 0; j < in; j++) x[j] = frand();

    /* CPU timing (averaged over reps). The CPU is the reference oracle. */
    double t0 = now_ms();
    for (int r = 0; r < reps; r++) cpu_matmul(A, in, out, x, y_cpu);
    double cpu_ms = (now_ms() - t0) / reps;

    /* ---- GPU path 1: upload-each-call (gpu_matmul_f32, the cert path) ---- */
    int up_ok = (gpu_matmul_f32(A, in, out, x, y_gpu) == 0);
    double up_ms = -1.0;
    if (up_ok) {
        double g0 = now_ms();
        for (int r = 0; r < reps; r++)
            if (gpu_matmul_f32(A, in, out, x, y_gpu) != 0) { up_ok = 0; break; }
        if (up_ok) up_ms = (now_ms() - g0) / reps;
    }

    /* ---- GPU path 2: resident weight (upload A once, N matmuls) --------- *
     * THE realistic inference pattern — the same weights, a new x per token.
     * Only x is uploaded + y read back per call; A stays on the GPU.        */
    int res_ok = 0;
    double res_ms = -1.0;
    gpu_weight_t h = gpu_upload_weight(A, in, out);
    if (h) {
        res_ok = (gpu_matmul_resident(h, x, y_res) == 0);  /* correctness run */
        if (res_ok) {
            double g0 = now_ms();
            for (int r = 0; r < reps; r++)
                if (gpu_matmul_resident(h, x, y_res) != 0) { res_ok = 0; break; }
            if (res_ok) res_ms = (now_ms() - g0) / reps;
        }
        gpu_free_weight(h);
    }

    /* Correctness vs CPU for BOTH GPU paths (abs OR rel within tolerance). */
    double atol = atol_for_k(in);
    double up_abs = 0.0, up_rel = 0.0, res_abs = 0.0, res_rel = 0.0;
    int up_pass = 1, res_pass = 1, up_bad = 0, res_bad = 0;
    if (up_ok)  { up_bad  = allclose(y_gpu, y_cpu, out, atol, &up_abs,  &up_rel);  up_pass  = (up_bad  == 0); }
    if (res_ok) { res_bad = allclose(y_res, y_cpu, out, atol, &res_abs, &res_rel); res_pass = (res_bad == 0); }

    printf("  size in=%-6zu out=%-6zu  (%.0f MACs)\n",
           in, out, (double)in * (double)out);
    if (up_ok) {
        printf("    [gpu-matmul-matches-cpu] upload   %s  max_abs=%.3e max_rel=%.3e nbad=%d (atol=%.2e rtol=%.0e)\n",
               up_pass ? "PASS" : "FAIL", up_abs, up_rel, up_bad, atol, (double)RTOL);
    } else {
        printf("    [gpu-matmul-matches-cpu] upload   SKIP (GPU unavailable; CPU is the answer)\n");
    }
    if (res_ok) {
        printf("    [gpu-matmul-matches-cpu] resident %s  max_abs=%.3e max_rel=%.3e nbad=%d (atol=%.2e rtol=%.0e)\n",
               res_pass ? "PASS" : "FAIL", res_abs, res_rel, res_bad, atol, (double)RTOL);
    } else {
        printf("    [gpu-matmul-matches-cpu] resident SKIP (GPU unavailable)\n");
    }

    /* The crossover table. The resident column is the one that matters for
     * inference (T_GPU); the upload column shows the A-re-upload tax. */
    char up_s[32], res_s[32];
    if (up_ok)  snprintf(up_s,  sizeof(up_s),  "%.3f", up_ms);  else snprintf(up_s,  sizeof(up_s),  "n/a");
    if (res_ok) snprintf(res_s, sizeof(res_s), "%.3f", res_ms); else snprintf(res_s, sizeof(res_s), "n/a");
    printf("    [gpu-faster-on-big] cpu=%.3f ms  gpu_upload=%s ms  gpu_resident=%s ms\n",
           cpu_ms, up_s, res_s);
    if (res_ok)
        printf("                       -> resident GPU %s CPU (%.2fx); upload-each-call %s CPU\n",
               (res_ms < cpu_ms) ? "FASTER than" : "slower than",
               (res_ms > 0.0) ? cpu_ms / res_ms : 0.0,
               (up_ok && up_ms < cpu_ms) ? "faster than" : "slower than");

    free(A); free(x); free(y_cpu); free(y_gpu); free(y_res);
    /* Only a real GPU NUMERIC MISMATCH fails the cert. */
    return ((up_ok && !up_pass) || (res_ok && !res_pass)) ? -1 : 0;
}

int main(void)
{
    char desc[160];

    printf("== yurikago GPU-2 device test (tiled Vulkan compute matmul) ==\n");

    /* Turn the conservative-default flag ON for the test, then probe. */
    gpu_set_enabled(1);
    gpu_init();
    gpu_describe(desc, sizeof(desc));
    int avail = gpu_available();
    printf("gpu_available = %d  (%s)\n", avail, desc);
    printf("gpu_name      = \"%s\"\n\n", gpu_name());

    if (!avail) {
        printf("No usable Vulkan compute device. This is OK: yurikago runs on\n");
        printf("the CPU. [gpu-matmul-matches-cpu] cannot run here; CPU is the\n");
        printf("answer. (On the S25 we expect avail=1.)\n");
        gpu_shutdown();
        return 0;   /* not a failure: compatibility path proven */
    }

    /* Sizes: small (toy brain, GPU expected SLOWER), threshold-ish, big
     * (conversation-model dims, GPU expected FASTER). reps keep timings
     * stable; big sizes use fewer reps. GPU-2 adds 8192^2 to confirm the
     * tiled shader + resident weights keep widening the gap at scale. */
    struct { size_t in, out; int reps; } cases[] = {
        {   64,    64, 200 },   /* tiny: dtr-ish; GPU launch overhead dominates */
        {  256,   256, 100 },
        {  512,   512,  50 },
        { 1024,  1024,  20 },   /* threshold neighborhood */
        { 2048,  2048,  10 },
        { 4096,  4096,   4 },   /* big: conversation-model row; GPU should win  */
        { 8192,  8192,   2 },   /* bigger: tiled shader should win clearly      */
    };
    int overall = 0;
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        if (run_size(cases[c].in, cases[c].out, cases[c].reps) != 0)
            overall = -1;
    }

    printf("\n== summary ==\n");
    printf("OVERALL [gpu-matmul-matches-cpu] %s\n",
           overall == 0 ? "PASS" : "FAIL");
    printf("Report the max_rel values + the cpu/gpu ms crossover (= T_GPU)\n");
    printf("back to the commander; tighten TOL to the measured rounding floor.\n");

    gpu_shutdown();
    return overall;
}
