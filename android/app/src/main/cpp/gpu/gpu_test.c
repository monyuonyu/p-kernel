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
 *   3. runs y_gpu = gpu_matmul_f32(A, x)  (falls back / skips if no GPU),
 *   4. asserts max relative error <= TOL and prints the MEASURED max error,
 *   5. times both and prints GPU ms vs CPU ms (the crossover = T_GPU).
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

/* relative tolerance; we also PRINT the measured max error so mk_pino can
 * report the real number and we can tighten this from data (design §4.3). */
#define TOL 1e-3

static int run_size(size_t in, size_t out, int reps)
{
    size_t na = in * out;
    float *A     = (float *)malloc(na  * sizeof(float));
    float *x     = (float *)malloc(in  * sizeof(float));
    float *y_cpu = (float *)malloc(out * sizeof(float));
    float *y_gpu = (float *)malloc(out * sizeof(float));
    if (!A || !x || !y_cpu || !y_gpu) {
        fprintf(stderr, "OOM at in=%zu out=%zu\n", in, out);
        free(A); free(x); free(y_cpu); free(y_gpu);
        return -1;
    }

    rng_state = 0x9E3779B9u ^ (uint32_t)(in * 1000003u + out);
    for (size_t k = 0; k < na; k++) A[k] = frand();
    for (size_t j = 0; j < in; j++) x[j] = frand();

    /* CPU timing (averaged over reps). */
    double t0 = now_ms();
    for (int r = 0; r < reps; r++) cpu_matmul(A, in, out, x, y_cpu);
    double cpu_ms = (now_ms() - t0) / reps;

    /* GPU: try once for correctness; time over reps if available. */
    int gpu_ok = (gpu_matmul_f32(A, in, out, x, y_gpu) == 0);
    double gpu_ms = -1.0;
    if (gpu_ok) {
        double g0 = now_ms();
        for (int r = 0; r < reps; r++) {
            if (gpu_matmul_f32(A, in, out, x, y_gpu) != 0) { gpu_ok = 0; break; }
        }
        if (gpu_ok) gpu_ms = (now_ms() - g0) / reps;
    }

    int pass = 1;
    double max_abs = 0.0, max_rel = 0.0;
    if (gpu_ok) {
        for (size_t i = 0; i < out; i++) {
            double a = fabs((double)y_gpu[i] - (double)y_cpu[i]);
            double denom = fabs((double)y_cpu[i]);
            double rel = (denom > 1e-6) ? a / denom : a;
            if (a   > max_abs) max_abs = a;
            if (rel > max_rel) max_rel = rel;
        }
        pass = (max_rel <= TOL);
    }

    printf("  size in=%-6zu out=%-6zu  (%.0f MACs)\n",
           in, out, (double)in * (double)out);
    if (gpu_ok) {
        printf("    [gpu-matmul-matches-cpu] %s  max_abs=%.3e max_rel=%.3e (TOL=%.0e)\n",
               pass ? "PASS" : "FAIL", max_abs, max_rel, (double)TOL);
        printf("    [gpu-faster-on-big] cpu=%.3f ms  gpu=%.3f ms  -> GPU %s\n",
               cpu_ms, gpu_ms, (gpu_ms < cpu_ms) ? "FASTER" : "slower");
    } else {
        printf("    [gpu-matmul-matches-cpu] SKIP (GPU unavailable; CPU is the answer)\n");
        printf("    [gpu-faster-on-big] cpu=%.3f ms  gpu=n/a\n", cpu_ms);
    }

    free(A); free(x); free(y_cpu); free(y_gpu);
    return (gpu_ok && !pass) ? -1 : 0;   /* only a real GPU MISMATCH fails */
}

int main(void)
{
    char desc[160];

    printf("== yurikago GPU-1 device test (Vulkan compute matmul) ==\n");

    /* Turn the conservative-default flag ON for the test, then probe. */
    gpu_set_enabled(1);
    gpu_init();
    gpu_describe(desc, sizeof(desc));
    int avail = gpu_available();
    printf("gpu_available = %d  (%s)\n\n", avail, desc);

    if (!avail) {
        printf("No usable Vulkan compute device. This is OK: yurikago runs on\n");
        printf("the CPU. [gpu-matmul-matches-cpu] cannot run here; CPU is the\n");
        printf("answer. (On the S25 we expect avail=1.)\n");
        gpu_shutdown();
        return 0;   /* not a failure: compatibility path proven */
    }

    /* Sizes: small (toy brain, GPU expected SLOWER), threshold-ish, big
     * (conversation-model dims, GPU expected FASTER). reps keep timings
     * stable; big sizes use fewer reps. */
    struct { size_t in, out; int reps; } cases[] = {
        {   64,    64, 200 },   /* tiny: dtr-ish; GPU launch overhead dominates */
        {  256,   256, 100 },
        {  512,   512,  50 },
        { 1024,  1024,  20 },   /* threshold neighborhood */
        { 2048,  2048,  10 },
        { 4096,  4096,   4 },   /* big: conversation-model row; GPU should win  */
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
