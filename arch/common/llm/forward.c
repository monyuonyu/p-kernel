/*
 *  forward.c — SmolLM2-135M (plain Llama) forward + greedy generation (M1c).
 *  See forward.h for the contract and the scope/honesty note.
 *
 *  Build (wave-49 rule, one math everywhere): -O1 -ffp-contract=off.
 *
 *  Reference: this matches llama.cpp's plain-Llama graph for SmolLM2:
 *    h = tok_embd[id]
 *    for each layer:
 *        a = RMSNorm(h, attn_norm)
 *        q = Wq·a ; k = Wk·a ; v = Wv·a        (no bias)
 *        RoPE(q per head) ; RoPE(k per kv-head) (NORMAL mode, adjacent pairs)
 *        append k,v to the KV cache at this position
 *        for each Q head, attend over all cached positions of its KV head
 *        (GQA: q_head h uses kv_head = h / (n_head/n_kv_head)); causal is
 *        automatic because the cache only holds positions <= current.
 *        o = Wo·concat(heads)
 *        h = h + o
 *        f = RMSNorm(h, ffn_norm)
 *        h = h + Wdown·( SiLU(Wgate·f) * (Wup·f) )
 *    h = RMSNorm(h, output_norm)
 *    logits = tok_embd · h     (tied embeddings)
 *
 *  RoPE detail (verified against ggml ggml_rope_cache_init + rotate_pairs,
 *  NORMAL mode): for pair j in [0, head_dim/2), rotate the ADJACENT elements
 *  (2j, 2j+1) by angle pos * rope_base^(-2j/head_dim). NOT the NeoX split-half.
 */
#include "forward.h"
#include "quant.h"
#include <stdlib.h>     /* malloc / free — host/Android tier (conversation §2) */

/* ============================== libc-free math ============================ */
/* All transcendentals are self-contained so "one math everywhere" holds.
 * These mirror dtr.c's algorithms (range-reduced exp, bit-twiddle rsqrt) but
 * live here to honor the no-modify-dtr.c constraint; forward.c stays libc-free
 * except for malloc/free. */

float lm_expf(float x)
{
    /* e^x = 2^k * e^r, k = round(x/ln2), r in [-ln2/2, ln2/2], 7-term Taylor. */
    if (x >  88.0f) return 3.0e38f;
    if (x < -87.0f) return 0.0f;
    float t = x * 1.4426950408889634f;                 /* x / ln2            */
    int   k = (int)(t + (t >= 0.0f ? 0.5f : -0.5f));   /* round to nearest   */
    float r = x - (float)k * 0.6931471805599453f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f +
              r * (0.041666667f + r * (0.0083333333f +
              r * 0.0013888889f)))));
    union { uint32_t u; float f; } s;
    s.u = (uint32_t)(k + 127) << 23;                   /* 2^k                */
    return p * s.f;
}

/* 1/sqrt(x): bit-trick seed + 3 Newton steps (double-precision-clean enough
 * for RMSNorm; rel err < 1e-6). */
float lm_rsqrtf(float x)
{
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t u; } v;
    v.f = x;
    v.u = 0x5f3759dfu - (v.u >> 1);                    /* fast inverse sqrt   */
    float y = v.f;
    float xh = 0.5f * x;
    y = y * (1.5f - xh * y * y);
    y = y * (1.5f - xh * y * y);
    y = y * (1.5f - xh * y * y);
    return y;
}

/* sin/cos via Payne–Hanek-lite range reduction to [-pi/4, pi/4] + minimax
 * polynomials. Adequate for RoPE angles (which can be large: pos * freq). */
void lm_sincosf(float a, float *so, float *co)
{
    /* reduce a to quadrant: n = round(a / (pi/2)); r = a - n*(pi/2).
     * round(a / (pi/2)) using double for the multiply to limit cancellation. */
    double q = (double)a * 0.6366197723675814;         /* 2/pi               */
    long   n = (long)(q + (q >= 0.0 ? 0.5 : -0.5));
    /* r = a - n*pi/2 via Cody–Waite (3-part pi/2) to keep precision for big a */
    double r = (double)a
             - (double)n * 1.5707963109016418            /* pi/2 hi          */
             - (double)n * 1.5893254712640187e-08        /* pi/2 mid         */
             - (double)n * 6.123233995736766e-17;        /* pi/2 lo (~0)     */
    double r2 = r * r;
    /* sin/cos minimax on [-pi/4, pi/4] (double coeffs) */
    double s = r * (1.0 + r2 * (-1.6666666664e-01 + r2 * (8.3333315e-03 +
               r2 * (-1.98412698e-04 + r2 * 2.7557314e-06))));
    double c = 1.0 + r2 * (-0.5 + r2 * (4.16666666e-02 + r2 * (-1.388731e-03 +
               r2 * 2.443315e-05)));
    float fs, fc;
    switch (((unsigned long)n) & 3u) {
        case 0:  fs = (float)s;  fc = (float)c;  break;
        case 1:  fs = (float)c;  fc = (float)(-s); break;
        case 2:  fs = (float)(-s); fc = (float)(-c); break;
        default: fs = (float)(-c); fc = (float)s;  break;
    }
    *so = fs;
    *co = fc;
}

/* SiLU(x) = x * sigmoid(x) = x / (1 + e^-x). */
static float lm_silu(float x)
{
    return x / (1.0f + lm_expf(-x));
}

/* ============================== error strings ============================ */
const char *lm_strerror(int e)
{
    switch (e) {
        case LM_OK:        return "ok";
        case LM_E_META:    return "missing metadata key";
        case LM_E_TENSOR:  return "missing tensor";
        case LM_E_OOM:     return "out of memory";
        case LM_E_TYPE:    return "unexpected tensor type";
        case LM_E_SHAPE:   return "tensor shape disagrees with config";
        default:           return "unknown error";
    }
}

/* ============================== tensor lookup ============================ */
/* gguf names are non-NUL-terminated gguf_str; compare against a C string. */
static int name_eq(const gguf_str *s, const char *c)
{
    size_t i = 0;
    for (; i < s->len; i++) {
        if (c[i] == '\0' || s->ptr[i] != c[i]) return 0;
    }
    return c[i] == '\0';
}

static const gguf_tensor *find_tensor(const gguf_file *gf, const char *name)
{
    for (uint64_t i = 0; i < gf->n_tensors; i++) {
        if (name_eq(&gf->tensors[i].name, name)) return &gf->tensors[i];
    }
    return NULL;
}

/* build "blk.<L>.<suffix>" into buf (no snprintf: libc-free) */
static void layer_name(char *buf, int layer, const char *suffix)
{
    char *p = buf;
    const char *pre = "blk.";
    while (*pre) *p++ = *pre++;
    /* decimal of layer (0..n, < 1000) */
    if (layer >= 100) { *p++ = (char)('0' + layer / 100); }
    if (layer >= 10)  { *p++ = (char)('0' + (layer / 10) % 10); }
    *p++ = (char)('0' + layer % 10);
    *p++ = '.';
    while (*suffix) *p++ = *suffix++;
    *p = '\0';
}

/* ============================== generic matmul =========================== */
/* y[i] = sum_j W[i][j] * x[j], W = tensor[in=ne0, out=ne1]. Dispatches on the
 * tensor's quant type. Q8_0 reuses M1b; F32 is a plain dense matmul (added in
 * M1c per the brief — norm weights are F32, and a portable Llama may store any
 * matrix as F32). Returns 0 / negative. */
static int matmul_tensor(const gguf_tensor *w, const float *x, float *y)
{
    const size_t in  = (size_t)w->dims[0];
    const size_t out = (size_t)w->dims[1];
    if (w->type == GGML_TYPE_Q8_0) {
        return qz_matmul_q8_0(w->data, in, out, x, y);
    }
    if (w->type == GGML_TYPE_F32) {
        const float *wf = (const float *)w->data;
        for (size_t i = 0; i < out; i++) {
            float acc = 0.0f;
            const float *row = wf + i * in;
            for (size_t j = 0; j < in; j++) acc += row[j] * x[j];
            y[i] = acc;
        }
        return 0;
    }
    return LM_E_TYPE;
}

/* RMSNorm: y = x / sqrt(mean(x^2)+eps) * weight (weight is F32, length d). */
static void rmsnorm(const float *x, const float *w, int d, float eps, float *y)
{
    float ss = 0.0f;
    for (int i = 0; i < d; i++) ss += x[i] * x[i];
    ss = ss / (float)d + eps;
    float inv = lm_rsqrtf(ss);
    for (int i = 0; i < d; i++) y[i] = x[i] * inv * w[i];
}

/* Apply NORMAL-mode RoPE in place to a per-head slice of length head_dim at
 * position `pos`. Pair j (j in [0, head_dim/2)) rotates ADJACENT elements
 * (2j, 2j+1) by angle pos * base^(-2j/head_dim). */
static void rope_head(float *h, int head_dim, int pos, float base)
{
    for (int j = 0; j < head_dim / 2; j++) {
        /* freq exponent = -2j/head_dim; theta = pos * base^(exponent) */
        float exponent = -2.0f * (float)j / (float)head_dim;
        /* base^exponent = exp(exponent * ln(base)); use lm_expf + a tiny ln */
        /* ln(base): base is a constant from metadata; compute via the same
         * range-reduced approach dtr_logf uses, inlined here. */
        /* ln(base) once would suffice but this is called rarely (head_dim/2
         * per head per token); keep it simple and correct. */
        float lb;
        {
            union { float f; uint32_t u; } v; v.f = base;
            int e = (int)((v.u >> 23) & 0xFF) - 127;
            v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
            float m = v.f;
            float z = (m - 1.0f) / (m + 1.0f);
            float z2 = z * z;
            lb = 2.0f * z * (1.0f + z2 * (0.33333333f + z2 * (0.2f +
                 z2 * (0.14285714f + z2 * 0.11111111f))))
                 + (float)e * 0.6931471805599453f;
        }
        float freq  = lm_expf(exponent * lb);
        float theta = (float)pos * freq;
        float s, c;
        lm_sincosf(theta, &s, &c);
        float x0 = h[2 * j];
        float x1 = h[2 * j + 1];
        h[2 * j]     = x0 * c - x1 * s;
        h[2 * j + 1] = x0 * s + x1 * c;
    }
}

/* ============================== load ==================================== */
int lm_load(lm_model *m, const gguf_file *gf)
{
    for (size_t i = 0; i < sizeof(*m); i++) ((char *)m)[i] = 0;
    m->gf = gf;

    uint64_t u;
    float    f;
    if (!gguf_get_u64(gf, "llama.block_count", &u))                       return LM_E_META;
    m->n_layer = (int)u;
    if (!gguf_get_u64(gf, "llama.embedding_length", &u))                  return LM_E_META;
    m->d_model = (int)u;
    if (!gguf_get_u64(gf, "llama.attention.head_count", &u))              return LM_E_META;
    m->n_head = (int)u;
    if (!gguf_get_u64(gf, "llama.attention.head_count_kv", &u))           return LM_E_META;
    m->n_kv_head = (int)u;
    if (!gguf_get_u64(gf, "llama.feed_forward_length", &u))               return LM_E_META;
    m->d_ff = (int)u;
    if (!gguf_get_u64(gf, "llama.context_length", &u))                    return LM_E_META;
    m->max_ctx = (int)u;
    if (!gguf_get_f32(gf, "llama.rope.freq_base", &f))                    f = 10000.0f;
    m->rope_base = f;
    if (!gguf_get_f32(gf, "llama.attention.layer_norm_rms_epsilon", &f))  f = 1e-5f;
    m->rms_eps = f;

    m->head_dim = m->d_model / m->n_head;
    m->n_kv     = m->n_kv_head * m->head_dim;

    /* vocab: prefer the embedding tensor's ne1 (authoritative), fall back to
     * the metadata key if present. */
    m->tok_embd = find_tensor(gf, "token_embd.weight");
    m->out_norm = find_tensor(gf, "output_norm.weight");
    if (!m->tok_embd || !m->out_norm) return LM_E_TENSOR;
    m->vocab = (int)m->tok_embd->dims[1];
    if ((int)m->tok_embd->dims[0] != m->d_model) return LM_E_SHAPE;

    /* allocate per-layer handle arrays */
    int L = m->n_layer;
    m->attn_norm = calloc((size_t)L, sizeof(*m->attn_norm));
    m->attn_q    = calloc((size_t)L, sizeof(*m->attn_q));
    m->attn_k    = calloc((size_t)L, sizeof(*m->attn_k));
    m->attn_v    = calloc((size_t)L, sizeof(*m->attn_v));
    m->attn_out  = calloc((size_t)L, sizeof(*m->attn_out));
    m->ffn_norm  = calloc((size_t)L, sizeof(*m->ffn_norm));
    m->ffn_gate  = calloc((size_t)L, sizeof(*m->ffn_gate));
    m->ffn_up    = calloc((size_t)L, sizeof(*m->ffn_up));
    m->ffn_down  = calloc((size_t)L, sizeof(*m->ffn_down));
    if (!m->attn_norm || !m->attn_q || !m->attn_k || !m->attn_v ||
        !m->attn_out || !m->ffn_norm || !m->ffn_gate || !m->ffn_up ||
        !m->ffn_down) { lm_free(m); return LM_E_OOM; }

    char nb[64];
    for (int l = 0; l < L; l++) {
        layer_name(nb, l, "attn_norm.weight");   m->attn_norm[l] = find_tensor(gf, nb);
        layer_name(nb, l, "attn_q.weight");       m->attn_q[l]    = find_tensor(gf, nb);
        layer_name(nb, l, "attn_k.weight");       m->attn_k[l]    = find_tensor(gf, nb);
        layer_name(nb, l, "attn_v.weight");       m->attn_v[l]    = find_tensor(gf, nb);
        layer_name(nb, l, "attn_output.weight");  m->attn_out[l]  = find_tensor(gf, nb);
        layer_name(nb, l, "ffn_norm.weight");     m->ffn_norm[l]  = find_tensor(gf, nb);
        layer_name(nb, l, "ffn_gate.weight");     m->ffn_gate[l]  = find_tensor(gf, nb);
        layer_name(nb, l, "ffn_up.weight");       m->ffn_up[l]    = find_tensor(gf, nb);
        layer_name(nb, l, "ffn_down.weight");     m->ffn_down[l]  = find_tensor(gf, nb);
        if (!m->attn_norm[l] || !m->attn_q[l] || !m->attn_k[l] || !m->attn_v[l] ||
            !m->attn_out[l] || !m->ffn_norm[l] || !m->ffn_gate[l] || !m->ffn_up[l] ||
            !m->ffn_down[l]) { lm_free(m); return LM_E_TENSOR; }
        /* shape sanity (orientation is the M1b open caveat — assert it). */
        if ((int)m->attn_q[l]->dims[0] != m->d_model ||
            (int)m->attn_q[l]->dims[1] != m->n_head * m->head_dim) { lm_free(m); return LM_E_SHAPE; }
        if ((int)m->attn_k[l]->dims[0] != m->d_model ||
            (int)m->attn_k[l]->dims[1] != m->n_kv)              { lm_free(m); return LM_E_SHAPE; }
        if ((int)m->attn_out[l]->dims[0] != m->n_head * m->head_dim ||
            (int)m->attn_out[l]->dims[1] != m->d_model)         { lm_free(m); return LM_E_SHAPE; }
        if ((int)m->ffn_gate[l]->dims[0] != m->d_model ||
            (int)m->ffn_gate[l]->dims[1] != m->d_ff)            { lm_free(m); return LM_E_SHAPE; }
        if ((int)m->ffn_down[l]->dims[0] != m->d_ff ||
            (int)m->ffn_down[l]->dims[1] != m->d_model)         { lm_free(m); return LM_E_SHAPE; }
    }

    /* scratch + KV cache */
    int dm = m->d_model, dq = m->n_head * m->head_dim, dkv = m->n_kv;
    m->x      = calloc((size_t)dm, sizeof(float));
    m->xn     = calloc((size_t)dm, sizeof(float));
    m->q      = calloc((size_t)dq, sizeof(float));
    m->k      = calloc((size_t)dkv, sizeof(float));
    m->v      = calloc((size_t)dkv, sizeof(float));
    m->attn_o = calloc((size_t)dq, sizeof(float));
    m->scores = calloc((size_t)m->max_ctx, sizeof(float));
    m->ffn_g  = calloc((size_t)m->d_ff, sizeof(float));
    m->ffn_u  = calloc((size_t)m->d_ff, sizeof(float));
    m->ffn_d  = calloc((size_t)dm, sizeof(float));
    m->logits = calloc((size_t)m->vocab, sizeof(float));
    m->kcache = calloc((size_t)L * (size_t)m->max_ctx * (size_t)dkv, sizeof(float));
    m->vcache = calloc((size_t)L * (size_t)m->max_ctx * (size_t)dkv, sizeof(float));
    if (!m->x || !m->xn || !m->q || !m->k || !m->v || !m->attn_o || !m->scores ||
        !m->ffn_g || !m->ffn_u || !m->ffn_d || !m->logits || !m->kcache || !m->vcache) {
        lm_free(m); return LM_E_OOM;
    }
    m->pos = 0;
    return LM_OK;
}

void lm_free(lm_model *m)
{
    free(m->attn_norm); free(m->attn_q); free(m->attn_k); free(m->attn_v);
    free(m->attn_out);  free(m->ffn_norm); free(m->ffn_gate); free(m->ffn_up);
    free(m->ffn_down);
    free(m->x); free(m->xn); free(m->q); free(m->k); free(m->v);
    free(m->attn_o); free(m->scores); free(m->ffn_g); free(m->ffn_u);
    free(m->ffn_d); free(m->logits); free(m->kcache); free(m->vcache);
    /* leave the struct zeroed-ish; pointers freed. */
    m->attn_norm = NULL; m->kcache = NULL; m->vcache = NULL;
}

void lm_reset(lm_model *m) { m->pos = 0; }

/* ============================== forward ================================= */
int lm_forward(lm_model *m, int token_id)
{
    const int dm  = m->d_model;
    const int hd  = m->head_dim;
    const int nh  = m->n_head;
    const int nkv = m->n_kv_head;
    const int dkv = m->n_kv;
    const int g   = nh / nkv;                 /* Q heads per KV head (GQA)     */
    const int pos = m->pos;
    if (pos >= m->max_ctx) return LM_E_SHAPE; /* context overflow              */

    /* --- token embedding lookup (row token_id of token_embd, Q8_0) --- */
    /* dequant one row: y = embd[id] = the id-th row (length d_model). We reuse
     * the matmul with x = e_{id}? No — cheaper: dequant the single row directly
     * by treating that row as out=1. token_embd is [d_model, vocab]: row v is v.
     * Row bytes = (d_model/32)*(2+32). Dequant it via qz_matmul with out=1 on
     * the row pointer and x=ones? That would SUM, not gather. Instead dequant
     * the row's blocks straight into m->x. */
    if (m->tok_embd->type == GGML_TYPE_Q8_0) {
        const int nblk = dm / 32;
        const uint8_t *row = m->tok_embd->data +
                             (size_t)token_id * (size_t)nblk * (2 + 32);
        for (int b = 0; b < nblk; b++) {
            const uint8_t *blk = row + (size_t)b * (2 + 32);
            float d = qz_fp16_to_fp32((uint16_t)((uint32_t)blk[0] | ((uint32_t)blk[1] << 8)));
            const int8_t *qd = (const int8_t *)(blk + 2);
            for (int kk = 0; kk < 32; kk++) m->x[b * 32 + kk] = d * (float)qd[kk];
        }
    } else if (m->tok_embd->type == GGML_TYPE_F32) {
        const float *row = (const float *)m->tok_embd->data + (size_t)token_id * dm;
        for (int i = 0; i < dm; i++) m->x[i] = row[i];
    } else {
        return LM_E_TYPE;
    }

    for (int l = 0; l < m->n_layer; l++) {
        /* attn norm */
        rmsnorm(m->x, (const float *)m->attn_norm[l]->data, dm, m->rms_eps, m->xn);

        /* q,k,v projections */
        if (matmul_tensor(m->attn_q[l], m->xn, m->q) != 0) return LM_E_TYPE;
        if (matmul_tensor(m->attn_k[l], m->xn, m->k) != 0) return LM_E_TYPE;
        if (matmul_tensor(m->attn_v[l], m->xn, m->v) != 0) return LM_E_TYPE;

        /* RoPE on each Q head and each KV head */
        for (int h = 0; h < nh; h++)  rope_head(m->q + h * hd, hd, pos, m->rope_base);
        for (int h = 0; h < nkv; h++) rope_head(m->k + h * hd, hd, pos, m->rope_base);

        /* append k,v to cache at this position */
        float *kc = m->kcache + ((size_t)l * m->max_ctx + pos) * dkv;
        float *vc = m->vcache + ((size_t)l * m->max_ctx + pos) * dkv;
        for (int i = 0; i < dkv; i++) { kc[i] = m->k[i]; vc[i] = m->v[i]; }

        /* attention per Q head */
        float scale = lm_rsqrtf((float)hd);     /* 1/sqrt(head_dim)            */
        for (int h = 0; h < nh; h++) {
            int kvh = h / g;                     /* which KV head this Q uses   */
            const float *qh = m->q + h * hd;
            /* scores over cached positions 0..pos (causal: cache only holds
             * positions <= pos), softmax, weighted sum of V. */
            float maxs = -3.0e38f;
            for (int t = 0; t <= pos; t++) {
                const float *kt = m->kcache + ((size_t)l * m->max_ctx + t) * dkv + kvh * hd;
                float dot = 0.0f;
                for (int i = 0; i < hd; i++) dot += qh[i] * kt[i];
                dot *= scale;
                m->scores[t] = dot;
                if (dot > maxs) maxs = dot;
            }
            float sum = 0.0f;
            for (int t = 0; t <= pos; t++) {
                float e = lm_expf(m->scores[t] - maxs);
                m->scores[t] = e;
                sum += e;
            }
            float invsum = 1.0f / sum;
            float *oh = m->attn_o + h * hd;
            for (int i = 0; i < hd; i++) oh[i] = 0.0f;
            for (int t = 0; t <= pos; t++) {
                float w = m->scores[t] * invsum;
                const float *vt = m->vcache + ((size_t)l * m->max_ctx + t) * dkv + kvh * hd;
                for (int i = 0; i < hd; i++) oh[i] += w * vt[i];
            }
        }

        /* output projection + residual */
        if (matmul_tensor(m->attn_out[l], m->attn_o, m->ffn_d) != 0) return LM_E_TYPE;
        for (int i = 0; i < dm; i++) m->x[i] += m->ffn_d[i];

        /* ffn norm */
        rmsnorm(m->x, (const float *)m->ffn_norm[l]->data, dm, m->rms_eps, m->xn);

        /* SwiGLU: down( SiLU(gate(xn)) * up(xn) ) */
        if (matmul_tensor(m->ffn_gate[l], m->xn, m->ffn_g) != 0) return LM_E_TYPE;
        if (matmul_tensor(m->ffn_up[l],   m->xn, m->ffn_u) != 0) return LM_E_TYPE;
        for (int i = 0; i < m->d_ff; i++) m->ffn_g[i] = lm_silu(m->ffn_g[i]) * m->ffn_u[i];
        if (matmul_tensor(m->ffn_down[l], m->ffn_g, m->ffn_d) != 0) return LM_E_TYPE;
        for (int i = 0; i < dm; i++) m->x[i] += m->ffn_d[i];
    }

    /* final norm + tied-embedding logits */
    rmsnorm(m->x, (const float *)m->out_norm->data, dm, m->rms_eps, m->xn);
    /* logits[v] = tok_embd_row[v] · xn  (tied embeddings; token_embd is
     * [d_model, vocab] = out_features vocab, in_features d_model, exactly the
     * matmul convention). */
    if (matmul_tensor(m->tok_embd, m->xn, m->logits) != 0) return LM_E_TYPE;

    m->pos = pos + 1;
    return LM_OK;
}

int lm_argmax(const lm_model *m)
{
    int best = 0;
    float bv = m->logits[0];
    for (int v = 1; v < m->vocab; v++) {
        if (m->logits[v] > bv) { bv = m->logits[v]; best = v; }
    }
    return best;
}

int lm_generate(lm_model *m, const int *in, int n_in, int *out, int n_gen)
{
    lm_reset(m);
    int last = 0;
    for (int i = 0; i < n_in; i++) {
        int rc = lm_forward(m, in[i]);
        if (rc != 0) return rc;
        last = lm_argmax(m);          /* logits after the last prompt token */
    }
    for (int g = 0; g < n_gen; g++) {
        out[g] = last;
        int rc = lm_forward(m, last);
        if (rc != 0) return rc;
        last = lm_argmax(m);
    }
    return n_gen;
}
