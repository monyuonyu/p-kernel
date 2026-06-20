/*
 *  mc1_test.c — MC-1 cert: wire the deterministic row-partitioned pool into the
 *  teacher matmuls behind the out*in SIZE GATE, and MEASURE the real speedup.
 *
 *  Subcommands (driven by tests/llm/run_mc1.sh):
 *
 *    equiv                synthetic teacher-scale F32 + Q4_0 matmuls: assert
 *                         byte-identical (memcmp + FNV hash) across NW{1,2,4,8},
 *                         incl. ragged out. (Q8_0's equiv is the MC-0 cert.)
 *                         [par-matmul-equiv]
 *
 *    speedup              wall-clock a teacher-scale matmul at NW{1,2,4,8};
 *                         report speedup + efficiency.  [par-matmul-speedup]
 *
 *    sweep                the CROSSOVER sweep: serial vs NW=4 across out*in from
 *                         2^12..2^22; print the empirical crossover MACs and the
 *                         regime where parallel LOSES.  [par-matmul-speedup]
 *
 *    gate                 assert the out*in gate routes a small (student d=128)
 *                         matmul SERIAL and a teacher-scale one PARALLEL, via
 *                         pk_parallel_last_was_parallel().  [par-matmul-gate]
 *
 *    idle                 the pool's wake-count does not advance across a 300ms
 *                         idle gap (no busy-spin regression).  [mc1-idle]
 *
 *  hash mode lives in mc1_hash.c (links forward.c+gguf.c — needs the GGUF).
 *
 *  Build: -O1 -ffp-contract=off, no VLA, LP64. Self-contained (no GGUF).
 */
#define _POSIX_C_SOURCE 200112L   /* setenv/unsetenv + clock_gettime */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quant.h"
#include "pk_parallel.h"

/* ---- fp32 -> fp16 RNE (for synthetic Q4_0/Q8_0 scales) ---------------- */
static uint16_t f32_to_f16(float f)
{
    union { float f; uint32_t u; } pun;
    pun.f = f;
    uint32_t x = pun.u;
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF) return (uint16_t)(sign | 0x7C00u | (mant ? 0x200u : 0));
    if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t half  = 1u << (shift - 1);
        uint32_t base  = mant >> shift;
        uint32_t rem   = mant & ((1u << shift) - 1u);
        if (rem > half || (rem == half && (base & 1u))) base++;
        return (uint16_t)(sign | base);
    }
    uint32_t base = mant >> 13;
    uint32_t rem  = mant & 0x1FFFu;
    uint32_t o    = ((uint32_t)exp << 10) | base;
    if (rem > 0x1000u || (rem == 0x1000u && (base & 1u))) o++;
    return (uint16_t)(sign | o);
}

/* ---- deterministic PRNG (xorshift) ----------------------------------- */
static uint32_t g_rng;
static void rng_seed(uint32_t s) { g_rng = s ? s : 0x1234567u; }
static uint32_t rng_u32(void) { uint32_t x = g_rng; x ^= x<<13; x ^= x>>17; x ^= x<<5; g_rng = x; return x; }
static float rng_f(void) { return (float)((int32_t)rng_u32()) / 2147483648.0f; }

/* FNV-1a over the logit buffer (identical scheme to student_shell.c:687). */
static uint64_t fnv1a(const float *y, size_t out)
{
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t *)y;
    size_t bytes = out * sizeof(float);
    for (size_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628253ULL; }
    return h;
}

/* ---- synthetic F32 weight matrix [out][in] + input x ------------------ */
static float *build_f32(size_t in, size_t out, float **x_out, uint32_t seed)
{
    float *w = (float *)malloc(in * out * sizeof(float));
    float *x = (float *)malloc(in * sizeof(float));
    if (!w || !x) { fprintf(stderr, "OOM\n"); exit(2); }
    rng_seed(seed);
    for (size_t i = 0; i < in * out; i++) w[i] = rng_f() * 0.1f;
    for (size_t j = 0; j < in; j++) x[j] = rng_f();
    *x_out = x;
    return w;
}

/* ---- synthetic Q4_0 weight matrix [out][in] + input x ----------------- */
static uint8_t *build_q4_0(size_t in, size_t out, float **x_out, uint32_t seed)
{
    const size_t nblk      = in / QK4_0;
    const size_t row_bytes = nblk * (2 + QK4_0/2);
    uint8_t *w = (uint8_t *)malloc(row_bytes * out);
    float   *x = (float *)malloc(in * sizeof(float));
    if (!w || !x) { fprintf(stderr, "OOM\n"); exit(2); }
    rng_seed(seed);
    for (size_t i = 0; i < out; i++) {
        uint8_t *row = w + i * row_bytes;
        for (size_t b = 0; b < nblk; b++) {
            uint8_t *blk = row + b * (2 + QK4_0/2);
            float d = 0.001f + 0.05f * (rng_u32() & 0xFFu) / 255.0f;
            uint16_t dh = f32_to_f16(d);
            blk[0] = (uint8_t)(dh & 0xFF);
            blk[1] = (uint8_t)(dh >> 8);
            for (int j = 0; j < QK4_0/2; j++) blk[2 + j] = (uint8_t)(rng_u32() & 0xFF);
        }
    }
    for (size_t j = 0; j < in; j++) x[j] = rng_f();
    *x_out = x;
    return w;
}

/* the F32 matmul as the production matmul_tensor runs it (routed via the gate).
 * We replicate the body+gate call here because matmul_tensor is static in
 * forward.c; the body is byte-identical to forward.c:f32_mm_body. */
struct f32_ctx { const float *wf; const float *x; float *y; size_t in; };
static void f32_body(void *vctx, size_t i0, size_t i1)
{
    const struct f32_ctx *c = (const struct f32_ctx *)vctx;
    const size_t in = c->in;
    for (size_t i = i0; i < i1; i++) {
        float acc = 0.0f;
        const float *row = c->wf + i * in;
        for (size_t j = 0; j < in; j++) acc += row[j] * c->x[j];
        c->y[i] = acc;
    }
}
static void f32_matmul_gated(const float *wf, size_t in, size_t out,
                             const float *x, float *y)
{
    struct f32_ctx ctx = { wf, x, y, in };
    pk_parallel_rows_gated(out, in, f32_body, &ctx);
}

static double now_wall_sec(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static double now_cpu_sec(void)
{
    struct timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ====================================================================== */
/* equiv: byte-identity across NW for F32 + Q4_0                          */
/* ====================================================================== */
static int cmd_equiv(void)
{
    int fails = 0;
    const int NWS[] = { 1, 2, 4, 8 };
    struct { size_t out, in; const char *name; } cases[] = {
        { 1536,  576, "ffn   (out=1536, in=576)" },
        { 49152, 576, "head  (out=49152, in=576)" },
        { 1530,  576, "ragged(out=1530, in=576)  [%4!=0 %8!=0]" },
        { 1031,  576, "ragged(out=1031, in=576)  [prime-ish]" },
    };

    /* force the gate OPEN for these teacher-scale shapes (all are >262144) but
     * be explicit so the test is independent of the default constant. */
    setenv("PKERNEL_MATMUL_MIN_MACS", "0", 1);

    printf("=== [par-matmul-equiv] F32 byte-identity across NW{1,2,4,8} ===\n");
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        size_t out = cases[c].out, in = cases[c].in;
        float *x = NULL;
        float *w = build_f32(in, out, &x, 0x1111u + (uint32_t)c);
        float *ys = malloc(out*sizeof(float)), *yp = malloc(out*sizeof(float));
        pk_parallel_set_threads(1);
        f32_matmul_gated(w, in, out, x, ys);
        uint64_t hs = fnv1a(ys, out);
        printf("  %-40s NW=1 hash=0x%016llx\n", cases[c].name, (unsigned long long)hs);
        for (size_t k = 1; k < 4; k++) {
            pk_parallel_set_threads(NWS[k]);
            f32_matmul_gated(w, in, out, x, yp);
            int bm = (memcmp(ys, yp, out*sizeof(float)) == 0);
            int hm = (fnv1a(yp, out) == hs);
            printf("      NW=%d memcmp=%s %s\n", NWS[k], bm?"EQUAL":"DIFFER", (bm&&hm)?"OK":"FAIL");
            if (!bm || !hm) fails++;
        }
        free(w); free(x); free(ys); free(yp);
    }

    printf("\n=== [par-matmul-equiv] Q4_0 byte-identity across NW{1,2,4,8} ===\n");
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        size_t out = cases[c].out, in = cases[c].in;
        float *x = NULL;
        uint8_t *w = build_q4_0(in, out, &x, 0x2222u + (uint32_t)c);
        float *ys = malloc(out*sizeof(float)), *yp = malloc(out*sizeof(float));
        pk_parallel_set_threads(1);
        if (qz_matmul_q4_0(w, in, out, x, ys) != 0) { fprintf(stderr,"q4 rc\n"); exit(2); }
        uint64_t hs = fnv1a(ys, out);
        printf("  %-40s NW=1 hash=0x%016llx\n", cases[c].name, (unsigned long long)hs);
        for (size_t k = 1; k < 4; k++) {
            pk_parallel_set_threads(NWS[k]);
            qz_matmul_q4_0(w, in, out, x, yp);
            int bm = (memcmp(ys, yp, out*sizeof(float)) == 0);
            int hm = (fnv1a(yp, out) == hs);
            printf("      NW=%d memcmp=%s %s\n", NWS[k], bm?"EQUAL":"DIFFER", (bm&&hm)?"OK":"FAIL");
            if (!bm || !hm) fails++;
        }
        free(w); free(x); free(ys); free(yp);
    }
    printf("\n[par-matmul-equiv] %s\n", fails==0?"PASS":"FAIL");
    return fails;
}

/* ====================================================================== */
/* speedup: wall-clock a teacher-scale matmul at NW{1,2,4,8}              */
/* ====================================================================== */
static int cmd_speedup(void)
{
    /* teacher output-head scale: out=49152, in=576 = 28.3M MACs */
    size_t out = 49152, in = 576;
    int    reps = 40;                 /* amortize timer noise */
    const int NWS[] = { 1, 2, 4, 8 };

    setenv("PKERNEL_MATMUL_MIN_MACS", "0", 1);   /* gate open for the measurement */

    float *x = NULL;
    float *w = build_f32(in, out, &x, 0xF00Du);
    float *y = malloc(out*sizeof(float));

    printf("=== [par-matmul-speedup] teacher-head F32 matmul "
           "out=%zu in=%zu (%.1fM MACs), %d reps ===\n",
           out, in, (double)(out*in)/1e6, reps);

    double t1 = 0.0;
    for (int k = 0; k < 4; k++) {
        int nw = NWS[k];
        pk_parallel_set_threads(nw);
        /* warm */
        f32_matmul_gated(w, in, out, x, y);
        double t0 = now_wall_sec();
        for (int r = 0; r < reps; r++) f32_matmul_gated(w, in, out, x, y);
        double dt = (now_wall_sec() - t0) / reps;
        if (nw == 1) t1 = dt;
        double sp  = t1 / dt;
        double eff = sp / (double)nw * 100.0;
        printf("  NW=%d  %.3f ms/matmul  speedup=%.2fx  efficiency=%.0f%%\n",
               nw, dt*1e3, sp, eff);
    }
    free(w); free(x); free(y);
    printf("[par-matmul-speedup] reported (honest wall-clock).\n");
    return 0;
}

/* ====================================================================== */
/* sweep: the crossover — serial vs NW=4 across out*in 2^12..2^22         */
/* ====================================================================== */
static int cmd_sweep(void)
{
    setenv("PKERNEL_MATMUL_MIN_MACS", "0", 1);   /* force pool to weigh raw cost */
    printf("=== [par-matmul-speedup] CROSSOVER SWEEP (serial vs NW=4) ===\n");
    printf("  in=%d fixed; out grows so out*in (MACs) ranges 2^12..2^22\n", 256);
    printf("  %-10s %-8s %-12s %-12s %-9s %s\n",
           "MACs", "out", "serial(us)", "NW=4(us)", "speedup", "verdict");

    size_t in = 256;                  /* 256 % 32 == 0, valid contraction */
    long crossover = -1;
    for (int e = 12; e <= 22; e++) {
        size_t macs = (size_t)1 << e;
        size_t out  = macs / in;
        if (out < 1) out = 1;
        float *x = NULL;
        float *w = build_f32(in, out, &x, 0xABBA0000u + (uint32_t)e);
        float *y = malloc(out*sizeof(float));
        int reps = (e < 16) ? 2000 : (e < 19 ? 300 : 60);

        pk_parallel_set_threads(1);
        f32_matmul_gated(w, in, out, x, y);                   /* warm */
        double s0 = now_wall_sec();
        for (int r = 0; r < reps; r++) f32_matmul_gated(w, in, out, x, y);
        double ts = (now_wall_sec() - s0) / reps;

        pk_parallel_set_threads(4);
        f32_matmul_gated(w, in, out, x, y);                   /* warm */
        double p0 = now_wall_sec();
        for (int r = 0; r < reps; r++) f32_matmul_gated(w, in, out, x, y);
        double tp = (now_wall_sec() - p0) / reps;

        double sp = ts / tp;
        /* Below PK_PARALLEL_MIN_ROWS (64) BOTH columns run the serial inline
         * body (out too small to dispatch), so any "win" there is sub-us timer
         * noise — exclude it from the crossover. The honest crossover is the
         * first shape large enough to dispatch where NW=4 actually wins. */
        int real = (out >= PK_PARALLEL_MIN_ROWS);
        const char *verdict = !real ? "serial(both)"
                            : (sp >= 1.0) ? "PAR WINS" : "serial wins";
        if (real && sp >= 1.0 && crossover < 0) crossover = (long)macs;
        printf("  2^%-2d=%-6zu %-8zu %-12.2f %-12.2f %-9.2f %s\n",
               e, macs, out, ts*1e6, tp*1e6, sp, verdict);
        free(w); free(x); free(y);
    }
    printf("\n  empirical crossover (first dispatchable MACs where NW=4 wins): %ld\n", crossover);
    printf("  gate constant in use (PK_PARALLEL_MIN_MACS / env): %zu\n",
           pk_parallel_min_macs());
    printf("  -> regime where PARALLEL LOSES: out*in below the crossover.\n");
    printf("[par-matmul-speedup] crossover sweep reported.\n");
    return 0;
}

/* ====================================================================== */
/* gate: small student matmul stays SERIAL, teacher matmul goes PARALLEL  */
/* ====================================================================== */
static int cmd_gate(void)
{
    int fails = 0;
    unsetenv("PKERNEL_MATMUL_MIN_MACS");      /* use the shipped default */
    pk_parallel_set_threads(4);
    printf("=== [par-matmul-gate] the size gate routes small=serial, big=parallel ===\n");
    printf("  default gate threshold: %zu MACs\n", pk_parallel_min_macs());

    struct { size_t out, in; int want_par; const char *what; } cs[] = {
        { 128,   128, 0, "student M-tier expert (d=128, dff=128)   16384 MACs" },
        { 256,   512, 0, "student L-tier expert (d=256, dff=512)  131072 MACs" },
        {  48,    48, 0, "R3 in-context matmul (48x48)              2304 MACs" },
        { 1536,  576, 1, "teacher ffn (out=1536, in=576)         884736 MACs" },
        { 49152, 576, 1, "teacher head (out=49152, in=576)       28.3M MACs" },
    };
    for (size_t i = 0; i < sizeof(cs)/sizeof(cs[0]); i++) {
        size_t out = cs[i].out, in = cs[i].in;
        float *x = NULL; float *w = build_f32(in, out, &x, 0x9000u+(uint32_t)i);
        float *y = malloc(out*sizeof(float));
        f32_matmul_gated(w, in, out, x, y);
        int par = pk_parallel_last_was_parallel();
        int ok = (par == cs[i].want_par);
        printf("  %-46s -> %-8s want=%-8s %s\n",
               cs[i].what, par?"PARALLEL":"SERIAL",
               cs[i].want_par?"PARALLEL":"SERIAL", ok?"OK":"FAIL");
        if (!ok) fails++;
        free(w); free(x); free(y);
    }
    printf("\n[par-matmul-gate] %s (small student + R3 confirmed SERIAL)\n",
           fails==0?"PASS":"FAIL");
    return fails;
}

/* ====================================================================== */
/* idle: no busy-spin between forwards                                    */
/* ====================================================================== */
static int cmd_idle(void)
{
    int fails = 0;
    setenv("PKERNEL_MATMUL_MIN_MACS", "0", 1);
    size_t out = 49152, in = 576;
    float *x = NULL; float *w = build_f32(in, out, &x, 0x1D1Eu);
    float *y = malloc(out*sizeof(float));
    pk_parallel_set_threads(4);
    f32_matmul_gated(w, in, out, x, y);                  /* warm + 1 dispatch */
    unsigned long wb = pk_parallel_wake_count();
    double cb = now_cpu_sec();
    struct timespec gap = { 0, 300*1000*1000 };
    nanosleep(&gap, NULL);
    double ca = now_cpu_sec();
    unsigned long wa = pk_parallel_wake_count();
    double gap_cpu = ca - cb;
    printf("=== [mc1-idle] pool blocks between forwards (no busy-spin) ===\n");
    printf("  wakes before=%lu after=%lu (delta in 300ms gap=%lu)\n", wb, wa, wa-wb);
    printf("  process CPU during idle gap: %.4f s\n", gap_cpu);
    int ok = (wa == wb) && (gap_cpu < 0.05);
    if (!ok) { printf("  FAIL: busy-spin regression\n"); fails++; }
    else printf("  PASS: pool slept (~0 CPU, no spurious wakes)\n");
    free(w); free(x); free(y);
    return fails;
}

int main(int argc, char **argv)
{
    const char *cmd = (argc >= 2) ? argv[1] : "all";
    int fails = 0;
    if (!strcmp(cmd, "equiv"))        fails += cmd_equiv();
    else if (!strcmp(cmd, "speedup")) fails += cmd_speedup();
    else if (!strcmp(cmd, "sweep"))   fails += cmd_sweep();
    else if (!strcmp(cmd, "gate"))    fails += cmd_gate();
    else if (!strcmp(cmd, "idle"))    fails += cmd_idle();
    else {
        fails += cmd_equiv();   printf("\n");
        fails += cmd_gate();    printf("\n");
        fails += cmd_idle();    printf("\n");
        cmd_speedup();          printf("\n");
        cmd_sweep();
    }
    printf("\n=== MC-1 %s result: %s (%d failure%s) ===\n",
           cmd, fails==0?"PASS":"FAIL", fails, fails==1?"":"s");
    return fails==0 ? 0 : 1;
}
