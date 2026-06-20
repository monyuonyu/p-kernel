/*
 *  mc0_test.c — MC-0 cert: deterministic row-partitioned parallel matmul.
 *
 *  Proves qz_matmul_q8_0 (re-expressed through pk_parallel_rows) is
 *  BYTE-IDENTICAL to the serial loop for worker counts NW in {1,2,4,8},
 *  including a NW that does NOT divide `out` (ragged remainder), on a real
 *  teacher-scale Q8_0 matmul. See docs/architecture/multicore-matmul-plan.md
 *  §4.1, §7.
 *
 *  Three sub-certs:
 *    [par-matmul-equiv]      memcmp(serial, par)==0 + FNV-1a logit hash match
 *                            across every NW (the GATE).
 *    [par-matmul-falsifier]  a debug REASSOCIATING variant (strided partial
 *                            sums summed in finish order) MUST FAIL memcmp —
 *                            proves the cert can SEE a rounding-order bug.
 *    [mc0-idle]              the pool's wake-count does not advance during an
 *                            idle gap between matmuls (workers BLOCK, never
 *                            spin); host CPU stays ~0 in the gap.
 *
 *  Build: -O1 -ffp-contract=off, no VLA, LP64. Self-contained (no GGUF).
 */
#define _POSIX_C_SOURCE 199309L      /* clock_gettime, nanosleep */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quant.h"
#include "pk_parallel.h"

/* ------------------------------------------------------------------ */
/* fp32 -> fp16 (round-to-nearest-even), to build synthetic Q8_0 scales. */
/* ------------------------------------------------------------------ */
static uint16_t f32_to_f16(float f)
{
    union { float f; uint32_t u; } pun;
    pun.f = f;
    uint32_t x = pun.u;
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;

    if (((x >> 23) & 0xFF) == 0xFF) {            /* inf / NaN */
        return (uint16_t)(sign | 0x7C00u | (mant ? 0x200u : 0));
    }
    if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u);   /* overflow -> inf */
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;             /* underflow -> 0  */
        mant |= 0x800000u;                                /* implicit bit    */
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t half  = 1u << (shift - 1);
        uint32_t base  = mant >> shift;                   /* RNE subnormal   */
        uint32_t rem   = mant & ((1u << shift) - 1u);
        if (rem > half || (rem == half && (base & 1u))) base++;
        return (uint16_t)(sign | base);
    }
    /* normal: round mantissa 23 -> 10 bits, RNE */
    uint32_t base = mant >> 13;
    uint32_t rem  = mant & 0x1FFFu;
    uint32_t out  = ((uint32_t)exp << 10) | base;
    if (rem > 0x1000u || (rem == 0x1000u && (base & 1u))) out++;
    return (uint16_t)(sign | out);
}

/* ------------------------------------------------------------------ */
/* deterministic PRNG (xorshift) so W and x are fixed-seed, reproducible */
/* ------------------------------------------------------------------ */
static uint32_t g_rng;
static void rng_seed(uint32_t s) { g_rng = s ? s : 0x1234567u; }
static uint32_t rng_u32(void)
{
    uint32_t x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_rng = x;
    return x;
}
/* int8 in [-127,127] */
static int8_t rng_i8(void) { return (int8_t)((int)(rng_u32() & 0xFF) - 128 + 1); }
/* float in [-1,1) */
static float rng_f(void) { return (float)((int32_t)rng_u32()) / 2147483648.0f; }

/* ------------------------------------------------------------------ */
/* build a synthetic Q8_0 weight matrix [out rows][in cols] + input x.   */
/* ------------------------------------------------------------------ */
static uint8_t *build_q8_0(size_t in, size_t out, float **x_out, uint32_t seed)
{
    const size_t nblk      = in / QK8_0;
    const size_t row_bytes = nblk * (2 + QK8_0);
    uint8_t *w = (uint8_t *)malloc(row_bytes * out);
    float   *x = (float *)malloc(in * sizeof(float));
    if (!w || !x) { fprintf(stderr, "OOM\n"); exit(2); }

    rng_seed(seed);
    for (size_t i = 0; i < out; i++) {
        uint8_t *row = w + i * row_bytes;
        for (size_t b = 0; b < nblk; b++) {
            uint8_t *blk = row + b * (2 + QK8_0);
            /* a varied but bounded scale so dequant values stay sane */
            float d = 0.001f + 0.05f * (rng_u32() & 0xFFu) / 255.0f;
            uint16_t dh = f32_to_f16(d);
            blk[0] = (uint8_t)(dh & 0xFF);
            blk[1] = (uint8_t)(dh >> 8);
            int8_t *q = (int8_t *)(blk + 2);
            for (int k = 0; k < QK8_0; k++) q[k] = rng_i8();
        }
    }
    for (size_t j = 0; j < in; j++) x[j] = rng_f();
    *x_out = x;
    return w;
}

/* FNV-1a over the logit buffer (identical scheme to student_shell.c:687). */
static uint64_t fnv1a(const float *y, size_t out)
{
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t *)y;
    size_t bytes = out * sizeof(float);
    for (size_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628253ULL; }
    return h;
}

/* ------------------------------------------------------------------ */
/* THE FALSIFIER: a reassociating Q8_0 matmul. Each output row is split    */
/* into NW STRIDED partial sums, each accumulated independently, then the  */
/* partials are added in slot order. This REASSOCIATES the left-fold ->     */
/* different rounding bits -> MUST differ from serial. (debug-only; proves  */
/* the byte-identity cert has teeth, per §4.1.)                            */
/* ------------------------------------------------------------------ */
static void qz_matmul_q8_0_reassoc(const uint8_t *w_data, size_t in, size_t out,
                                   const float *x, float *y, int nw)
{
    const size_t nblk      = in / QK8_0;
    const size_t row_bytes = nblk * (2 + QK8_0);
    if (nw < 2) nw = 2;

    for (size_t i = 0; i < out; i++) {
        const uint8_t *row = w_data + i * row_bytes;
        /* nw strided partial accumulators over the contraction dim */
        float part[8];
        for (int p = 0; p < nw; p++) part[p] = 0.0f;

        for (size_t b = 0; b < nblk; b++) {
            const uint8_t *blk = row + b * (2 + QK8_0);
            /* reuse the engine's exact fp16 decode for the scale */
            const float d = qz_fp16_to_fp32((uint16_t)(blk[0] | (blk[1] << 8)));
            const int8_t *q = (const int8_t *)(blk + 2);
            for (int k = 0; k < QK8_0; k++) {
                size_t col = b * QK8_0 + (size_t)k;
                part[col % (size_t)nw] += (d * (float)q[k]) * x[col];
            }
        }
        /* sum partials in slot order — a DIFFERENT reduction tree than the
         * serial left-fold; the rounding bits will not match. */
        float acc = 0.0f;
        for (int p = 0; p < nw; p++) acc += part[p];
        y[i] = acc;
    }
}

/* run qz_matmul_q8_0 at a forced worker count and return its hash */
static uint64_t run_nw(const uint8_t *w, size_t in, size_t out, const float *x,
                       float *y, int nw)
{
    pk_parallel_set_threads(nw);
    int rc = qz_matmul_q8_0(w, in, out, x, y);
    if (rc != 0) { fprintf(stderr, "qz_matmul_q8_0 rc=%d\n", rc); exit(2); }
    return fnv1a(y, out);
}

static double now_cpu_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void)
{
    int fails = 0;

    /* ---- shapes ---- */
    /* ffn-scale: out=1536, in=576 (576 % 32 == 0). */
    /* head-scale: out=49152, in=576. ragged: out=1530 (not /4, not /8). */
    struct { size_t out, in; const char *name; } cases[] = {
        { 1536,  576, "ffn   (out=1536, in=576)" },
        { 49152, 576, "head  (out=49152, in=576)" },
        { 1530,  576, "ragged(out=1530, in=576)  [out %4!=0, %8!=0]" },
        { 1031,  576, "ragged(out=1031, in=576)  [prime-ish remainder]" },
    };
    const int NWS[] = { 1, 2, 4, 8 };

    printf("=== [par-matmul-equiv] byte-identity across NW in {1,2,4,8} ===\n");
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        size_t out = cases[c].out, in = cases[c].in;
        float *x = NULL;
        uint8_t *w = build_q8_0(in, out, &x, 0xABCDEF01u + (uint32_t)c);

        float *y_ser = (float *)malloc(out * sizeof(float));
        float *y_par = (float *)malloc(out * sizeof(float));

        uint64_t h_ser = run_nw(w, in, out, x, y_ser, 1);      /* serial ref */
        printf("  %-44s NW=1 hash=0x%016llx\n",
               cases[c].name, (unsigned long long)h_ser);

        for (size_t k = 1; k < sizeof(NWS) / sizeof(NWS[0]); k++) {
            int nw = NWS[k];
            uint64_t h = run_nw(w, in, out, x, y_par, nw);
            int bytematch = (memcmp(y_ser, y_par, out * sizeof(float)) == 0);
            int hashmatch = (h == h_ser);
            printf("      NW=%d  memcmp=%s  hash=0x%016llx  %s\n",
                   nw, bytematch ? "EQUAL" : "DIFFER",
                   (unsigned long long)h,
                   (bytematch && hashmatch) ? "OK" : "FAIL");
            if (!bytematch || !hashmatch) fails++;
        }
        free(w); free(x); free(y_ser); free(y_par);
    }

    /* ---- THE FALSIFIER: the reassociating variant MUST FAIL ---- */
    printf("\n=== [par-matmul-falsifier] reassociating variant MUST DIFFER ===\n");
    {
        size_t out = 1536, in = 576;
        float *x = NULL;
        uint8_t *w = build_q8_0(in, out, &x, 0x5A5A5A5Au);
        float *y_ser = (float *)malloc(out * sizeof(float));
        float *y_bad = (float *)malloc(out * sizeof(float));

        run_nw(w, in, out, x, y_ser, 1);            /* the true serial result */

        int any_caught = 0;
        for (size_t k = 1; k < sizeof(NWS) / sizeof(NWS[0]); k++) {
            int nw = NWS[k];
            qz_matmul_q8_0_reassoc(w, in, out, x, y_bad, nw);
            int differ = (memcmp(y_ser, y_bad, out * sizeof(float)) != 0);
            printf("  reassoc NW=%d  vs serial: %s  (cert wants DIFFER)\n",
                   nw, differ ? "DIFFER -> cert has teeth" : "EQUAL  -> VACUOUS");
            if (differ) any_caught = 1;
        }
        if (!any_caught) {
            printf("  FAIL: the falsifier was byte-identical to serial -> the "
                   "cert is VACUOUS, it cannot see a rounding-order bug.\n");
            fails++;
        } else {
            printf("  PASS: the cert distinguishes a reassociated reduction "
                   "(proves byte-identity is a real, killable property).\n");
        }
        free(w); free(x); free(y_ser); free(y_bad);
    }

    /* ---- [mc0-idle]: workers BLOCK between matmuls (no spin) ---- */
    printf("\n=== [mc0-idle] worker pool blocks between matmuls (no spin) ===\n");
    {
        size_t out = 1536, in = 576;
        float *x = NULL;
        uint8_t *w = build_q8_0(in, out, &x, 0x0FF1CE00u);
        float *y = (float *)malloc(out * sizeof(float));

        /* warm the pool + one dispatch at NW=4 */
        run_nw(w, in, out, x, y, 4);
        unsigned long wakes_before = pk_parallel_wake_count();
        double cpu_before = now_cpu_sec();

        /* IDLE GAP: ~300ms wall-clock with NO matmul. A spinning pool would
         * burn CPU here and advance the wake counter; a blocked pool will not. */
        struct timespec gap = { 0, 300 * 1000 * 1000 };
        nanosleep(&gap, NULL);

        double cpu_after = now_cpu_sec();
        unsigned long wakes_after = pk_parallel_wake_count();
        double gap_cpu = cpu_after - cpu_before;

        /* run a few more matmuls so wakes advancing on REAL work is shown */
        for (int r = 0; r < 5; r++) run_nw(w, in, out, x, y, 4);
        unsigned long wakes_work = pk_parallel_wake_count();

        printf("  wakes: before-gap=%lu  after-gap=%lu  (delta in gap=%lu)\n",
               wakes_before, wakes_after, wakes_after - wakes_before);
        printf("  process CPU consumed during 300ms idle gap: %.4f s\n", gap_cpu);
        printf("  wakes after 5 more matmuls: %lu (advances on REAL work)\n",
               wakes_work);

        /* gate: no wakes during the gap, and CPU in the gap stays tiny. */
        int idle_ok = (wakes_after == wakes_before) && (gap_cpu < 0.05);
        if (!idle_ok) {
            printf("  FAIL: pool consumed CPU / woke while idle -> busy-spin "
                   "regression (would burn a phone battery).\n");
            fails++;
        } else {
            printf("  PASS: pool blocked on the condvar while idle "
                   "(~0 CPU, no spurious wakes).\n");
        }
        free(w); free(x); free(y);
    }

    printf("\n=== MC-0 result: %s (%d failure%s) ===\n",
           fails == 0 ? "PASS" : "FAIL", fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
