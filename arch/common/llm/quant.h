/*
 *  quant.h — dequantize-while-you-matmul for GGUF block-quantized weights.
 *
 *  Milestone M1b (docs/architecture/20-architecture/inference-engine.md §3, §5):
 *  given one GGUF quantized weight matrix W and a float input vector x,
 *  compute y = W·x by dequantizing each 32-value block ON THE FLY (the weight
 *  stays quantized in memory; it is widened to float only inside the multiply).
 *  This is "the heavy lift" (§3: ~99% of inference compute). M1b is the SINGLE
 *  matmul, correctness-first, plain C — NO SIMD/NEON (that is a later perf wave),
 *  NO RoPE / attention / forward (those are M1c).
 *
 *  Scope / honesty (conversation.md §2): host / Android-side ("身体") code, the
 *  same tier as gguf.c. libc-light: <stdint.h>/<stddef.h> only, no malloc here.
 *
 *  ggml weight-matrix convention (matches ggml_mul_mat):
 *    a GGUF tensor with dims [ne0, ne1] is an out_features × in_features matrix
 *    stored row-major where ne0 = in_features (the REDUCTION dim, contiguous and
 *    block-quantized) and ne1 = out_features (the number of rows). Thus
 *        y[i] = sum_{j=0..ne0-1} W[i][j] * x[j]   for i in [0, ne1)
 *    and each ROW of ne0 elements is an independent run of 32-value blocks.
 *
 *  Block geometry (verified against ggml-quants, NOT guessed):
 *    Q8_0 : 32 vals/block = fp16 scale d (2 bytes) + 32 int8 q (34 bytes total)
 *           dequant: w = d * q[k]
 *    Q4_0 : 32 vals/block = fp16 scale d (2 bytes) + 16 packed bytes (18 total)
 *           byte j (j in 0..15) holds two nibbles:
 *               low  nibble (qs[j] & 0x0F) - 8  -> element j
 *               high nibble (qs[j] >> 4)   - 8  -> element j + 16
 *           dequant: w = d * (nibble - 8)
 *           (little-endian fp16 scale, lower nibble = lower index — ggml layout)
 */
#ifndef PKERNEL_QUANT_H
#define PKERNEL_QUANT_H

/* M1b carries its own version (modver registry; compatibility.md): the
 * dequant-while-you-matmul contract. v1 = Q8_0 + Q4_0 single matmul. */
#define LLM_QUANT_VER  1

#include <stdint.h>
#include <stddef.h>

#define QK8_0 32          /* elements per Q8_0 block                         */
#define QK4_0 32          /* elements per Q4_0 block                         */

/* IEEE-754 binary16 -> binary32. Handles subnormals, inf, NaN. Libc-free. */
float qz_fp16_to_fp32(uint16_t h);

/* Dequantize-while-you-matmul: y[i] = sum_j W[i][j] * x[j].
 *
 *   w_data : raw tensor bytes from the GGUF mmap (gguf_tensor.data)
 *   in     : ne0 = in_features  (reduction dim; MUST be a multiple of 32)
 *   out    : ne1 = out_features (number of rows)
 *   x      : float input vector, length `in`
 *   y      : float output vector, length `out` (caller-allocated)
 *
 * Returns 0 on success, -1 if `in % 32 != 0` (block-quantized rows require it).
 * The weights are never materialized: each block is widened to float, used, and
 * discarded inside the inner loop (a few floats of scratch, O(1) extra memory).
 */
int qz_matmul_q8_0(const uint8_t *w_data, size_t in, size_t out,
                   const float *x, float *y);
int qz_matmul_q4_0(const uint8_t *w_data, size_t in, size_t out,
                   const float *x, float *y);

#endif /* PKERNEL_QUANT_H */
