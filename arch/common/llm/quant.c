/*
 *  quant.c — dequantize-while-you-matmul (milestone M1b).
 *  See quant.h for the contract, the ggml weight convention, and the verified
 *  Q8_0 / Q4_0 block geometry. Correctness-first plain C; no SIMD, no malloc.
 *
 *  Build (wave-49 rule, one math everywhere): -O1 -ffp-contract=off.
 */
#include "quant.h"

/* ------------------------------------------------------------------ */
/* fp16 -> fp32 (IEEE-754 binary16). Pure integer/bit work, libc-free. */
/* ------------------------------------------------------------------ */
float qz_fp16_to_fp32(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exp  = (h >> 10) & 0x1Fu;
    const uint32_t mant =  h        & 0x3FFu;
    uint32_t bits;

    if (exp == 0) {
        if (mant == 0) {
            bits = sign;                       /* +/- zero                  */
        } else {
            /* subnormal half: normalize into a float exponent             */
            uint32_t m = mant;
            int e = -1;
            do { m <<= 1; e++; } while ((m & 0x400u) == 0);
            m &= 0x3FFu;                       /* drop the implicit bit     */
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | (m << 13);
        }
    } else if (exp == 0x1Fu) {
        /* inf / NaN: keep mantissa payload */
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        /* normal: rebias exponent 15 -> 127, shift mantissa 10 -> 23 */
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }

    /* type-pun via union (no <string.h> dependency) */
    union { uint32_t u; float f; } pun;
    pun.u = bits;
    return pun.f;
}

/* read a little-endian uint16 (the scale d) without alignment assumptions */
static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* ------------------------------------------------------------------ */
/* Q8_0                                                               */
/* ------------------------------------------------------------------ */
int qz_matmul_q8_0(const uint8_t *w_data, size_t in, size_t out,
                   const float *x, float *y)
{
    if (in % QK8_0 != 0) return -1;

    const size_t nblk      = in / QK8_0;        /* blocks per row          */
    const size_t row_bytes = nblk * (2 + QK8_0);/* 2-byte d + 32 int8      */

    for (size_t i = 0; i < out; i++) {
        const uint8_t *row = w_data + i * row_bytes;
        float acc = 0.0f;
        size_t base = 0;                        /* column index into x     */

        for (size_t b = 0; b < nblk; b++) {
            const uint8_t *blk = row + b * (2 + QK8_0);
            const float d = qz_fp16_to_fp32(rd_u16le(blk));
            const int8_t *q = (const int8_t *)(blk + 2);

            /* dequant block on the fly: w = d*q[k]; multiply-accumulate */
            for (int k = 0; k < QK8_0; k++)
                acc += (d * (float)q[k]) * x[base + (size_t)k];

            base += QK8_0;
        }
        y[i] = acc;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Q4_0                                                               */
/* ------------------------------------------------------------------ */
int qz_matmul_q4_0(const uint8_t *w_data, size_t in, size_t out,
                   const float *x, float *y)
{
    if (in % QK4_0 != 0) return -1;

    const size_t nblk      = in / QK4_0;          /* blocks per row        */
    const size_t row_bytes = nblk * (2 + QK4_0/2);/* 2-byte d + 16 packed  */

    for (size_t i = 0; i < out; i++) {
        const uint8_t *row = w_data + i * row_bytes;
        float acc = 0.0f;
        size_t base = 0;

        for (size_t b = 0; b < nblk; b++) {
            const uint8_t *blk = row + b * (2 + QK4_0/2);
            const float d = qz_fp16_to_fp32(rd_u16le(blk));
            const uint8_t *qs = blk + 2;

            /* ggml layout: byte j -> elements j (low nibble) and j+16 (high),
             * each centered by -8. We accumulate in element-index order. */
            for (int j = 0; j < QK4_0/2; j++) {
                const int x0 = (qs[j] & 0x0F) - 8;     /* element j         */
                const int x1 = (qs[j] >>   4) - 8;     /* element j + 16    */
                acc += (d * (float)x0) * x[base + (size_t)j];
                acc += (d * (float)x1) * x[base + (size_t)j + QK4_0/2];
            }

            base += QK4_0;
        }
        y[i] = acc;
    }
    return 0;
}
