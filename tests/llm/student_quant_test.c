/*
 *  student_quant_test.c — host cert for the brain's self-quantization, stage 1
 *  (arch/common/llm/student.c st_quant_fake;
 *   docs/architecture/30-module/research/brain-quantization.md §2/§3).
 *
 *  One M-tier baby is trained on a fixed corpus (fixed seed), then fake-
 *  quantized (RTN, dequantized back to fp32) at bits {8,4,2} x gran
 *  {row,g32,tensor}; held-out loss is measured before/after.
 *
 *  Gates:
 *    (A) [st-quant-int8]   int8 per-row: |delta| <= EPS8 nats.
 *    (B) [st-quant-damage] 2-bit: delta(tensor) > delta(g32); and 2-bit
 *                          tensor is clearly damaged: delta > DMG2 nats AND
 *                          > 10x |delta(int8 row)| — the measure can SEE damage.
 *                          (Audit-13 found the old 4-bit "tensor >= row" held
 *                          only by seed luck: 2 of 3 other seeds and another
 *                          corpus reversed it. The 2-bit ordering held in all
 *                          5 by a wide margin; 4-bit deltas are noise here.)
 *    (C) [st-quant-bound]  for EVERY quantized element, recomputing the scale
 *                          HERE from src (not trusting the quantizer):
 *                          |w - w_hat| <= s/2, w_hat/s is an integer in
 *                          [-qmax-1, qmax]; >0 elements changed; router +
 *                          RMSNorm gains byte-identical; src untouched.
 *    (D) [st-quant-determ] quantizing twice gives byte-identical w.
 *
 *  Negative controls (run_st_quant.sh builds them): -DST_QUANT_NOOP must go RED
 *  on (C) (nothing changed), -DST_QUANT_BADSCALE (s/4, clips) RED on (C) and (A).
 *
 *  argv[1] (optional): init seed of the trained baby (default 0xC0FFEE).
 *  run_st_quant.sh runs the plain build on several seeds; all must pass.
 *
 *  Build (wave-49): -O1 -ffp-contract=off.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../arch/common/llm/student.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

/* EPS8: int8 per-row must cost at most this many nats of held-out loss.
 * DMG2: 2-bit per-tensor must cost at least this much (damage is visible).
 * Rationale + measured values: run_st_quant.sh header. */
#define EPS8  0.01f
#define DMG2  0.10f

static const uint8_t CORPUS[] =
    "the cat sat on the mat. the dog ran in the sun. she said the sea is blue "
    "and the sky is blue too. a bird sang on the old oak tree at dawn. the cat "
    "and the dog sat by the sea and saw the sun set red. the old man ran to the "
    "sea to see the red sun set on the blue sea at the end of the day. the bird "
    "and the cat saw the old man by the tree. the sun is red and the sea is blue.";

#define SEQLEN 24

static float heldout_loss(st_model *m, int from, int count)
{
    double tot = 0.0; int got = 0;
    for (int w = 0; w < count; w++) {
        int off = from + w * SEQLEN;
        if (off + SEQLEN > (int)sizeof(CORPUS) - 1) break;
        uint8_t buf[SEQLEN];
        for (int i = 0; i < SEQLEN; i++) buf[i] = CORPUS[off + i];
        int np = 0;
        float l = st_eval_loss(m, buf, SEQLEN, &np);
        if (np) { tot += l; got++; }
    }
    return got ? (float)(tot / got) : 0.0f;
}

/* The quantized matrices, derived HERE from the o_* offsets (independent of
 * student.c's own list): {offset, rows, cols}. */
typedef struct { int off, rows, cols; } qmat;
static int mat_list(const st_model *m, qmat *out, int max)
{
    int n = 0, D = m->d, F = m->dff, L = m->nlayer, E = m->nexpert, V = ST_VOCAB;
#define ADD(o, r, c) do { if (n < max) { out[n].off=(o); out[n].rows=(r); out[n].cols=(c); } n++; } while (0)
    ADD(m->o_embed, V, D);
    for (int l = 0; l < L; l++) {
        ADD(m->o_wq + l*D*D, D, D); ADD(m->o_wk + l*D*D, D, D);
        ADD(m->o_wv + l*D*D, D, D); ADD(m->o_wo + l*D*D, D, D);
    }
    for (int x = 0; x < L*E; x++) {
        ADD(m->o_w1 + x*F*D, F, D); ADD(m->o_w3 + x*F*D, F, D);
        ADD(m->o_w2 + x*D*F, D, F);
    }
    ADD(m->o_out, V, D);
#undef ADD
    return n;
}

static float fabsf_(float x) { return x < 0 ? -x : x; }

/* gate (C) for one (bits, gran): returns #violations, adds #changed. */
static long bound_check(const st_model *src, const st_model *dst, int bits,
                        int gran, long *changed, long *nq)
{
    qmat ml[256];
    int nm = mat_list(src, ml, 256);
    int qmax = (1 << (bits - 1)) - 1;
    long viol = 0;
    for (int k = 0; k < nm && k < 256; k++) {
        int glen = gran == ST_QG_TENSOR ? ml[k].rows * ml[k].cols
                 : gran == ST_QG_G32    ? 32 : ml[k].cols;
        int total = ml[k].rows * ml[k].cols;
        for (int g0 = 0; g0 < total; g0 += glen) {
            const float *a = src->w + ml[k].off + g0;
            const float *b = dst->w + ml[k].off + g0;
            int n = glen; if (g0 + n > total) n = total - g0;
            float amax = 0.0f;
            for (int i = 0; i < n; i++) if (fabsf_(a[i]) > amax) amax = fabsf_(a[i]);
            float s = amax / (float)qmax;
            float tol = 1e-5f * amax + 1e-30f;
            for (int i = 0; i < n; i++) {
                (*nq)++;
                if (a[i] != b[i]) (*changed)++;
                if (s == 0.0f) { if (b[i] != 0.0f) viol++; continue; }
                if (fabsf_(a[i] - b[i]) > 0.5f * s + tol) { viol++; continue; }
                float r = b[i] / s;
                long ri = (long)(r < 0 ? r - 0.5f : r + 0.5f);
                if (fabsf_(r - (float)ri) > 1e-3f || ri < -qmax - 1 || ri > qmax) viol++;
            }
        }
    }
    return viol;
}

static int fp32_parts_same(const st_model *a, const st_model *b)
{
    int D = a->d, L = a->nlayer, E = a->nexpert;
    if (memcmp(a->w + a->o_router, b->w + b->o_router, (size_t)L*E*D*4)) return 0;
    if (memcmp(a->w + a->o_attn_norm, b->w + b->o_attn_norm, (size_t)L*D*4)) return 0;
    if (memcmp(a->w + a->o_ffn_norm, b->w + b->o_ffn_norm, (size_t)L*D*4)) return 0;
    if (memcmp(a->w + a->o_out_norm, b->w + b->o_out_norm, (size_t)D*4)) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    uint32_t seed = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0xC0FFEEu;
    printf("=== [st-quant] brain self-quantization stage 1 cert (seed=0x%lx) ===\n",
           (unsigned long)seed);
#if defined(ST_QUANT_NOOP)
    printf("(NEGATIVE CONTROL build: -DST_QUANT_NOOP — expect RED)\n");
#elif defined(ST_QUANT_BADSCALE)
    printf("(NEGATIVE CONTROL build: -DST_QUANT_BADSCALE — expect RED)\n");
#endif
    int total_w = ((int)sizeof(CORPUS) - 1) / SEQLEN;
    int train_w = total_w * 3 / 4;
    int held_from = train_w * SEQLEN, held_w = total_w - train_w;

    float *logits = (float *)malloc((size_t)SEQLEN * ST_VOCAB * sizeof(float));
    st_model M, Q, Q2;
    if (!logits || st_init_tier(&M, seed, ST_TIER_M) != ST_OK ||
        st_init_tier(&Q, 1, ST_TIER_M) != ST_OK ||
        st_init_tier(&Q2, 2, ST_TIER_M) != ST_OK) { printf("OOM\n"); return 2; }

    const int ROUNDS = 60; const float LR = 0.02f;
    for (int r = 0; r < ROUNDS; r++)
        for (int j = 0; j < train_w; j++) {
            uint8_t b[SEQLEN];
            for (int i = 0; i < SEQLEN; i++) b[i] = CORPUS[j*SEQLEN + i];
            st_zero_grad(&M); st_forward(&M, b, SEQLEN, logits);
            st_backward(&M, b, SEQLEN); st_adam_step(&M, LR);
        }
    float fp = heldout_loss(&M, held_from, held_w);
    printf("  corpus=%dB train=%dwin held=%dwin  fp32 held-out = %.4f nats "
           "(chance %.4f)\n", (int)sizeof(CORPUS) - 1, train_w, held_w,
           (double)fp, (double)st_logf(256.0f));

    size_t np = (size_t)M.n_params;
    float *snap = (float *)malloc(np * sizeof(float));
    memcpy(snap, M.w, np * sizeof(float));

    static const int BITS[3] = { 8, 4, 2 };
    static const char *GN[3] = { "row", "g32", "tensor" };
    float delta[3][3];
    long viol_all = 0, changed_all = 0, nq_all = 0;
    int fp32_ok = 1, rc_ok = 1;
    for (int bi = 0; bi < 3; bi++)
        for (int g = 0; g < 3; g++) {
            size_t bq = 0;
            if (st_quant_fake(&M, &Q, BITS[bi], g, &bq) != ST_OK) rc_ok = 0;
            float ql = heldout_loss(&Q, held_from, held_w);
            delta[bi][g] = ql - fp;
            long ch = 0, nq = 0;
            long v = bound_check(&M, &Q, BITS[bi], g, &ch, &nq);
            viol_all += v; changed_all += ch; nq_all += nq;
            if (!fp32_parts_same(&M, &Q)) fp32_ok = 0;
            printf("  [st-quant] bits=%d gran=%-6s fp32=%.4f q=%.4f delta=%+.4f "
                   "bytes_q=%zu (fp32 w %zu)  changed=%ld/%ld viol=%ld\n",
                   BITS[bi], GN[g], (double)fp, (double)ql, (double)delta[bi][g],
                   bq, np * 4, ch, nq, v);
        }

    printf("\n[A] int8 per-row\n");
    CHECK(rc_ok, "st_quant_fake returned ST_OK for all 9 (bits,gran)");
    CHECK(fabsf_(delta[0][0]) <= EPS8, "[st-quant-int8] |delta(8,row)| <= EPS8");

    printf("[B] damage is visible\n");
    CHECK(delta[2][2] > delta[2][1], "[st-quant-damage] delta(2,tensor) > delta(2,g32)");
    CHECK(delta[2][2] > DMG2 && delta[2][2] > 10.0f * fabsf_(delta[0][0]),
          "[st-quant-damage] delta(2,tensor) > DMG2 and > 10x |delta(8,row)|");

    printf("[C] per-element bound (scale recomputed by the cert)\n");
    printf("  elements checked=%ld changed=%ld violations=%ld\n",
           nq_all, changed_all, viol_all);
    CHECK(viol_all == 0, "[st-quant-bound] |w-w_hat|<=s/2 and w_hat/s integer in range");
    CHECK(changed_all > 0, "[st-quant-bound] quantization changed >0 elements");
    CHECK(fp32_ok, "[st-quant-bound] router + RMSNorm gains byte-identical");
    CHECK(memcmp(snap, M.w, np * sizeof(float)) == 0, "[st-quant-bound] src w untouched");

    printf("[D] determinism\n");
    st_quant_fake(&M, &Q, 4, ST_QG_ROW, NULL);
    st_quant_fake(&M, &Q2, 4, ST_QG_ROW, NULL);
    CHECK(memcmp(Q.w, Q2.w, np * sizeof(float)) == 0, "[st-quant-determ] twice -> byte-identical");

    printf("[E] refusal\n");
    st_model S;
    st_init_tier(&S, 3, ST_TIER_S);
    CHECK(st_quant_fake(&M, &S, 8, ST_QG_ROW, NULL) == ST_E_ARG, "cross-tier dst refused");
    CHECK(st_quant_fake(&M, &Q, 3, ST_QG_ROW, NULL) == ST_E_ARG, "bits=3 refused");
    CHECK(st_quant_fake(&M, &Q, 8, 7, NULL) == ST_E_ARG, "gran=7 refused");
    st_free(&S);

    printf("\n[st-quant] %d PASS / %d FAIL\n", g_pass, g_fail);
    st_free(&M); st_free(&Q); st_free(&Q2); free(snap); free(logits);
    return g_fail ? 1 : 0;
}
