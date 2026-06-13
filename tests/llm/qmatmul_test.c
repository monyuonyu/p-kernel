/*
 *  qmatmul_test.c — host harness for arch/common/llm/quant.c (milestone M1b).
 *
 *  Three things, in order of how much they prove:
 *
 *   (1) SYNTHETIC CERT (always runs, no network, no model file):
 *       hand-build Q8_0 and Q4_0 blocks with KNOWN scale d and KNOWN quants,
 *       so the exact float y = W·x can be written down INDEPENDENTLY (by a
 *       second, differently-structured reference loop in this file that
 *       materializes the whole dequantized matrix first, then does a textbook
 *       matmul). Assert qz_matmul_* matches the reference to a tight tolerance.
 *       Also a fp16->fp32 spot cert against known bit patterns. Exit 0 = PASS.
 *
 *   (2) REAL-TENSOR RUN (optional, if argv[1] is a .gguf path):
 *       pull a real Q8_0 weight matrix out of the model via the M1a loader,
 *       multiply by a FIXED pseudo-random x, time it, print y stats. Then dump
 *       the exact (tensor name, in, out, x[], y[]) to a sidecar file so the
 *       INDEPENDENT python oracle (qmatmul_oracle.py) can re-read the SAME GGUF
 *       block bytes, recompute y from scratch, and diff. (This binary does NOT
 *       grade itself against the real tensor — the oracle does.)
 *
 *  Usage:
 *      ./qmatmul_test                       # synthetic cert only
 *      ./qmatmul_test model.gguf            # real-tensor run + dump + synth cert
 *      ./qmatmul_test model.gguf out.txt    # also choose the dump path
 */
#define _GNU_SOURCE
#include "../../arch/common/llm/gguf.h"
#include "../../arch/common/llm/quant.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static int g_fails = 0;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  PASS  %s\n", (msg)); }                     \
        else      { printf("  FAIL  %s\n", (msg)); g_fails++; }          \
    } while (0)

/* ------------------------------------------------------------------ */
/* fixed pseudo-random float vector (xorshift, deterministic across runs */
/* and re-derivable by the python oracle from the same seed)            */
/* ------------------------------------------------------------------ */
static uint32_t prng_state;
static void prng_seed(uint32_t s) { prng_state = s ? s : 0x9E3779B9u; }
static uint32_t prng_u32(void)
{
    uint32_t x = prng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    prng_state = x;
    return x;
}
/* float in [-1, 1), 24-bit mantissa from the top bits (matches oracle) */
static float prng_f(void)
{
    uint32_t r = prng_u32() >> 8;          /* 24 bits                     */
    return (float)r / 8388608.0f - 1.0f;   /* [0,2) - 1 = [-1,1)          */
}

/* ------------------------------------------------------------------ */
/* fp16 helpers for building synthetic blocks                          */
/* round-to-nearest-even fp32 -> fp16 (only used on exact-ish values).  */
/* ------------------------------------------------------------------ */
static uint16_t f32_to_f16(float f)
{
    union { float f; uint32_t u; } pun; pun.f = f;
    uint32_t x = pun.u;
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;

    if (exp <= 0) {
        /* (synthetic test only ever uses well-scaled values; clamp to 0) */
        return (uint16_t)sign;
    }
    if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u);   /* inf */
    /* round mantissa 23 -> 10 bits, round-to-nearest-even */
    uint32_t m = mant >> 13;
    uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (m & 1u))) {
        m++;
        if (m == 0x400u) { m = 0; exp++; if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u); }
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | m);
}

static void put_u16le(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }

/* ------------------------------------------------------------------ */
/* INDEPENDENT reference: materialize the full dequantized matrix, then */
/* do a textbook matmul. Structured deliberately UNLIKE quant.c (it     */
/* dequantizes everything up front into a float buffer, row-major, and  */
/* multiplies separately) so a shared bug is unlikely to hide in both.  */
/* ------------------------------------------------------------------ */
static float ref_fp16(uint16_t h)   /* a 2nd, independent fp16 decode  */
{
    int sign = (h & 0x8000) ? -1 : 1;
    int exp  = (h >> 10) & 0x1F;
    int mant = h & 0x3FF;
    if (exp == 0) {
        if (mant == 0) return sign * 0.0f;
        /* subnormal: value = mant * 2^-24 */
        return (float)sign * (float)mant * 0.000000059604645f; /* 2^-24 */
    }
    if (exp == 0x1F) return mant ? (sign * (0.0f/0.0f)) : (sign * (1.0f/0.0f));
    /* normal: (1 + mant/1024) * 2^(exp-15) — compute 2^e by a loop      */
    float frac = 1.0f + (float)mant / 1024.0f;
    int e = exp - 15;
    float scale = 1.0f;
    if (e >= 0) for (int i = 0; i < e; i++) scale *= 2.0f;
    else        for (int i = 0; i < -e; i++) scale *= 0.5f;
    return (float)sign * frac * scale;
}

static void ref_dequant_q8_0(const uint8_t *w, size_t in, size_t out, float *M)
{
    size_t nblk = in / 32;
    for (size_t i = 0; i < out; i++) {
        const uint8_t *row = w + i * nblk * 34;
        for (size_t b = 0; b < nblk; b++) {
            const uint8_t *blk = row + b * 34;
            float d = ref_fp16((uint16_t)(blk[0] | (blk[1] << 8)));
            for (int k = 0; k < 32; k++) {
                int8_t q = (int8_t)blk[2 + k];
                M[i * in + b * 32 + k] = d * (float)q;
            }
        }
    }
}
static void ref_dequant_q4_0(const uint8_t *w, size_t in, size_t out, float *M)
{
    size_t nblk = in / 32;
    for (size_t i = 0; i < out; i++) {
        const uint8_t *row = w + i * nblk * 18;
        for (size_t b = 0; b < nblk; b++) {
            const uint8_t *blk = row + b * 18;
            float d = ref_fp16((uint16_t)(blk[0] | (blk[1] << 8)));
            const uint8_t *qs = blk + 2;
            for (int j = 0; j < 16; j++) {
                M[i * in + b * 32 + j]      = d * (float)((qs[j] & 0x0F) - 8);
                M[i * in + b * 32 + j + 16] = d * (float)((qs[j] >> 4)   - 8);
            }
        }
    }
}
static void ref_matmul(const float *M, size_t in, size_t out,
                       const float *x, float *y)
{
    for (size_t i = 0; i < out; i++) {
        float acc = 0.0f;
        for (size_t j = 0; j < in; j++) acc += M[i * in + j] * x[j];
        y[i] = acc;
    }
}

static float maxabs_err(const float *a, const float *b, size_t n)
{
    float m = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float e = a[i] - b[i]; if (e < 0) e = -e;
        if (e > m) m = e;
    }
    return m;
}

/* ------------------------------------------------------------------ */
/* (1) synthetic cert                                                  */
/* ------------------------------------------------------------------ */
static int run_synthetic_cert(void)
{
    printf("\n=== SYNTHETIC CERT (hand-built blocks, independent reference) ===\n");

    /* fp16->fp32 spot checks against known bit patterns ------------- */
    CHECK(qz_fp16_to_fp32(0x0000) == 0.0f,          "fp16 0x0000 == 0");
    CHECK(qz_fp16_to_fp32(0x3C00) == 1.0f,          "fp16 0x3C00 == 1.0");
    CHECK(qz_fp16_to_fp32(0xC000) == -2.0f,         "fp16 0xC000 == -2.0");
    CHECK(qz_fp16_to_fp32(0x3800) == 0.5f,          "fp16 0x3800 == 0.5");
    CHECK(qz_fp16_to_fp32(0x4900) == 10.0f,         "fp16 0x4900 == 10.0");
    {   /* subnormal: smallest positive half = 2^-24 */
        float sub = qz_fp16_to_fp32(0x0001);
        CHECK(sub > 5.9e-8f && sub < 6.0e-8f,       "fp16 0x0001 ~ 2^-24");
    }

    /* build a small Q8_0 matrix: in=64 (2 blocks), out=5 ------------- */
    const size_t Q8_IN = 64, Q8_OUT = 5;
    size_t q8_nblk = Q8_IN / 32;
    size_t q8_bytes = Q8_OUT * q8_nblk * 34;
    uint8_t *q8 = (uint8_t *)calloc(1, q8_bytes);
    /* fill with deterministic but varied d and q values */
    for (size_t i = 0; i < Q8_OUT; i++) {
        uint8_t *row = q8 + i * q8_nblk * 34;
        for (size_t b = 0; b < q8_nblk; b++) {
            uint8_t *blk = row + b * 34;
            float d = 0.0125f * (float)(i + 1) + 0.05f * (float)b; /* fp16-representable-ish */
            put_u16le(blk, f32_to_f16(d));
            for (int k = 0; k < 32; k++)
                blk[2 + k] = (uint8_t)(int8_t)(((int)(i * 7 + b * 3 + k) % 200) - 100);
        }
    }
    /* input + outputs */
    float xq8[64], y_dut[5], y_ref[5];
    prng_seed(0xBADC0FFE);
    for (size_t j = 0; j < Q8_IN; j++) xq8[j] = prng_f();
    float *M8 = (float *)malloc(sizeof(float) * Q8_IN * Q8_OUT);
    ref_dequant_q8_0(q8, Q8_IN, Q8_OUT, M8);
    ref_matmul(M8, Q8_IN, Q8_OUT, xq8, y_ref);
    int rc8 = qz_matmul_q8_0(q8, Q8_IN, Q8_OUT, xq8, y_dut);
    CHECK(rc8 == 0, "qz_matmul_q8_0 returns 0");
    float e8 = maxabs_err(y_dut, y_ref, Q8_OUT);
    printf("  [q8_0] y_ref[0..4] = %.6f %.6f %.6f %.6f %.6f\n",
           y_ref[0], y_ref[1], y_ref[2], y_ref[3], y_ref[4]);
    printf("  [q8_0] max abs err vs independent ref = %.3e\n", (double)e8);
    CHECK(e8 < 1e-3f, "q8_0 matmul matches independent ref (< 1e-3)");
    /* in % 32 != 0 must be rejected */
    { float dummy; CHECK(qz_matmul_q8_0(q8, 63, 1, xq8, &dummy) == -1,
                         "q8_0 rejects in not multiple of 32"); }
    free(M8); free(q8);

    /* build a small Q4_0 matrix: in=32 (1 block), out=4 ------------- */
    const size_t Q4_IN = 32, Q4_OUT = 4;
    size_t q4_nblk = Q4_IN / 32;
    size_t q4_bytes = Q4_OUT * q4_nblk * 18;
    uint8_t *q4 = (uint8_t *)calloc(1, q4_bytes);
    for (size_t i = 0; i < Q4_OUT; i++) {
        uint8_t *row = q4 + i * q4_nblk * 18;
        for (size_t b = 0; b < q4_nblk; b++) {
            uint8_t *blk = row + b * 18;
            float d = 0.1f * (float)(i + 1);
            put_u16le(blk, f32_to_f16(d));
            for (int j = 0; j < 16; j++) {
                int lo = (int)((i * 5 + j) % 16);       /* 0..15 */
                int hi = (int)((i * 3 + j * 2 + 1) % 16);
                blk[2 + j] = (uint8_t)((lo & 0x0F) | ((hi & 0x0F) << 4));
            }
        }
    }
    float xq4[32], y4_dut[4], y4_ref[4];
    prng_seed(0x1234ABCD);
    for (size_t j = 0; j < Q4_IN; j++) xq4[j] = prng_f();
    float *M4 = (float *)malloc(sizeof(float) * Q4_IN * Q4_OUT);
    ref_dequant_q4_0(q4, Q4_IN, Q4_OUT, M4);
    ref_matmul(M4, Q4_IN, Q4_OUT, xq4, y4_ref);
    int rc4 = qz_matmul_q4_0(q4, Q4_IN, Q4_OUT, xq4, y4_dut);
    CHECK(rc4 == 0, "qz_matmul_q4_0 returns 0");
    float e4 = maxabs_err(y4_dut, y4_ref, Q4_OUT);
    printf("  [q4_0] y_ref[0..3] = %.6f %.6f %.6f %.6f\n",
           y4_ref[0], y4_ref[1], y4_ref[2], y4_ref[3]);
    printf("  [q4_0] max abs err vs independent ref = %.3e\n", (double)e4);
    CHECK(e4 < 1e-3f, "q4_0 matmul matches independent ref (< 1e-3)");
    free(M4); free(q4);

    return 0;
}

/* ------------------------------------------------------------------ */
/* (2) real-tensor run + dump for the python oracle                    */
/* ------------------------------------------------------------------ */
static int streq_gguf(gguf_str s, const char *c)
{
    size_t n = strlen(c);
    return s.len == n && memcmp(s.ptr, c, n) == 0;
}

static double now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static int run_real_tensor(const char *path, const char *dump_path)
{
    printf("\n=== REAL-TENSOR RUN (Q8_0 from %s) ===\n", path);
    gguf_file gf;
    int rc = gguf_open(&gf, path);
    if (rc != GGUF_OK) {
        fprintf(stderr, "  gguf_open: %s\n", gguf_strerror(rc));
        return 1;
    }

    /* pick a real Q8_0 weight. Prefer blk.0.ffn_down.weight ([1536,576]):
     * in=1536, out=576 — a fat reduction dim, good matmul exercise. Fall back
     * to the first Q8_0 tensor whose in-dim is a multiple of 32. */
    const gguf_tensor *t = NULL;
    for (uint64_t i = 0; i < gf.n_tensors; i++) {
        if (gf.tensors[i].type == GGML_TYPE_Q8_0 &&
            streq_gguf(gf.tensors[i].name, "blk.0.ffn_down.weight")) {
            t = &gf.tensors[i]; break;
        }
    }
    if (!t) {
        for (uint64_t i = 0; i < gf.n_tensors; i++)
            if (gf.tensors[i].type == GGML_TYPE_Q8_0 &&
                gf.tensors[i].n_dims == 2 && gf.tensors[i].dims[0] % 32 == 0) {
                t = &gf.tensors[i]; break;
            }
    }
    if (!t) { fprintf(stderr, "  no suitable Q8_0 tensor found\n"); gguf_close(&gf); return 1; }

    size_t in  = (size_t)t->dims[0];   /* reduction dim (in_features)  */
    size_t out = (size_t)t->dims[1];   /* rows (out_features)          */
    printf("  tensor: %.*s  [in=%zu, out=%zu]  nbytes=%llu\n",
           (int)t->name.len, t->name.ptr, in, out,
           (unsigned long long)t->nbytes);

    /* fixed pseudo-random x (seed reproduced by the oracle) */
    const uint32_t SEED = 0xC0FFEE11;
    float *x = (float *)malloc(sizeof(float) * in);
    float *y = (float *)malloc(sizeof(float) * out);
    prng_seed(SEED);
    for (size_t j = 0; j < in; j++) x[j] = prng_f();

    double t0 = now_ms();
    int mr = qz_matmul_q8_0(t->data, in, out, x, y);
    double t1 = now_ms();
    if (mr != 0) { fprintf(stderr, "  qz_matmul_q8_0 rc=%d\n", mr); free(x); free(y); gguf_close(&gf); return 1; }

    /* y stats */
    float ymin = y[0], ymax = y[0]; double ysum = 0.0;
    for (size_t i = 0; i < out; i++) {
        if (y[i] < ymin) ymin = y[i];
        if (y[i] > ymax) ymax = y[i];
        ysum += y[i];
    }
    printf("  y: min=%.6f max=%.6f mean=%.6f\n",
           ymin, ymax, (double)(ysum / (double)out));
    printf("  measured matmul time: %.3f ms (un-optimized plain C, no SIMD)\n",
           t1 - t0);

    /* dump for the independent python oracle: tensor name, seed, in, out,
     * and the full y[] (and x is re-derivable from SEED). */
    if (dump_path) {
        FILE *f = fopen(dump_path, "w");
        if (f) {
            fprintf(f, "tensor %.*s\n", (int)t->name.len, t->name.ptr);
            fprintf(f, "seed %u\n", SEED);
            fprintf(f, "in %zu\nout %zu\n", in, out);
            for (size_t i = 0; i < out; i++)
                fprintf(f, "%.9g\n", y[i]);
            fclose(f);
            printf("  dumped DUT y[] -> %s (for qmatmul_oracle.py)\n", dump_path);
        } else {
            fprintf(stderr, "  WARN could not write dump %s\n", dump_path);
        }
    }

    free(x); free(y);
    gguf_close(&gf);
    return 0;
}

int main(int argc, char **argv)
{
    const char *gguf_path = (argc >= 2) ? argv[1] : NULL;
    const char *dump_path = (argc >= 3) ? argv[2] : "/tmp/pkernel_qmatmul_dut.txt";

    int real_rc = 0;
    if (gguf_path) real_rc = run_real_tensor(gguf_path, dump_path);

    int syn_rc = run_synthetic_cert();

    printf("\n==================================\n");
    if (g_fails == 0 && syn_rc == 0 && real_rc == 0) {
        printf("RESULT: PASS (synthetic cert; real run dumped for python oracle)\n");
        return 0;
    }
    printf("RESULT: FAIL (%d assertion failures%s)\n",
           g_fails ? g_fails : 1, real_rc ? ", real run errored" : "");
    return 1;
}
