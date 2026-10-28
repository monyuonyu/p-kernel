/*
 *  student.c — the Cradle baby (NS-1). See student.h for the contract and the
 *  native-student.md citations. This file is the baby's WHOLE brain: its own
 *  forward, its own grad-checked backward, an Adam step, and a finite-diff
 *  grad-check. libc-free transcendentals (one-math rule, wave-49).
 *
 *  Math layout (per layer l, sequence position t):
 *    h            = x_t                                  (residual stream)
 *    a_in         = rmsnorm(h, attn_norm[l])
 *    q,k,v        = Wq a_in, Wk a_in, Wv a_in            (single head, dim D)
 *    attn_t       = sum_{s<=t} softmax_s(q_t . k_s / sqrt(D)) v_s   (causal)
 *    h            = h + Wo attn_t
 *    f_in         = rmsnorm(h, ffn_norm[l])
 *    gate_e       = router[l][e] . f_in                  (e = 0..E-1)
 *    pick top-K experts by gate, weights = softmax over the K picked logits
 *    moe          = sum_{e in topK} weight_e * w2_e ( silu(w1_e f_in) * (w3_e f_in) )
 *    h            = h + moe
 *  After L layers:
 *    o_in         = rmsnorm(h, out_norm)
 *    logits_t     = Out o_in                             (untied head, [VOCAB])
 *
 *  C1 / NS v2 (scale_wall_design.md §8, [ctx-carry]): the attention now carries
 *  RoPE positional encoding (q,k rotated by position BEFORE the score dot) and
 *  the context window widened ST_MAXSEQ 64->256. Still a SINGLE attention head
 *  (head_dim == d_model). RoPE is parameter-free, so the analytic backward stays
 *  grad-checkable end to end — the rotation is a fixed orthogonal map and its
 *  gradient transpose is the inverse rotation (applied to g_q/g_k before the
 *  Wq/Wk backward). Multi-head / vocab-merges remain later growth levers
 *  (native-student.md §A.2/§A.4/§A.5), honestly deferred — see the NS report.
 */
#include "student.h"
/* pk_nosw.h (hosted x86_64 only; empty elsewhere): st_forward shares tw_buf and
 * the st_frozen / st_fw / remote counters across ALL models and frees m->cache
 * on every call; backward and eval read that cache; adam and the blob / merge /
 * grow functions rewrite m->w. Before the hosted exit dispatch nothing here was
 * switched out (no kernel calls), so each public entry below is one no-switch
 * region. A forward -> backward -> adam sequence is one more: ST_TRAIN_STEP. */
#include "../include/pk_nosw.h"

/* ================================================================== */
/* libc-free math (same recipes as dtr.c / forward.c — one math)       */
/* ================================================================== */

_Static_assert(sizeof(float) == 4, "float must be IEEE754 binary32");
typedef union { float f; uint32_t u; } ST_F32;

float st_expf(float x)
{
    if (x >  88.0f) return 3.0e38f;
    if (x < -87.0f) return 0.0f;
    float t = x * 1.4426950f;                       /* x / ln2 */
    int   k = (int)(t + (t >= 0.0f ? 0.5f : -0.5f));
    float r = x - (float)k * 0.69314718f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f +
              r * (0.041666667f + r * (0.0083333333f +
              r * 0.0013888889f)))));
    ST_F32 s; s.u = (uint32_t)(k + 127) << 23;
    return p * s.f;
}

float st_logf(float x)
{
    if (x < 1e-30f) return -69.0f;
    ST_F32 v; v.f = x;
    int e = (int)((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFFU) | 0x3F800000U;
    float m  = v.f;
    float z  = (m - 1.0f) / (m + 1.0f);
    float z2 = z * z;
    float l  = 2.0f * z * (1.0f + z2 * (0.33333333f + z2 * (0.2f +
               z2 * (0.14285714f + z2 * 0.11111111f))));
    return l + (float)e * 0.69314718f;
}

static float st_sqrtf(float x)
{
    if (x <= 0.0f) return 0.0f;
    float r = x > 1.0f ? x * 0.5f : 1.0f;
    for (int i = 0; i < 6; i++) r = (r + x / r) * 0.5f;
    return r;
}

float st_rsqrtf(float x)
{
    float s = st_sqrtf(x);
    return s > 0.0f ? 1.0f / s : 0.0f;
}

static float st_silu(float x) { return x / (1.0f + st_expf(-x)); }
/* d/dx silu(x) = sig(x) * (1 + x*(1-sig(x))), sig = 1/(1+e^-x) */
static float st_silu_grad(float x)
{
    float sig = 1.0f / (1.0f + st_expf(-x));
    return sig * (1.0f + x * (1.0f - sig));
}

/* ---- C1 RoPE (scale_wall_design.md §8) -----------------------------------
 * Rotary positional encoding, SAME recipe as forward.c's rope_head/lm_sincosf
 * (the anti-fork one-math rule): pair (2j, 2j+1) rotated by theta =
 * pos * base^(-2j/D). base = 10000 (the student is its own model; 10000 is the
 * standard choice, and its ln is a compile-time literal so no per-call log).
 *
 * sin/cos via Payne-Hanek-lite range reduction (double intermediates) — the
 * angles get large (pos up to 255, freq up to 1). double math is IEEE-exact and
 * bit-identical across x86_64/aarch64 under -O1 -ffp-contract=off (forward.c's
 * lm_sincosf is already cross-arch oracle-certified with this recipe).         */
#define ST_ROPE_BASE 10000.0f
static const float ST_LN_ROPE_BASE = 9.210340371976182f;   /* ln(10000)        */

/* sin/cos of `a` into so and co (ported verbatim from forward.c lm_sincosf). */
static void st_sincosf(float a, float *so, float *co)
{
    double q = (double)a * 0.6366197723675814;         /* 2/pi                  */
    long   n = (long)(q + (q >= 0.0 ? 0.5 : -0.5));
    double r = (double)a
             - (double)n * 1.5707963109016418            /* pi/2 hi             */
             - (double)n * 1.5893254712640187e-08        /* pi/2 mid            */
             - (double)n * 6.123233995736766e-17;        /* pi/2 lo (~0)        */
    double r2 = r * r;
    double s = r * (1.0 + r2 * (-1.6666666664e-01 + r2 * (8.3333315e-03 +
               r2 * (-1.98412698e-04 + r2 * 2.7557314e-06))));
    double c = 1.0 + r2 * (-0.5 + r2 * (4.16666666e-02 + r2 * (-1.388731e-03 +
               r2 * 2.443315e-05)));
    float fs, fc;
    switch (((unsigned long)n) & 3u) {
        case 0:  fs = (float)s;    fc = (float)c;    break;
        case 1:  fs = (float)c;    fc = (float)(-s); break;
        case 2:  fs = (float)(-s); fc = (float)(-c); break;
        default: fs = (float)(-c); fc = (float)s;    break;
    }
    *so = fs; *co = fc;
}

/* RoPE enable toggle (test hook, like st_kv_set_enabled). Default ON: the
 * forward is EXACTLY the C1 model. The [ctx-carry] cert flips it OFF to train +
 * eval a NoPE twin for the MEASURED (printed, NOT gated) RoPE-vs-NoPE side-by-
 * side (§8: decoder-only nets can learn implicit position, so RoPE is not the
 * cert's bet — the WINDOW is). When OFF, st_rope_apply is a no-op in BOTH the
 * forward and the backward, so the NoPE model stays self-consistent. */
static int st_rope_enabled = 1;
void st_rope_set_enabled(int on) { st_rope_enabled = on ? 1 : 0; }
int  st_rope_get_enabled(void)   { return st_rope_enabled; }

/* Rotate the D-vector `h` in place by RoPE at position `pos`. ssign=+1.0 is the
 * FORWARD rotation R(theta); ssign=-1.0 is R(theta)^T = R(-theta), the exact
 * gradient transpose used in the backward (g_unrot = R^T g_rot). pos==0 is the
 * identity (theta=0 -> c=1,s=0 exactly) — early-returned, bit-identical. D odd
 * leaves the last element unrotated (never happens: all tiers have even D). */
static void st_rope_apply(float *h, int pos, int d, float ssign)
{
    if (!st_rope_enabled) return;                      /* NoPE twin (test hook) */
    if (pos == 0) return;                              /* theta 0 -> identity  */
    for (int j = 0; j < d / 2; j++) {
        float exponent = -2.0f * (float)j / (float)d;
        float freq  = st_expf(exponent * ST_LN_ROPE_BASE);
        float theta = (float)pos * freq;
        float s, c; st_sincosf(theta, &s, &c);
        s *= ssign;
        float x0 = h[2 * j], x1 = h[2 * j + 1];
        h[2 * j]     = x0 * c - x1 * s;
        h[2 * j + 1] = x0 * s + x1 * c;
    }
}

/* ================================================================== */
/* weight layout / arena                                               */
/* ================================================================== */

/* ---- SS-2: the model dims are RUNTIME (m->d/dff/nlayer/nexpert), the legacy
 * D/L/E/DFF #defines are GONE.  Each function brings the runtime dims into
 * scope with ST_DIMS(m) (below) so the math body reads them unchanged.  Every
 * STACK scratch array binds to the FIXED *MAX constants — never the runtime
 * value — which is the [no-vla] gate (§3.2).  V (byte vocab) and K_min are
 * tier-invariant, so they stay compile-time constants.                        */
#define V    ST_VOCAB            /* byte vocab — FIXED across tiers             */
#define K    ST_TOPK             /* K_min — the floor firing width (FIXED)      */

/* fixed stack-scratch ceilings (== the L tier).  ALL stack arrays size by these. */
#define DMAX   ST_D_MAX
#define DFFMAX ST_DFF_MAX
#define LMAX   ST_L_MAX
#define EMAX   ST_E_MAX
#define KMAX   ST_KMAX          /* == ST_E_MAX: per-token K scratch ceiling     */

/* the MAX constants MUST equal the L tier (the table's largest), or a scratch
 * array could be smaller than a runtime dim — the VLA-equivalent overflow. */
_Static_assert(ST_D_MAX   == ST_D_L,   "ST_D_MAX must == the L tier d_model");
_Static_assert(ST_DFF_MAX == ST_DFF_L, "ST_DFF_MAX must == the L tier dff");
_Static_assert(ST_L_MAX   == ST_L_L,   "ST_L_MAX must == the L tier layers");
_Static_assert(ST_E_MAX   == ST_E_L,   "ST_E_MAX must == the L tier experts");
/* every tier's dims must fit under the MAX (else its scratch would overflow). */
_Static_assert(ST_D_S   <= ST_D_MAX   && ST_D_M   <= ST_D_MAX,   "S/M d <= MAX");
_Static_assert(ST_DFF_S <= ST_DFF_MAX && ST_DFF_M <= ST_DFF_MAX, "S/M dff <= MAX");
_Static_assert(ST_L_S   <= ST_L_MAX   && ST_L_M   <= ST_L_MAX,   "S/M L <= MAX");
_Static_assert(ST_E_S   <= ST_E_MAX   && ST_E_M   <= ST_E_MAX,   "S/M E <= MAX");
_Static_assert(KMAX <= ST_E_MAX, "K_MAX must not exceed the L-tier expert count");
_Static_assert(K <= ST_E_S, "K_min must not exceed even the SMALLEST tier's E");

/* Bring the resident model's runtime dims into local scope.  The math body then
 * reads D/L/E/DFF exactly as before, but they are now per-model integers.  V is
 * the file-scope #define (tier-invariant).  Use at the top of every fn that
 * touches the layout. */
#define ST_DIMS(m) \
    const int D   = (m)->d;       (void)D;   \
    const int DFF = (m)->dff;     (void)DFF; \
    const int L   = (m)->nlayer;  (void)L;   \
    const int E   = (m)->nexpert; (void)E

/* sizes of each weight family (in floats) — read the in-scope runtime D/L/E/DFF
 * (NOT the MAX): the arena is heap, sized exactly to the resident tier. */
#define SZ_EMBED     (V * D)
#define SZ_ANORM     (L * D)
#define SZ_W         (L * D * D)        /* one of Wq/Wk/Wv/Wo */
#define SZ_FNORM     (L * D)
#define SZ_ROUTER    (L * E * D)
#define SZ_W1        (L * E * DFF * D)  /* gate */
#define SZ_W3        (L * E * DFF * D)  /* up   */
#define SZ_W2        (L * E * D * DFF)  /* down */
#define SZ_ONORM     (D)
#define SZ_OUT       (V * D)

/* ---------- forward activation cache ---------- */
typedef struct {
    int   n;                 /* sequence length cached                     */
    /* residual stream entering each layer + final, [L+1][n][D] flattened. */
    float *resid;            /* resid[(l*n + t)*D + i]                      */
    /* per-layer attention internals (needed for backward) */
    float *a_in;             /* [L][n][D]  rmsnorm(resid_l)                */
    float *a_rstd;           /* [L][n]     1/rms of attn-norm input        */
    float *q, *k, *v;        /* [L][n][D] each                            */
    float *attn_w;           /* [L][n][n]  softmax weights (causal, lower) */
    float *attn_o;           /* [L][n][D]  attention output (pre-Wo)       */
    /* per-layer ffn internals */
    float *attn_resid;       /* [L][n][D]  rout_pre = rin + Wo attn_o      */
                             /* (the x that ffn-rmsnorm read; resid[l+1]   */
                             /*  is overwritten with rout_pre+moe)         */
    float *f_in;             /* [L][n][D]  rmsnorm(resid after attn)       */
    float *f_rstd;           /* [L][n]                                     */
    float *gate;             /* [L][n][E]  router logits                   */
    int   *topk_e;           /* [L][n][E]  chosen expert ids (E-slot, heap) */
    float *topk_w;           /* [L][n][E]  softmax weight over chosen       */
    int   *topk_n;           /* [L][n]     runtime firing width (K..E)      */
    float *e_g, *e_u, *e_h;  /* [L][n][E][DFF] gate/up/h (E-slot, heap)     */
    /* output */
    float *o_in;             /* [n][D]  rmsnorm(final resid)               */
    float *o_rstd;           /* [n]                                        */
    float *probs;            /* [n][V] softmax(logits)                     */
} st_cache;

/* total floats for the cache (function of n AND the resident tier's dims).  The
 * cache lives in the malloc'd arena, so it is sized to the RUNTIME dims (heap,
 * bounded by the tier) — NOT a stack array, so no VLA concern.  The per-token K
 * slot stride is the model's own E (a token fires at most E experts). */
static size_t cache_floats(const st_model *m, int n)
{
    ST_DIMS(m);
    size_t s = 0;
    s += (size_t)(L + 1) * n * D;      /* resid    */
    s += (size_t)L * n * D;            /* a_in     */
    s += (size_t)L * n;                /* a_rstd   */
    s += (size_t)L * n * D * 3;        /* q,k,v    */
    s += (size_t)L * n * n;            /* attn_w   */
    s += (size_t)L * n * D;            /* attn_o   */
    s += (size_t)L * n * D;            /* attn_resid */
    s += (size_t)L * n * D;            /* f_in     */
    s += (size_t)L * n;                /* f_rstd   */
    s += (size_t)L * n * E;            /* gate     */
    s += (size_t)L * n * E * DFF * 3;  /* e_g,e_u,e_h (E-slot, runtime/heap) */
    s += (size_t)n * D;                /* o_in     */
    s += (size_t)n;                    /* o_rstd   */
    s += (size_t)n * V;                /* probs    */
    return s;
}
/* topk_e (E-slot) + topk_n (per-token width): ints L*n*E + L*n. */
static size_t cache_ints(const st_model *m, int n)
{
    ST_DIMS(m);
    return (size_t)L * n * E + (size_t)L * n;
}

/* ================================================================== */
/* tiny host allocator shim (libc-light: one malloc/free for the arena) */
/* ================================================================== */
#include <stdlib.h>

/* ================================================================== */
/* init                                                               */
/* ================================================================== */

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }
/* uniform in [-a, a) */
static float runi(uint32_t *s, float a)
{
    uint32_t r = lcg(s);
    float u = (float)(r >> 8) * (1.0f / 16777216.0f);  /* [0,1) */
    return (u * 2.0f - 1.0f) * a;
}

/* ---- the const tier table (SS-2, §3.2): {d, dff, nlayer, nexpert} per tier.
 * Indexed by ST_TIER_S/_M/_L.  M reproduces the legacy dims exactly. */
typedef struct { int d, dff, nlayer, nexpert; } st_tier_dims;
static const st_tier_dims ST_TIERS[ST_NTIER] = {
    /* S */ { ST_D_S, ST_DFF_S, ST_L_S, ST_E_S },
    /* M */ { ST_D_M, ST_DFF_M, ST_L_M, ST_E_M },
    /* L */ { ST_D_L, ST_DFF_L, ST_L_L, ST_E_L },
};

int st_init(st_model *m, uint32_t seed)
{
    /* DEFAULT tier == M: byte-identical to the pre-SS-2 baby (all callers). */
    return st_init_tier(m, seed, ST_TIER_DEFAULT);
}

int st_init_tier(st_model *m, uint32_t seed, int tier)
{
    if (!m) return ST_E_ARG;
    /* out-of-range tier fails SAFE to the default (never undersizes scratch). */
    if (tier < 0 || tier >= ST_NTIER) tier = ST_TIER_DEFAULT;

    for (int i = 0; i < (int)sizeof(*m); i++) ((char *)m)[i] = 0;

    /* select the resident tier's dims (runtime, read by every loop below). */
    m->tier    = tier;
    m->d       = ST_TIERS[tier].d;
    m->dff     = ST_TIERS[tier].dff;
    m->nlayer  = ST_TIERS[tier].nlayer;
    m->nexpert = ST_TIERS[tier].nexpert;
    ST_DIMS(m);   /* D/DFF/L/E now in scope for SZ_* below */

    /* assign offsets */
    int o = 0;
    m->o_embed     = o; o += SZ_EMBED;
    m->o_attn_norm = o; o += SZ_ANORM;
    m->o_wq        = o; o += SZ_W;
    m->o_wk        = o; o += SZ_W;
    m->o_wv        = o; o += SZ_W;
    m->o_wo        = o; o += SZ_W;
    m->o_ffn_norm  = o; o += SZ_FNORM;
    m->o_router    = o; o += SZ_ROUTER;
    m->o_w1        = o; o += SZ_W1;
    m->o_w3        = o; o += SZ_W3;
    m->o_w2        = o; o += SZ_W2;
    m->o_out_norm  = o; o += SZ_ONORM;
    m->o_out       = o; o += SZ_OUT;
    m->n_params = o;

    size_t bytes = (size_t)m->n_params * 4 * sizeof(float);
    float *base = (float *)malloc(bytes);
    if (!base) return ST_E_OOM;
    m->mem = base;
    m->w  = base;
    m->g  = base + m->n_params;
    m->mu = base + (size_t)m->n_params * 2;
    m->vu = base + (size_t)m->n_params * 3;
    for (size_t i = 0; i < (size_t)m->n_params * 4; i++) base[i] = 0.0f;

    uint32_t s = seed ? seed : 0xBABE0001u;

    /* embeddings + output head: small random */
    for (int i = 0; i < SZ_EMBED; i++) m->w[m->o_embed + i] = runi(&s, 0.02f);
    for (int i = 0; i < SZ_OUT;   i++) m->w[m->o_out   + i] = runi(&s, 0.02f);

    /* RMSNorm gains = 1 */
    for (int i = 0; i < SZ_ANORM; i++) m->w[m->o_attn_norm + i] = 1.0f;
    for (int i = 0; i < SZ_FNORM; i++) m->w[m->o_ffn_norm  + i] = 1.0f;
    for (int i = 0; i < SZ_ONORM; i++) m->w[m->o_out_norm  + i] = 1.0f;

    /* attention projections: scaled-uniform ~ 1/sqrt(D) */
    float aw = 1.0f / st_sqrtf((float)D);
    for (int i = 0; i < SZ_W; i++) m->w[m->o_wq + i] = runi(&s, aw);
    for (int i = 0; i < SZ_W; i++) m->w[m->o_wk + i] = runi(&s, aw);
    for (int i = 0; i < SZ_W; i++) m->w[m->o_wv + i] = runi(&s, aw);
    for (int i = 0; i < SZ_W; i++) m->w[m->o_wo + i] = runi(&s, aw);

    /* router: small so the baby starts roughly load-balanced */
    for (int i = 0; i < SZ_ROUTER; i++) m->w[m->o_router + i] = runi(&s, 0.02f);

    /* experts */
    float fw = 1.0f / st_sqrtf((float)D);
    float dw = 1.0f / st_sqrtf((float)DFF);
    for (int i = 0; i < SZ_W1; i++) m->w[m->o_w1 + i] = runi(&s, fw);
    for (int i = 0; i < SZ_W3; i++) m->w[m->o_w3 + i] = runi(&s, fw);
    for (int i = 0; i < SZ_W2; i++) m->w[m->o_w2 + i] = runi(&s, dw);

    /* cache will be (re)allocated lazily in st_forward for the seq length. */
    m->cache = NULL;
    return ST_OK;
}

void st_free(st_model *m)
{
    if (!m) return;
    if (m->cache) { free(m->cache); m->cache = NULL; }
    if (m->mem)   { free(m->mem);   m->mem = NULL; }
    if (m->alive) { free(m->alive); m->alive = NULL; }  /* SS-4 liveness mask */
    m->w = m->g = m->mu = m->vu = NULL;
}

/* ================================================================== */
/* SS-4 — function-preserving expert growth (add DEAD experts)         */
/*   ss4-function-preserving-growth-plan.md §1                          */
/* ================================================================== */

/* Recompute the o_* offsets for an expert count `E` (the SAME assignment
 * st_init_tier does; D/DFF/L taken from the model).  Writes the offsets into the
 * out struct WITHOUT touching m, and returns n_params.  Used to lay out both the
 * old (E_old) and the new (E_new) arenas so growth copies block-by-block. */
typedef struct {
    int o_embed, o_attn_norm, o_wq, o_wk, o_wv, o_wo, o_ffn_norm;
    int o_router, o_w1, o_w3, o_w2, o_out_norm, o_out, n_params;
} st_layout;

static int st_layout_for(const st_model *m, int E, st_layout *L_out)
{
    const int D = m->d, DFF = m->dff, L = m->nlayer;
    int o = 0;
    L_out->o_embed     = o; o += V * D;
    L_out->o_attn_norm = o; o += L * D;
    L_out->o_wq        = o; o += L * D * D;
    L_out->o_wk        = o; o += L * D * D;
    L_out->o_wv        = o; o += L * D * D;
    L_out->o_wo        = o; o += L * D * D;
    L_out->o_ffn_norm  = o; o += L * D;
    L_out->o_router    = o; o += L * E * D;
    L_out->o_w1        = o; o += L * E * DFF * D;
    L_out->o_w3        = o; o += L * E * DFF * D;
    L_out->o_w2        = o; o += L * E * D * DFF;
    L_out->o_out_norm  = o; o += D;
    L_out->o_out       = o; o += V * D;
    L_out->n_params    = o;
    return o;
}

/* Copy one of the four per-layer expert families from src (E_old strides) into
 * dst (E_new strides): for each layer, copy the first E_old expert slices
 * verbatim, leaving slices [E_old, E_new) as the caller's pre-zeroed dst (DEAD).
 * `per_e` = floats per (layer,expert) block (D for router, DFF*D for w1/w3,
 * D*DFF for w2). */
static void st_copy_expert_family(float *dst, const float *src,
                                  int L, int E_old, int E_new, int per_e)
{
    for (int l = 0; l < L; l++) {
        const float *s = src + (size_t)l * E_old * per_e;
        float       *d = dst + (size_t)l * E_new * per_e;
        for (int k = 0; k < E_old * per_e; k++) d[k] = s[k];  /* first E_old kept */
        /* slices [E_old,E_new) stay 0 (dst pre-zeroed) — the DEAD W2 / router    */
    }
}

PK_NOSW_FN(int, st_grow_experts, (st_model *m, int e_new), (m, e_new))
{
    if (!m || !m->w) return ST_E_ARG;
    const int E_old = m->nexpert;
    /* grow within [E_old, ST_E_MAX]: never below the resident count, never past
     * the fixed L-tier scratch ceiling (the [no-vla] gate / open-risk #7). */
    if (e_new < E_old || e_new > ST_E_MAX) return ST_E_ARG;
    if (e_new == E_old) {
        /* [grow-noop-identity] no-op: ensure an all-ones alive[] exists so the
         * mask path is exercised, but the FORWARD stays byte-identical (every
         * incumbent alive == the all-alive default). */
        if (!m->alive) {
            int8_t *a = (int8_t *)malloc((size_t)E_old);
            if (!a) return ST_E_OOM;
            for (int e = 0; e < E_old; e++) a[e] = 1;
            m->alive = a;
        }
        return ST_OK;
    }

    const int D = m->d, DFF = m->dff, L = m->nlayer;

    /* old + new layouts (offsets recomputed exactly as st_init_tier). */
    st_layout lo, ln;
    st_layout_for(m, E_old, &lo);
    int np_new = st_layout_for(m, e_new, &ln);

    /* new arena: w | g | mu | vu, each np_new floats (same shape as st_init). */
    size_t bytes = (size_t)np_new * 4 * sizeof(float);
    float *base = (float *)malloc(bytes);
    if (!base) return ST_E_OOM;
    for (size_t i = 0; i < (size_t)np_new * 4; i++) base[i] = 0.0f;  /* DEAD slots stay 0 */

    int8_t *alive = (int8_t *)malloc((size_t)e_new);
    if (!alive) { free(base); return ST_E_OOM; }

    float *w_new  = base;
    float *g_new  = base + np_new;
    float *mu_new = base + (size_t)np_new * 2;
    float *vu_new = base + (size_t)np_new * 3;

    const float *w_old  = m->w;
    const float *g_old  = m->g;
    const float *mu_old = m->mu;
    const float *vu_old = m->vu;

    /* --- E-INDEPENDENT families: bitwise copy (embed, attn_norm, Wq/Wk/Wv/Wo,
     * ffn_norm, out_norm, out).  Their offsets are IDENTICAL in lo and ln up to
     * o_router (they precede the expert families), so a single contiguous copy of
     * [0, o_router) reproduces them verbatim. */
    for (int i = 0; i < lo.o_router; i++) {
        w_new[i]  = w_old[i];
        g_new[i]  = g_old[i];
        mu_new[i] = mu_old[i];
        vu_new[i] = vu_old[i];
    }
    /* out_norm + out follow the expert families at DIFFERENT offsets in lo vs ln;
     * copy them by name. */
    for (int i = 0; i < D; i++) {
        w_new[ln.o_out_norm + i]  = w_old[lo.o_out_norm + i];
        g_new[ln.o_out_norm + i]  = g_old[lo.o_out_norm + i];
        mu_new[ln.o_out_norm + i] = mu_old[lo.o_out_norm + i];
        vu_new[ln.o_out_norm + i] = vu_old[lo.o_out_norm + i];
    }
    for (int i = 0; i < V * D; i++) {
        w_new[ln.o_out + i]  = w_old[lo.o_out + i];
        g_new[ln.o_out + i]  = g_old[lo.o_out + i];
        mu_new[ln.o_out + i] = mu_old[lo.o_out + i];
        vu_new[ln.o_out + i] = vu_old[lo.o_out + i];
    }

    /* --- EXPERT families: copy the E_old incumbent slices into the E_new strides
     * (new slices [E_old,e_new) stay 0 = DEAD W2 + DEAD router row).  Done for w,
     * g, mu, vu so incumbents' Adam moments survive at the new strides (§1.1.4);
     * the new slots' moments start at 0. */
    st_copy_expert_family(w_new  + ln.o_router, w_old  + lo.o_router, L, E_old, e_new, D);
    st_copy_expert_family(g_new  + ln.o_router, g_old  + lo.o_router, L, E_old, e_new, D);
    st_copy_expert_family(mu_new + ln.o_router, mu_old + lo.o_router, L, E_old, e_new, D);
    st_copy_expert_family(vu_new + ln.o_router, vu_old + lo.o_router, L, E_old, e_new, D);

    st_copy_expert_family(w_new  + ln.o_w1, w_old  + lo.o_w1, L, E_old, e_new, DFF * D);
    st_copy_expert_family(g_new  + ln.o_w1, g_old  + lo.o_w1, L, E_old, e_new, DFF * D);
    st_copy_expert_family(mu_new + ln.o_w1, mu_old + lo.o_w1, L, E_old, e_new, DFF * D);
    st_copy_expert_family(vu_new + ln.o_w1, vu_old + lo.o_w1, L, E_old, e_new, DFF * D);

    st_copy_expert_family(w_new  + ln.o_w3, w_old  + lo.o_w3, L, E_old, e_new, DFF * D);
    st_copy_expert_family(g_new  + ln.o_w3, g_old  + lo.o_w3, L, E_old, e_new, DFF * D);
    st_copy_expert_family(mu_new + ln.o_w3, mu_old + lo.o_w3, L, E_old, e_new, DFF * D);
    st_copy_expert_family(vu_new + ln.o_w3, vu_old + lo.o_w3, L, E_old, e_new, DFF * D);

    st_copy_expert_family(w_new  + ln.o_w2, w_old  + lo.o_w2, L, E_old, e_new, D * DFF);
    st_copy_expert_family(g_new  + ln.o_w2, g_old  + lo.o_w2, L, E_old, e_new, D * DFF);
    st_copy_expert_family(mu_new + ln.o_w2, mu_old + lo.o_w2, L, E_old, e_new, D * DFF);
    st_copy_expert_family(vu_new + ln.o_w2, vu_old + lo.o_w2, L, E_old, e_new, D * DFF);

    /* --- DEAD-expert warm start (§1.2): clone the busiest INCUMBENT's W1/W3 into
     * each new slot (latent — contributes nothing while W2=0 + alive=0; a warm
     * start for the SEPARATE, deliberately-ε resurrection step).  Router row and
     * W2 of the new slots STAY 0 (defense-in-depth); the alive mask is what makes
     * preservation exact.  "Busiest" here = incumbent 0 (a deterministic choice;
     * any incumbent is a valid latent seed — the DMN picks the real source at
     * resurrection).  This does NOT affect the forward (W2=0, alive=0). */
    for (int l = 0; l < L; l++) {
        const int src = 0;  /* deterministic incumbent seed */
        const float *w1_src = w_new + ln.o_w1 + ((size_t)l * e_new + src) * DFF * D;
        const float *w3_src = w_new + ln.o_w3 + ((size_t)l * e_new + src) * DFF * D;
        for (int e = E_old; e < e_new; e++) {
            float *w1_d = w_new + ln.o_w1 + ((size_t)l * e_new + e) * DFF * D;
            float *w3_d = w_new + ln.o_w3 + ((size_t)l * e_new + e) * DFF * D;
            for (int k = 0; k < DFF * D; k++) { w1_d[k] = w1_src[k]; w3_d[k] = w3_src[k]; }
        }
    }

    /* alive[]: incumbents alive, new experts DEAD (the exact never-admit flag). */
    for (int e = 0; e < E_old; e++) alive[e] = 1;
    for (int e = E_old; e < e_new; e++) alive[e] = 0;

    /* commit: swap the arena + bump the count.  Free the old arena + any old
     * alive[].  The forward cache is sized by E, so drop it — st_forward
     * re-allocates it lazily at the new E (cache_get note, :319). */
    free(m->mem);
    if (m->alive) free(m->alive);
    if (m->cache) { free(m->cache); m->cache = NULL; }
    m->mem     = base;
    m->w       = w_new;
    m->g       = g_new;
    m->mu      = mu_new;
    m->vu      = vu_new;
    m->alive   = alive;
    m->nexpert = e_new;
    m->n_params = np_new;
    /* re-stamp the o_* offsets to the new (E_new) layout. */
    m->o_embed=ln.o_embed; m->o_attn_norm=ln.o_attn_norm;
    m->o_wq=ln.o_wq; m->o_wk=ln.o_wk; m->o_wv=ln.o_wv; m->o_wo=ln.o_wo;
    m->o_ffn_norm=ln.o_ffn_norm; m->o_router=ln.o_router;
    m->o_w1=ln.o_w1; m->o_w3=ln.o_w3; m->o_w2=ln.o_w2;
    m->o_out_norm=ln.o_out_norm; m->o_out=ln.o_out;
    return ST_OK;
}

/* ensure cache is allocated for sequence length n. */
static st_cache *cache_get(st_model *m, int n)
{
    ST_DIMS(m);
    /* layout: a struct header followed by a float pool then an int pool. */
    size_t hdr   = sizeof(st_cache);
    size_t fpool = cache_floats(m, n) * sizeof(float);
    size_t ipool = cache_ints(m, n)   * sizeof(int);
    /* (re)alloc on demand; NS-1 always uses one n, so this happens once. */
    if (m->cache) { free(m->cache); m->cache = NULL; }
    char *blob = (char *)malloc(hdr + fpool + ipool);
    if (!blob) return NULL;
    st_cache *c = (st_cache *)blob;
    float *fp = (float *)(blob + hdr);
    int   *ip = (int *)(blob + hdr + fpool);
    c->n = n;
    size_t off = 0;
    c->resid  = fp + off; off += (size_t)(L + 1) * n * D;
    c->a_in   = fp + off; off += (size_t)L * n * D;
    c->a_rstd = fp + off; off += (size_t)L * n;
    c->q      = fp + off; off += (size_t)L * n * D;
    c->k      = fp + off; off += (size_t)L * n * D;
    c->v      = fp + off; off += (size_t)L * n * D;
    c->attn_w = fp + off; off += (size_t)L * n * n;
    c->attn_o = fp + off; off += (size_t)L * n * D;
    c->attn_resid = fp + off; off += (size_t)L * n * D;
    c->f_in   = fp + off; off += (size_t)L * n * D;
    c->f_rstd = fp + off; off += (size_t)L * n;
    c->gate   = fp + off; off += (size_t)L * n * E;
    c->e_g    = fp + off; off += (size_t)L * n * E * DFF;
    c->e_u    = fp + off; off += (size_t)L * n * E * DFF;
    c->e_h    = fp + off; off += (size_t)L * n * E * DFF;
    c->o_in   = fp + off; off += (size_t)n * D;
    c->o_rstd = fp + off; off += (size_t)n;
    c->probs  = fp + off; off += (size_t)n * V;
    c->topk_e = ip;                              /* [L][n][E] expert ids    */
    c->topk_n = ip + (size_t)L * n * E;          /* [L][n] runtime width    */
    /* topk_w lives in a separate static-grown float buffer (set in st_forward,
     * sized L*n*KMAX) so it survives the per-FD cache realloc in grad-check. */
    c->topk_w = NULL; /* set below */
    (void)hdr;
    m->cache = c;
    return c;
}

/* ================================================================== */
/* small linear helpers                                               */
/* ================================================================== */

/* y[M] = W[M*N] . x[N] */
static void mv(const float *W, const float *x, float *y, int M, int N)
{
    for (int r = 0; r < M; r++) {
        const float *Wr = W + (size_t)r * N;
        float acc = 0.0f;
        for (int c = 0; c < N; c++) acc += Wr[c] * x[c];
        y[r] = acc;
    }
}
/* gx[N] += W^T[N*M] . gy[M] ; gW[M*N] += gy outer x */
static void mv_bwd(const float *W, const float *x, const float *gy,
                   float *gx, float *gW, int M, int N)
{
    for (int r = 0; r < M; r++) {
        float g = gy[r];
        if (g == 0.0f) continue;
        const float *Wr = W + (size_t)r * N;
        float *gWr = gW + (size_t)r * N;
        for (int c = 0; c < N; c++) {
            gx[c]  += Wr[c] * g;
            gWr[c] += g * x[c];
        }
    }
}

/* RMSNorm forward: y = (x / rms) * gain ; returns 1/rms. rms = sqrt(mean(x^2)+eps) */
static float rmsnorm_fwd(const float *x, const float *gain, float *y, int d)
{
    float ss = 0.0f;
    for (int i = 0; i < d; i++) ss += x[i] * x[i];
    float rstd = st_rsqrtf(ss / (float)d + 1e-5f);
    for (int i = 0; i < d; i++) y[i] = x[i] * rstd * gain[i];
    return rstd;
}
/* RMSNorm backward. Given gy (grad wrt y), x, gain, rstd: accumulate into gx,
 * ggain. d(y_i)/d(x_j): y_i = x_i*rstd*g_i, rstd = (ss/d+eps)^-1/2,
 * d rstd/d x_j = -rstd^3/d * x_j. */
static void rmsnorm_bwd(const float *x, const float *gain, float rstd,
                        const float *gy, float *gx, float *ggain, int d)
{
    /* ggain_i += gy_i * x_i * rstd */
    float dot = 0.0f;       /* sum_i gy_i * gain_i * x_i */
    for (int i = 0; i < d; i++) {
        ggain[i] += gy[i] * x[i] * rstd;
        dot += gy[i] * gain[i] * x[i];
    }
    float coef = rstd * rstd * rstd / (float)d * dot;
    for (int i = 0; i < d; i++)
        gx[i] += gy[i] * gain[i] * rstd - coef * x[i];
}

/* ================================================================== */
/* forward                                                            */
/* ================================================================== */

/* router: compute gate logits, ADAPTIVE-K pick (SS-1, special-structure-mind.md
 * §4), softmax-normalize over the chosen.  An EASY token (one expert dominates
 * -> big router margin) fires K_min experts; a HARD/ambiguous token (flat gate
 * -> small margin) widens toward K_MAX = E.  Writes chosen ids (topk_e[KMAX]),
 * softmax weights (topk_w[KMAX]), and returns the runtime firing width nk
 * (K_min..K_MAX).  Hardness signal = router MARGIN gate[top0]-gate[topj], the
 * cheapest order-stable signal under -ffp-contract=off (no transcendental in
 * the comparison, so (weights,bytes) -> identical nk on every target).
 *
 * When st_freeze_routing is set, router_pick REPLAYS the frozen ids AND the
 * frozen width instead of re-deriving them (the discontinuous selection is
 * non-differentiable; the analytic gradient is the derivative AT FIXED routing,
 * which is what an Adam step actually uses).  The softmax weights are still
 * recomputed from the current logits so they remain differentiable. */
static int   st_freeze_routing = 0;
static int  *st_frozen_route = NULL;  /* [L*n*KMAX] ids snapshot (survives realloc) */
static int  *st_frozen_n     = NULL;  /* [L*n]      width snapshot                  */
static int   st_frozen_pos = 0;       /* running index as forward visits (l,t)      */

/* ---- firing-width observability (SS-1) — read-only via st_last_fire_width().
 * Updated at the END of each st_forward from the cached per-token widths. */
static int   st_fw_last  = 0;   /* width of the final token's final layer        */
static int   st_fw_milli = 0;   /* mean width across all (l,t), x1000 (libc-free) */
static int   st_fw_experts[ST_KMAX]; /* final token's final-layer chosen expert ids */

/* ---- SS-6 cross-node expert firing (special-structure-mind.md §5) ----------
 * The remote-expert transport + gating predicate are CALLER-installed (the
 * kernel wires DRPC; the cert wires a stub). When EITHER is NULL the MoE loop
 * is byte-identical to the pre-SS-6 single-node forward.  Observability counts
 * are reset at the top of each st_forward. */
static st_remote_expert_fn st_remote_fn   = NULL;
static st_remote_gate_fn   st_remote_gate = NULL;
static void               *st_remote_ctx  = NULL;
static int                 st_remote_fired_cnt    = 0;  /* expert outputs from a peer */
static int                 st_remote_fallback_cnt = 0;  /* remote refused -> local    */

void st_set_remote_expert(st_remote_expert_fn fn, st_remote_gate_fn gate,
                          void *ctx)
{
    st_remote_fn   = fn;
    st_remote_gate = gate;
    st_remote_ctx  = ctx;
}
int st_last_remote_fired(void)    { return st_remote_fired_cnt; }
int st_last_remote_fallback(void) { return st_remote_fallback_cnt; }

/* ---- DMOE-A cross-node expert BANK (distributed_moe_design.md §2/§4) --------
 * The bank score/fire hooks are CALLER-installed (dmoe_bank.c wires the real
 * transport; the cert wires an in-process fleet). When st_dmoe_score is NULL or
 * st_dmoe_nbank==0 the joint-routing branch in st_forward is NEVER taken and the
 * MoE loop is byte-identical to the pre-DMOE forward (the [dmoe-bank-empty-
 * identity] gate). Counts reset at the top of each st_forward. */
static st_dmoe_score_fn st_dmoe_score = NULL;
static st_dmoe_fire_fn  st_dmoe_fire  = NULL;
static int              st_dmoe_nbank = 0;
static void            *st_dmoe_ctx   = NULL;
static int              st_bank_fired_cnt   = 0;  /* bank [D] outputs summed     */
static int              st_bank_dropped_cnt = 0;  /* selected-but-unreachable    */

void st_dmoe_install(st_dmoe_score_fn score, st_dmoe_fire_fn fire,
                     int nbank, void *ctx)
{
    if (nbank < 0) nbank = 0;
    if (nbank > ST_DMOE_FLEET_MAX) nbank = ST_DMOE_FLEET_MAX;
    st_dmoe_score = score;
    st_dmoe_fire  = fire;
    st_dmoe_nbank = (score && fire) ? nbank : 0;
    st_dmoe_ctx   = ctx;
}
int st_last_bank_fired(void)   { return st_bank_fired_cnt;   }
int st_last_bank_dropped(void) { return st_bank_dropped_cnt; }

/* DMOE joint router pick over ecand = E_res + nbank candidates (floor logits
 * first, then bank logits). MIRRORS router_pick (same selection sort + margin
 * widening + max-subtracted softmax, one math) but with scratch bound to the
 * FIXED ST_DMOE_CAND_MAX (candidate space) and the FIRED width capped at KMAX
 * (only the candidate space grows, §3.1; a bank logit <= -1e29f is an
 * unreachable/LOST expert, treated as effective -inf: never selected — the
 * exact SS-4 never-admit mechanism). Writes chosen ids (xe[KMAX]); returns the
 * width nk (K_min..min(selectable,KMAX)). The SOFTMAX is deliberately NOT
 * computed here — st_forward recomputes it over the SURVIVING slots after the
 * degrade ladder (§4.2), so it stays deterministic in the failure set. */
static int dmoe_router_pick(const float *gate, int *xe, int ecand)
{
    int order[ST_DMOE_CAND_MAX];
    int used[ST_DMOE_CAND_MAX];
    int n_sel = 0;
    if (ecand > ST_DMOE_CAND_MAX) ecand = ST_DMOE_CAND_MAX;
    for (int e = 0; e < ecand; e++) { used[e] = 0; if (gate[e] > -1e29f) n_sel++; }
    for (int r = 0; r < ecand; r++) {
        int best = -1; float bv = -1e30f;
        for (int e = 0; e < ecand; e++) {
            if (used[e]) continue;
            if (gate[e] <= -1e29f) continue;          /* LOST: effective -inf     */
            if (best < 0 || gate[e] > bv) { bv = gate[e]; best = e; }
        }
        if (best < 0) {                               /* only LOST cands remain   */
            for (int e = 0; e < ecand; e++)
                if (!used[e]) { order[r] = e; used[e] = 1; break; }
            continue;
        }
        order[r] = best; used[best] = 1;
    }
    int cap = n_sel < KMAX ? n_sel : KMAX;            /* fired width never > KMAX  */
    int nk  = K < cap ? K : cap;                      /* start at K_min           */
    float top1 = gate[order[0]];
    while (nk < cap && (top1 - gate[order[nk]]) < ST_K_THETA) nk++;
    for (int j = 0; j < nk; j++) xe[j] = order[j];
    return nk;
}

/* ---- SS-4 (ss4-function-preserving-growth-plan.md §1.3): the per-expert
 * liveness mask router_pick reads.  st_forward points this at the resident
 * model's m->alive (nexpert-sized) for the MoE loop, then clears it.  When it is
 * NULL — every pre-SS-4 / never-grown model — selection is BYTE-IDENTICAL to the
 * pre-SS-4 router_pick (the all-alive case): the skip branch below is never
 * taken, n_alive == ne, and the sort + widening + softmax execute the same float
 * ops in the same order.  A DEAD expert (alive[e]==0) has an EFFECTIVE LOGIT of
 * -inf: it is never `best` in the selection sort (sorts strictly AFTER every
 * alive expert), and the widening ceiling is capped at the alive count, so it is
 * PROVABLY never in order[0..nk) for ANY input ⇒ nk and every chosen tw[j] are
 * bit-unchanged by adding it.  This is the EXACT never-admit guarantee. */
static const int8_t *st_router_alive = NULL;   /* [ne] or NULL == all-alive */

/* `ne` = the resident model's expert count (m->nexpert) — the RUNTIME widening
 * ceiling (K_min..ne).  Scratch is bound to EMAX (fixed L-tier) — no VLA.  The
 * frozen-route slot stride is ne (the cache's E-slot stride; see cache_get). */
static int router_pick(const float *gate, int *topk_e, float *topk_w, int ne)
{
    const int8_t *alive = st_router_alive;     /* NULL == all-alive (pre-SS-4) */

    /* full descending order of the ne experts by gate logit (E tiny: selection
     * sort).  order[0] = top1.  Scratch bound to EMAX (compile-time) — no VLA.
     * DEAD experts (alive[e]==0) are treated as effective logit -inf: they are
     * never chosen as `best`, so they sort strictly after every alive expert. */
    int order[EMAX];
    int used[EMAX];
    int n_alive = 0;
    for (int e = 0; e < ne; e++) { used[e] = 0; if (!alive || alive[e]) n_alive++; }
    for (int r = 0; r < ne; r++) {
        int best = -1; float bv = -1e30f;
        for (int e = 0; e < ne; e++) {
            if (used[e]) continue;
            if (alive && !alive[e]) continue;      /* DEAD: effective -inf      */
            if (best < 0 || gate[e] > bv) { bv = gate[e]; best = e; }
        }
        if (best < 0) {                            /* only DEAD experts remain  */
            /* park the remaining DEAD experts at the tail of order[] in ascending
             * id (deterministic; they are never selected so the order is inert). */
            for (int e = 0; e < ne; e++)
                if (!used[e]) { order[r] = e; used[e] = 1; break; }
            continue;
        }
        order[r] = best; used[best] = 1;
    }

    int nk;
    if (st_freeze_routing && st_frozen_route && st_frozen_n) {
        /* replay the exact frozen selection (ids + width) for FD probes */
        nk = st_frozen_n[st_frozen_pos];
        for (int j = 0; j < nk; j++)
            topk_e[j] = st_frozen_route[st_frozen_pos * ne + j];
        st_frozen_pos++;
    } else {
        /* margin widening: start at K_min, admit the next expert while the gap
         * from top1 to it is below THETA and we are under the ALIVE-expert count
         * (a DEAD expert at order[nk] has effective logit -inf — never admitted;
         * capping at n_alive is the exact analogue and keeps the all-alive path,
         * where n_alive == ne, byte-identical). */
        nk = K;
        float top1 = gate[order[0]];
        while (nk < n_alive && (top1 - gate[order[nk]]) < ST_K_THETA) nk++;
        for (int j = 0; j < nk; j++) topk_e[j] = order[j];
    }

    /* softmax over the nk chosen logits (max-subtracted, libc-free expf) */
    float mx = -1e30f;
    for (int j = 0; j < nk; j++) if (gate[topk_e[j]] > mx) mx = gate[topk_e[j]];
    float sum = 0.0f;
    for (int j = 0; j < nk; j++) { topk_w[j] = st_expf(gate[topk_e[j]] - mx); sum += topk_w[j]; }
    if (sum < 1e-20f) sum = 1e-20f;
    for (int j = 0; j < nk; j++) topk_w[j] /= sum;
    return nk;
}

/* TEST HOOK: exercise the EXACT production selection on a crafted gate of length
 * ST_NEXPERT (the M-tier expert count — the SS-1 cert's contract; the gate
 * vector the caller supplies is ST_NEXPERT long).  Scratch bound to KMAX. */
int st_router_pick_width(const float *gate, int *chosen)
{
    float w[KMAX];
    int   was_freeze = st_freeze_routing;
    st_freeze_routing = 0;                 /* never replay frozen routing here */
    int nk = router_pick(gate, chosen, w, ST_NEXPERT);
    st_freeze_routing = was_freeze;
    return nk;
}

PK_NOSW_FN(int, st_forward, (st_model *m, const uint8_t *bytes, int n, float *logits),
           (m, bytes, n, logits))
{
    ST_DIMS(m);
    if (n < 1 || n > ST_MAXSEQ) return ST_E_ARG;
    st_cache *c = cache_get(m, n);
    if (!c) return ST_E_OOM;
    /* topk_w needs L*n*E floats (E-slot per token; heap, no-VLA) in a separate
     * static-grown buffer so it survives the per-FD cache realloc in grad-check. */
    static float *tw_buf = NULL; static size_t tw_cap = 0;
    size_t tw_need = (size_t)L * n * E;
    if (tw_need > tw_cap) { free(tw_buf); tw_buf = (float *)malloc(tw_need * sizeof(float)); tw_cap = tw_need; }
    if (!tw_buf) return ST_E_OOM;
    c->topk_w = tw_buf;
    st_frozen_pos = 0;   /* frozen routing replays in (l,t) visit order */
    st_remote_fired_cnt = 0; st_remote_fallback_cnt = 0;  /* SS-6 per-forward */
    st_bank_fired_cnt = 0; st_bank_dropped_cnt = 0;       /* DMOE per-forward */
    /* DMOE-A joint-routing branch: active iff a bank is installed AND non-empty.
     * When inactive EVERY line below is byte-identical to the pre-DMOE forward. */
    const int dmoe_active = (st_dmoe_score && st_dmoe_fire && st_dmoe_nbank > 0);
    long dmoe_sumw = 0;                 /* Σ fired width over (l,t) for milli    */
    int  dmoe_last_nk = 0;              /* final-token final-layer fired width   */
    int  dmoe_last_e[KMAX];             /* final-token final-layer chosen ids    */
    for (int j = 0; j < KMAX; j++) dmoe_last_e[j] = -1;
    /* SS-4: point router_pick at this model's liveness mask (NULL == all-alive,
     * byte-identical to pre-SS-4).  st_forward is single-threaded / non-reentrant
     * (shares tw_buf / st_frozen_pos), so a file-static pointer is safe. */
    st_router_alive = m->alive;

    const float *W  = m->w;
    /* embed: resid[layer 0] */
    float *r0 = c->resid;  /* [n][D] for layer 0 */
    for (int t = 0; t < n; t++) {
        const float *emb = W + m->o_embed + (size_t)bytes[t] * D;
        for (int i = 0; i < D; i++) r0[(size_t)t * D + i] = emb[i];
    }

    float scale = st_rsqrtf((float)D);
    for (int l = 0; l < L; l++) {
        float *rin  = c->resid + (size_t)l * n * D;       /* layer l input  */
        float *rout = c->resid + (size_t)(l + 1) * n * D; /* layer l output */
        const float *anorm = W + m->o_attn_norm + (size_t)l * D;
        const float *fnorm = W + m->o_ffn_norm  + (size_t)l * D;
        const float *Wq = W + m->o_wq + (size_t)l * D * D;
        const float *Wk = W + m->o_wk + (size_t)l * D * D;
        const float *Wv = W + m->o_wv + (size_t)l * D * D;
        const float *Wo = W + m->o_wo + (size_t)l * D * D;

        /* ---- attention ---- */
        float *a_in = c->a_in + (size_t)l * n * D;
        float *qL = c->q + (size_t)l * n * D;
        float *kL = c->k + (size_t)l * n * D;
        float *vL = c->v + (size_t)l * n * D;
        for (int t = 0; t < n; t++) {
            const float *x = rin + (size_t)t * D;
            float *ain = a_in + (size_t)t * D;
            c->a_rstd[(size_t)l * n + t] = rmsnorm_fwd(x, anorm, ain, D);
            mv(Wq, ain, qL + (size_t)t * D, D, D);
            mv(Wk, ain, kL + (size_t)t * D, D, D);
            mv(Wv, ain, vL + (size_t)t * D, D, D);
            /* C1 RoPE: rotate q,k (NOT v) by absolute position t before the
             * causal score. The cached qL/kL are the ROTATED vectors, so the
             * attention loop below and the backward read them consistently. */
            st_rope_apply(qL + (size_t)t * D, t, D, +1.0f);
            st_rope_apply(kL + (size_t)t * D, t, D, +1.0f);
        }
        float *attn_w = c->attn_w + (size_t)l * n * n;
        float *attn_o = c->attn_o + (size_t)l * n * D;
        for (int t = 0; t < n; t++) {
            float *aw = attn_w + (size_t)t * n;   /* [n], only 0..t used */
            float mx = -1e30f;
            for (int s = 0; s <= t; s++) {
                float dot = 0.0f;
                const float *qt = qL + (size_t)t * D, *ks = kL + (size_t)s * D;
                for (int i = 0; i < D; i++) dot += qt[i] * ks[i];
                aw[s] = dot * scale;
                if (aw[s] > mx) mx = aw[s];
            }
            float sum = 0.0f;
            for (int s = 0; s <= t; s++) { aw[s] = st_expf(aw[s] - mx); sum += aw[s]; }
            for (int s = 0; s <= t; s++) aw[s] /= sum;
            for (int s = t + 1; s < n; s++) aw[s] = 0.0f;
            float *ao = attn_o + (size_t)t * D;
            for (int i = 0; i < D; i++) ao[i] = 0.0f;
            for (int s = 0; s <= t; s++) {
                float w = aw[s]; const float *vs = vL + (size_t)s * D;
                for (int i = 0; i < D; i++) ao[i] += w * vs[i];
            }
        }
        /* residual: rout = rin + Wo attn_o */
        for (int t = 0; t < n; t++) {
            float tmp[DMAX];   /* [no-vla] bound to the L-tier d_model */
            mv(Wo, attn_o + (size_t)t * D, tmp, D, D);
            const float *x = rin + (size_t)t * D;
            float *y = rout + (size_t)t * D;
            for (int i = 0; i < D; i++) y[i] = x[i] + tmp[i];
        }

        /* ---- MoE FFN ---- (reads rout_pre, then adds moe to rout in place) */
        float *f_in = c->f_in + (size_t)l * n * D;
        float *attn_resid = c->attn_resid + (size_t)l * n * D;
        float *gateL = c->gate + (size_t)l * n * E;
        for (int t = 0; t < n; t++) {
            float *x = rout + (size_t)t * D;
            /* snapshot rout_pre (the x ffn-rmsnorm reads) before moe overwrites */
            float *arp = attn_resid + (size_t)t * D;
            for (int i = 0; i < D; i++) arp[i] = x[i];
            float *fin = f_in + (size_t)t * D;
            c->f_rstd[(size_t)l * n + t] = rmsnorm_fwd(x, fnorm, fin, D);
            float *gt = gateL + (size_t)t * E;
            for (int e = 0; e < E; e++) {
                const float *re = W + m->o_router + ((size_t)l * E + e) * D;
                float acc = 0.0f;
                for (int i = 0; i < D; i++) acc += re[i] * fin[i];
                gt[e] = acc;
            }

            /* ── DMOE-A joint path (distributed_moe_design.md §2/§4): score the
             * REPLICATED bank router rows alongside the floor logits, route the
             * top-K over the UNION, fire each (floor local; bank resident-or-
             * remote via the installed transport), then DROP any unreachable
             * bank expert and sum the survivors. When the bank is inactive this
             * branch is never entered, so the floor path below is byte-identical
             * to the pre-DMOE forward (the [dmoe-bank-empty-identity] gate). ── */
            if (dmoe_active) {
                float xg[ST_DMOE_CAND_MAX];
                for (int e = 0; e < E; e++) xg[e] = gt[e];   /* floor logits first */
                int nb = st_dmoe_score(l, fin, D, xg + E,
                                       ST_DMOE_CAND_MAX - E, st_dmoe_ctx);
                if (nb < 0) nb = 0;
                if (E + nb > ST_DMOE_CAND_MAX) nb = ST_DMOE_CAND_MAX - E;
                int xe[KMAX];
                int nk = dmoe_router_pick(xg, xe, E + nb);

                static float eo_all[KMAX][DMAX];   /* fixed L-tier K, d_model     */
                int alive_slot[KMAX];
                for (int j = 0; j < nk; j++) {
                    int id = xe[j];
                    float *eo = eo_all[j];
                    if (id < E) {
                        /* floor expert: the EXACT single-node SwiGLU (one math). */
                        const float *w1 = W + m->o_w1 + ((size_t)l * E + id) * DFF * D;
                        const float *w3 = W + m->o_w3 + ((size_t)l * E + id) * DFF * D;
                        const float *w2 = W + m->o_w2 + ((size_t)l * E + id) * D * DFF;
                        float ehh[DFFMAX];
                        for (int h = 0; h < DFF; h++) {
                            const float *w1h = w1 + (size_t)h * D;
                            const float *w3h = w3 + (size_t)h * D;
                            float g = 0.0f, u = 0.0f;
                            for (int i = 0; i < D; i++) { g += w1h[i] * fin[i]; u += w3h[i] * fin[i]; }
                            ehh[h] = st_silu(g) * u;
                        }
                        for (int i = 0; i < D; i++) {
                            const float *w2r = w2 + (size_t)i * DFF;
                            float acc2 = 0.0f;
                            for (int h = 0; h < DFF; h++) acc2 += w2r[h] * ehh[h];
                            eo[i] = acc2;
                        }
                        alive_slot[j] = 1;
                    } else {
                        /* bank expert: resident-or-remote; NEVER recompute what
                         * this node does not hold (the SS-6 clause DMOE inverts). */
                        int bslot = id - E;
                        if (st_dmoe_fire(l, bslot, fin, D, eo, st_dmoe_ctx) == 0) {
                            alive_slot[j] = 1; st_bank_fired_cnt++;
                        } else {
                            alive_slot[j] = 0; st_bank_dropped_cnt++;
                        }
                    }
                }
                /* honest degrade (§4.2 step 3): re-derive the softmax over the
                 * SURVIVING slots only, then sum in ASCENDING surviving-slot
                 * order — deterministic in (weights, bytes, failure set F). A
                 * token with F=∅ (oracle / all-remote-OK) is bit-identical to the
                 * joint softmax (same max-subtracted code, same order). */
                float smx = -1e30f;
                for (int j = 0; j < nk; j++)
                    if (alive_slot[j] && xg[xe[j]] > smx) smx = xg[xe[j]];
                float ssum = 0.0f; float wsurv[KMAX];
                for (int j = 0; j < nk; j++) {
                    if (!alive_slot[j]) { wsurv[j] = 0.0f; continue; }
                    wsurv[j] = st_expf(xg[xe[j]] - smx); ssum += wsurv[j];
                }
                if (ssum < 1e-20f) ssum = 1e-20f;
                float moe[DMAX];
                for (int i = 0; i < D; i++) moe[i] = 0.0f;
                for (int j = 0; j < nk; j++) {
                    if (!alive_slot[j]) continue;
                    float wj = wsurv[j] / ssum; const float *eo = eo_all[j];
                    for (int i = 0; i < D; i++) moe[i] += wj * eo[i];
                }
                for (int i = 0; i < D; i++) x[i] += moe[i];  /* residual in place */

                dmoe_sumw += nk;
                if (l == L - 1 && t == n - 1) {
                    dmoe_last_nk = nk;
                    for (int j = 0; j < KMAX; j++) dmoe_last_e[j] = (j < nk) ? xe[j] : -1;
                }
                continue;   /* token done — skip the floor-only path below */
            }

            int   *te = c->topk_e + ((size_t)l * n + t) * E;
            float *tw = c->topk_w + ((size_t)l * n + t) * E;
            int    nk = router_pick(gt, te, tw, E);
            c->topk_n[(size_t)l * n + t] = nk;   /* runtime firing width      */

            /* ── SS-6 (special-structure-mind.md §5): materialize EACH chosen
             * expert's [D] down-projection output into eo_all[j], LOCALLY or on
             * a PEER (SS-5 placement), then sum them into moe[] in a FIXED
             * canonical reduction order (ascending slot j == the single-node
             * order — CRITIQUE GATE #3). A remote expert returns the SAME [D]
             * vector the local SwiGLU would, so the sum is BYTE-IDENTICAL to a
             * single-node forward. When no hook is installed the inner branch is
             * always local => byte-unchanged from the pre-SS-6 forward.
             *
             * eo_all is a FILE-STATIC fixed [KMAX][DMAX] scratch (NOT a stack
             * array: KMAX*DMAX floats = 8 KB would risk the kernel's small
             * stack — the feedback_hosted_relay_stack_overflow class). st_forward
             * is single-threaded / non-reentrant (shares tw_buf / st_frozen_pos),
             * so a file-static scratch is safe and [no-vla] by construction. */
            static float eo_all[KMAX][DMAX];   /* fixed L-tier K, d_model */
            for (int j = 0; j < nk; j++) {
                int e = te[j];
                float *eo = eo_all[j];

                /* SS-6 gating + remote attempt: ONLY the EXTRA experts (slot
                 * j >= K_min) are ever eligible (local K_min ALWAYS local); the
                 * caller's predicate adds placement + degrade + region>=2. On a
                 * successful remote call we take the peer's [D] output as-is. */
                int did_remote = 0;
                if (st_remote_fn && st_remote_gate &&
                    st_remote_gate(l, j, K, e, st_remote_ctx)) {
                    if (st_remote_fn(l, e, fin, D, DFF, eo, st_remote_ctx) == 0) {
                        did_remote = 1;
                        st_remote_fired_cnt++;
                    } else {
                        /* timeout / absent peer -> recompute LOCALLY below
                         * (lose the WIDTH, not correctness; honest degraded). */
                        st_remote_fallback_cnt++;
                    }
                }

                if (!did_remote) {
                    /* LOCAL expert: compute SwiGLU, caching e_g/e_u/e_h for the
                     * backward (remote experts never run during training, so a
                     * remote slot leaves its cache untouched — backward is never
                     * taken on a forward that fired remotely). */
                    const float *w1 = W + m->o_w1 + ((size_t)l * E + e) * DFF * D;
                    const float *w3 = W + m->o_w3 + ((size_t)l * E + e) * DFF * D;
                    const float *w2 = W + m->o_w2 + ((size_t)l * E + e) * D * DFF;
                    float *eg = c->e_g + (((size_t)l * n + t) * E + j) * DFF;
                    float *eu = c->e_u + (((size_t)l * n + t) * E + j) * DFF;
                    float *eh = c->e_h + (((size_t)l * n + t) * E + j) * DFF;
                    for (int h = 0; h < DFF; h++) {
                        const float *w1h = w1 + (size_t)h * D;
                        const float *w3h = w3 + (size_t)h * D;
                        float g = 0.0f, u = 0.0f;
                        for (int i = 0; i < D; i++) { g += w1h[i] * fin[i]; u += w3h[i] * fin[i]; }
                        eg[h] = g; eu[h] = u; eh[h] = st_silu(g) * u;
                    }
                    /* down: eo[D] = w2 . eh (UNWEIGHTED — the weight wj is
                     * applied in the canonical sum below, exactly as a single-
                     * node forward applies it). */
                    for (int i = 0; i < D; i++) {
                        const float *w2r = w2 + (size_t)i * DFF;
                        float acc = 0.0f;
                        for (int h = 0; h < DFF; h++) acc += w2r[h] * eh[h];
                        eo[i] = acc;
                    }
                }
            }
            /* ── canonical reduction: sum the per-expert [D] outputs weighted by
             * the router softmax, in ASCENDING slot order j (identical to the
             * single-node forward's accumulation order -> byte-for-byte equal,
             * -O1 -ffp-contract=off, both arches). NO reassociation. */
            float moe[DMAX];   /* [no-vla] bound to the L-tier d_model */
            for (int i = 0; i < D; i++) moe[i] = 0.0f;
            for (int j = 0; j < nk; j++) {
                float wj = tw[j]; const float *eo = eo_all[j];
                for (int i = 0; i < D; i++) moe[i] += wj * eo[i];
            }
            for (int i = 0; i < D; i++) x[i] += moe[i];  /* residual in place */
        }
    }

    /* ---- output head ---- */
    const float *onorm = W + m->o_out_norm;
    const float *Out   = W + m->o_out;
    float *rfinal = c->resid + (size_t)L * n * D;
    for (int t = 0; t < n; t++) {
        const float *x = rfinal + (size_t)t * D;
        float *oin = c->o_in + (size_t)t * D;
        c->o_rstd[t] = rmsnorm_fwd(x, onorm, oin, D);
        float *lt = logits + (size_t)t * V;
        mv(Out, oin, lt, V, D);
        /* probs (softmax) cached for the CE backward */
        float *pr = c->probs + (size_t)t * V;
        float mx = -1e30f;
        for (int o = 0; o < V; o++) if (lt[o] > mx) mx = lt[o];
        float sum = 0.0f;
        for (int o = 0; o < V; o++) { pr[o] = st_expf(lt[o] - mx); sum += pr[o]; }
        if (sum < 1e-20f) sum = 1e-20f;
        for (int o = 0; o < V; o++) pr[o] /= sum;
    }

    /* ---- firing-width observability (SS-1) ----
     * last  = the final token's final-layer width ("answer token" width).
     * milli = mean width across all (layer, token) experts x1000 (libc-free
     * integer; no float division of the count, so it stays deterministic). */
    if (dmoe_active) {
        /* DMOE path writes no per-(l,t) cache (bank ids can exceed the E-slot
         * stride, and the backward never runs on a bank forward); the width
         * observability comes from the DMOE accumulators instead. */
        st_fw_last = dmoe_last_nk;
        for (int j = 0; j < KMAX; j++) st_fw_experts[j] = dmoe_last_e[j];
        long cells = (long)L * n;
        st_fw_milli = cells ? (int)((dmoe_sumw * 1000) / cells) : 0;
    } else {
        size_t fcell = (size_t)(L - 1) * n + (n - 1);   /* final token, final layer */
        st_fw_last = c->topk_n[fcell];
        const int *fe = c->topk_e + fcell * E;          /* E-slot stride (heap) */
        for (int j = 0; j < KMAX; j++)                  /* st_fw_experts[KMAX] fixed */
            st_fw_experts[j] = (j < st_fw_last) ? fe[j] : -1;
        long sumw = 0;
        for (int l = 0; l < L; l++)
            for (int t = 0; t < n; t++) sumw += c->topk_n[(size_t)l * n + t];
        long cells = (long)L * n;
        st_fw_milli = cells ? (int)((sumw * 1000) / cells) : 0;
    }
    st_router_alive = NULL;   /* SS-4: clear; never leak the mask past forward */
    return ST_OK;
}

/* ---- SS-6: the EXACT per-expert SwiGLU a peer must run (special-structure-
 * mind.md §5).  out[d] = w2_e . (silu(w1_e . fin) * (w3_e . fin)), UNWEIGHTED.
 * This is the SAME code the local MoE branch runs (one math); the kernel's
 * DRPC remote-expert handler and the cert's in-process stub both call THIS so
 * a remote expert's [D] output is bit-identical to the local computation, and
 * the canonical sum in st_forward is byte-identical to a single-node forward.
 * No cache writes (a remote slot is never read by the backward). */
int st_expert_forward_ref(const st_model *m, int layer, int expert_id,
                          const float *fin, float *out)
{
    ST_DIMS(m);
    if (!m || !fin || !out) return ST_E_ARG;
    if (layer < 0 || layer >= L) return ST_E_ARG;
    if (expert_id < 0 || expert_id >= E) return ST_E_ARG;
    if (DFF > DFFMAX) return ST_E_ARG;
    const float *W  = m->w;
    const float *w1 = W + m->o_w1 + ((size_t)layer * E + expert_id) * DFF * D;
    const float *w3 = W + m->o_w3 + ((size_t)layer * E + expert_id) * DFF * D;
    const float *w2 = W + m->o_w2 + ((size_t)layer * E + expert_id) * D * DFF;
    float eh[DFFMAX];   /* [no-vla] bound to the L-tier dff */
    for (int h = 0; h < DFF; h++) {
        const float *w1h = w1 + (size_t)h * D;
        const float *w3h = w3 + (size_t)h * D;
        float g = 0.0f, u = 0.0f;
        for (int i = 0; i < D; i++) { g += w1h[i] * fin[i]; u += w3h[i] * fin[i]; }
        eh[h] = st_silu(g) * u;
    }
    for (int i = 0; i < D; i++) {
        const float *w2r = w2 + (size_t)i * DFF;
        float acc = 0.0f;
        for (int h = 0; h < DFF; h++) acc += w2r[h] * eh[h];
        out[i] = acc;
    }
    return ST_OK;
}

/* ---- firing-width accessors (SS-1 observability; NOT wired to galaxy) ---- */
int st_last_fire_width(void)            { return st_fw_last; }
int st_last_fire_width_mean_milli(void) { return st_fw_milli; }
int st_last_fire_experts(int *out, int max)
{
    int nk = st_fw_last;
    if (nk > max) nk = max;
    for (int j = 0; j < nk; j++) out[j] = st_fw_experts[j];
    return nk;
}

/* ================================================================== */
/* KV cache (wave-kv-cache) — incremental generation forward          */
/* ================================================================== */
/*
 *  WHY: st_generate today re-runs the FULL st_forward over the whole growing
 *  context window every step (O(nctx) positions recomputed per byte, the ~1s/
 *  byte chat-speed pain and the SS-6 cross-node blocker). But this is a CAUSAL
 *  transformer with NO positional encoding (see the file header): the residual
 *  stream — and therefore the per-layer key/value — at position s depends ONLY
 *  on positions 0..s. Appending a NEW token at position t does NOT change any
 *  earlier position's k/v at any layer. So we can cache k[l][s] and v[l][s] for
 *  every prior position and, for a new token, compute ONLY its own q/k/v and
 *  attend over the CACHED k/v of positions 0..t — O(1) new position instead of
 *  re-deriving the whole prefix.
 *
 *  BYTE-IDENTICAL (the gate): caching changes WHAT is recomputed, never the
 *  math or the rounding order of any single position's attention sum. The new
 *  position's q.k dot iterates i=0..D-1, the softmax scans s=0..t finding the
 *  same max then the same st_expf, and the value mix accumulates s=0..t,
 *  i=0..D-1 — EXACTLY the recompute path's loop nest in st_forward. Same scale,
 *  same router_pick, same st_sample_byte. So (model,prompt,params,seed) -> the
 *  same logits, same sampled bytes, same FNV hash, on both arches under -O1
 *  -ffp-contract=off. The kv-equivalence cert proves this by hash + bytes.
 *
 *  BOUNDED: the cache is sized for ST_MAXSEQ positions at the L (MAX) tier dims
 *  (heap, one malloc reused across steps) so it is never a VLA and never grows.
 *  Every single-token scratch array is bound to DMAX/DFFMAX/EMAX/KMAX/V (fixed).
 *  When the generation window SLIDES (only after ST_MAXSEQ bytes), the position
 *  indices shift, so the cache is reset and that one step rebuilds from scratch
 *  — still byte-identical (it just refills the cache token by token).
 *
 *  SCOPE: inference/generation ONLY. st_forward / st_backward / training are
 *  UNTOUCHED — no cache there. This path feeds st_generate_core; st_forward's
 *  full activation cache (st_cache) is the backward's, and is left exactly as is.
 */

/* The KV cache: per-layer, per-position k and v, plus the count already filled.
 * k/v sized to the L-tier MAX dims (heap; bounded by ST_MAXSEQ positions). The
 * slot stride is DMAX so the same arena serves any tier (the resident model
 * uses only its own D <= DMAX of each slot). */
typedef struct {
    int   filled;            /* positions 0..filled-1 have valid k/v          */
    int   d, nlayer;         /* dims this cache was provisioned for (sanity)   */
    float *k;                /* [L][ST_MAXSEQ][DMAX]                            */
    float *v;                /* [L][ST_MAXSEQ][DMAX]                            */
} st_kvcache;

/* floats per k (or v) plane: bound to the FIXED maxima — no VLA, no growth. */
#define KV_PLANE_FLOATS ((size_t)LMAX * ST_MAXSEQ * DMAX)

/* Allocate the KV cache for model m (sized to MAX, used at the resident dims).
 * One malloc for both k and v planes. Returns NULL on OOM. */
static st_kvcache *kv_alloc(const st_model *m)
{
    ST_DIMS(m);
    size_t need = sizeof(st_kvcache) + 2 * KV_PLANE_FLOATS * sizeof(float);
    char *blob = (char *)malloc(need);
    if (!blob) return NULL;
    st_kvcache *kv = (st_kvcache *)blob;
    float *fp = (float *)(blob + sizeof(st_kvcache));
    kv->k = fp;
    kv->v = fp + KV_PLANE_FLOATS;
    kv->filled = 0;
    kv->d = D;
    kv->nlayer = L;
    return kv;
}

/* Incremental forward of ONE new token (raw byte `b`) at sequence position
 * `pos` (== kv->filled on entry, 0-based). Computes this position's per-layer
 * q/k/v, STORES its k/v into the cache at slot `pos`, attends over the cached
 * k/v of positions 0..pos (causal), runs the MoE FFN, and writes the V next-byte
 * logits for THIS position into `logits_row[V]`. kv->filled becomes pos+1.
 *
 * The math body mirrors st_forward's per-(l,t) loop EXACTLY for t==pos, but reads
 * cached k/v for s<pos instead of recomputing them — byte-identical reductions.
 * No st_cache mutation, no backward dependency. Returns ST_OK / negative. */
static int kv_step(st_model *m, st_kvcache *kv, uint8_t b, int pos,
                   float *logits_row)
{
    ST_DIMS(m);
    if (pos < 0 || pos >= ST_MAXSEQ) return ST_E_ARG;
    if (pos != kv->filled) return ST_E_ARG;   /* must extend contiguously     */
    const float *W = m->w;
    const float scale = st_rsqrtf((float)D);

    /* x = the residual stream for THIS position, flowing up through the layers.
     * Bound to DMAX (fixed) — no VLA. Seed with the byte embedding (== st_forward
     * embed: resid[layer 0][t] = embed[bytes[t]]). */
    float x[DMAX];
    {
        const float *emb = W + m->o_embed + (size_t)b * D;
        for (int i = 0; i < D; i++) x[i] = emb[i];
    }

    for (int l = 0; l < L; l++) {
        const float *anorm = W + m->o_attn_norm + (size_t)l * D;
        const float *fnorm = W + m->o_ffn_norm  + (size_t)l * D;
        const float *Wq = W + m->o_wq + (size_t)l * D * D;
        const float *Wk = W + m->o_wk + (size_t)l * D * D;
        const float *Wv = W + m->o_wv + (size_t)l * D * D;
        const float *Wo = W + m->o_wo + (size_t)l * D * D;

        /* ---- attention (this position only; attend over cached prefix) ----
         * a_in = rmsnorm(x); q/k/v = Wq/Wk/Wv a_in. Store k/v for slot `pos`. */
        float a_in[DMAX];
        (void)rmsnorm_fwd(x, anorm, a_in, D);
        float qcur[DMAX];
        mv(Wq, a_in, qcur, D, D);
        float *kslot = kv->k + ((size_t)l * ST_MAXSEQ + pos) * DMAX;
        float *vslot = kv->v + ((size_t)l * ST_MAXSEQ + pos) * DMAX;
        mv(Wk, a_in, kslot, D, D);
        mv(Wv, a_in, vslot, D, D);
        /* C1 RoPE: rotate q,k by this position (v untouched). The cache stores
         * the ROTATED k so prior positions' cached k are already rotated —
         * byte-identical to st_forward's rotate-then-attend order. */
        st_rope_apply(qcur, pos, D, +1.0f);
        st_rope_apply(kslot, pos, D, +1.0f);

        /* causal attention over s = 0..pos. SAME reduction order as st_forward:
         * dot loops i=0..D-1, softmax scans s=0..pos, value mix s then i.
         * aw scratch bound to ST_MAXSEQ (fixed) — no VLA. */
        float aw[ST_MAXSEQ];
        float mx = -1e30f;
        for (int s = 0; s <= pos; s++) {
            const float *ks = kv->k + ((size_t)l * ST_MAXSEQ + s) * DMAX;
            float dot = 0.0f;
            for (int i = 0; i < D; i++) dot += qcur[i] * ks[i];
            aw[s] = dot * scale;
            if (aw[s] > mx) mx = aw[s];
        }
        float sum = 0.0f;
        for (int s = 0; s <= pos; s++) { aw[s] = st_expf(aw[s] - mx); sum += aw[s]; }
        for (int s = 0; s <= pos; s++) aw[s] /= sum;
        float ao[DMAX];
        for (int i = 0; i < D; i++) ao[i] = 0.0f;
        for (int s = 0; s <= pos; s++) {
            float w = aw[s];
            const float *vs = kv->v + ((size_t)l * ST_MAXSEQ + s) * DMAX;
            for (int i = 0; i < D; i++) ao[i] += w * vs[i];
        }
        /* residual: x += Wo ao  (== st_forward's rout = rin + Wo attn_o) */
        float tmp[DMAX];
        mv(Wo, ao, tmp, D, D);
        for (int i = 0; i < D; i++) x[i] += tmp[i];

        /* ---- MoE FFN (this position) ---- identical to st_forward's body. */
        float fin[DMAX];
        (void)rmsnorm_fwd(x, fnorm, fin, D);
        float gt[EMAX];
        for (int e = 0; e < E; e++) {
            const float *re = W + m->o_router + ((size_t)l * E + e) * D;
            float acc = 0.0f;
            for (int i = 0; i < D; i++) acc += re[i] * fin[i];
            gt[e] = acc;
        }
        int   te[EMAX];
        float tw[EMAX];
        int   nk = router_pick(gt, te, tw, E);   /* st_freeze_routing==0 here */

        float moe[DMAX];
        for (int i = 0; i < D; i++) moe[i] = 0.0f;
        float eh[DFFMAX];
        for (int j = 0; j < nk; j++) {
            int e = te[j]; float wj = tw[j];
            const float *w1 = W + m->o_w1 + ((size_t)l * E + e) * DFF * D;
            const float *w3 = W + m->o_w3 + ((size_t)l * E + e) * DFF * D;
            const float *w2 = W + m->o_w2 + ((size_t)l * E + e) * D * DFF;
            for (int h = 0; h < DFF; h++) {
                const float *w1h = w1 + (size_t)h * D;
                const float *w3h = w3 + (size_t)h * D;
                float g = 0.0f, u = 0.0f;
                for (int i = 0; i < D; i++) { g += w1h[i] * fin[i]; u += w3h[i] * fin[i]; }
                eh[h] = st_silu(g) * u;
            }
            for (int i = 0; i < D; i++) {
                const float *w2r = w2 + (size_t)i * DFF;
                float acc = 0.0f;
                for (int h = 0; h < DFF; h++) acc += w2r[h] * eh[h];
                moe[i] += wj * acc;
            }
        }
        for (int i = 0; i < D; i++) x[i] += moe[i];
    }

    /* ---- output head (this position) ---- identical to st_forward. */
    const float *onorm = W + m->o_out_norm;
    const float *Out   = W + m->o_out;
    float oin[DMAX];
    (void)rmsnorm_fwd(x, onorm, oin, D);
    mv(Out, oin, logits_row, V, D);

    kv->filled = pos + 1;
    return ST_OK;
}

/* ---- KV-cache enable toggle + logit-hash observability (test hooks) ----
 * st_generate uses the KV-cache path by DEFAULT (the whole point — chat speed).
 * The cert flips it OFF to obtain the recompute oracle, ON to obtain the cached
 * run, and asserts byte-identical. The per-step logit FNV lets the cert prove
 * the LOGITS (not just the sampled bytes) are bit-identical between the paths. */
static int      st_kv_enabled = 1;
static uint64_t st_gen_logit_fnv = 1469598103934665603ULL; /* FNV-1a offset */

void     st_kv_set_enabled(int on) { st_kv_enabled = on ? 1 : 0; }
int      st_kv_get_enabled(void)   { return st_kv_enabled; }
void     st_gen_logit_hash_reset(void) { st_gen_logit_fnv = 1469598103934665603ULL; }
uint64_t st_gen_logit_hash(void)       { return st_gen_logit_fnv; }

/* fold the V-float logit row (the row generation actually sampled from) into the
 * running FNV-1a over its raw bytes — order-stable, libc-free. */
static void st_gen_hash_row(const float *row)
{
    const unsigned char *p = (const unsigned char *)row;
    uint64_t h = st_gen_logit_fnv;
    for (size_t i = 0; i < (size_t)V * sizeof(float); i++) { h ^= p[i]; h *= 1099511628211ULL; }
    st_gen_logit_fnv = h;
}

/* ================================================================== */
/* generation (autoregressive sampling) — step ⑥, the chat mouth      */
/* ================================================================== */
/*
 *  The baby SPEAKS. Feed `prompt` (n_prompt raw bytes) as context, then
 *  repeatedly st_forward the running window and sample the next byte from the
 *  LAST row's logits (temperature + top-k). Byte-level, 256 vocab, OOV-free.
 *
 *  Reproducibility / one-math (wave-49): the categorical draw uses a
 *  self-contained xorshift64* (NO libc rand, NO transcendental beyond the
 *  file-local st_expf), so (model, prompt, params, seed) -> the SAME bytes on
 *  every target. Built -O1 -ffp-contract=off with the rest of the file.
 *
 *  Bounded by design (the relay stack-overflow lesson): the running context is
 *  CAPPED at ST_MAXSEQ (the model never saw longer), so each step is a fixed
 *  O(ST_MAXSEQ^2) forward; max_gen is clamped to ST_GEN_CAP. The only heap is
 *  one logits buffer (ST_MAXSEQ*V floats) freed before return — no per-token
 *  malloc, no big stack array.
 *
 *  Optional streaming: when `emit` is non-NULL it is called with each freshly
 *  produced byte (and the opaque `ctx`) as generation proceeds, so a caller can
 *  flush chunks progressively (the slow baby's text appears as it thinks).
 */
#define ST_GEN_CAP 96   /* hard cap on generated bytes (step ⑥ keep-it-fast) */

/* xorshift64* — full-period libc-free PRNG, state threaded explicitly so the
 * whole generation is reproducible. Zero seed maps to a fixed nonzero state. */
static uint64_t st_rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}
/* uniform float in [0,1) from the top 24 bits (single-precision mantissa). */
static float st_rng_unit(uint64_t *s)
{
    return (float)(st_rng_next(s) >> 40) * (1.0f / 16777216.0f);  /* 2^-24 */
}

/* Sample one byte from `row` (the V next-byte logits after the last context
 * byte): temperature scale + optional top-k cap, softmax, categorical draw.
 * temp<=0 short-circuits to argmax (deterministic). top_k<=0 or >=V means "all".
 * The top-k cap is an O(V*k) partial selection into the small fixed `idx`/`val`
 * scratch (k is tiny, V=256), matching sample.c's discipline. */
static int st_sample_byte(const float *row, float temp, int top_k, uint64_t *rng)
{
    if (temp <= 0.0f) {                       /* greedy */
        int best = 0; float bl = row[0];
        for (int v = 1; v < V; v++) if (row[v] > bl) { bl = row[v]; best = v; }
        return best;
    }

    int k = (top_k > 0 && top_k < V) ? top_k : V;

    /* fixed-size scratch: V=256 ints + floats is small and bounded — keep it on
     * the stack here (this is NOT a network task; sample.c uses the same style).
     * idx[i] holds the byte id of the i-th highest logit kept. */
    int   idx[V];
    float val[V];

    /* partial selection of the top-k logits (descending insertion). */
    int nsel = 0;
    for (int v = 0; v < V; v++) {
        float lg = row[v];
        if (nsel < k) {
            int i = nsel++;
            while (i > 0 && val[i - 1] < lg) { val[i] = val[i - 1]; idx[i] = idx[i - 1]; i--; }
            val[i] = lg; idx[i] = v;
        } else if (lg > val[k - 1]) {
            int i = k - 1;
            while (i > 0 && val[i - 1] < lg) { val[i] = val[i - 1]; idx[i] = idx[i - 1]; i--; }
            val[i] = lg; idx[i] = v;
        }
    }

    /* temperature + softmax over the kept set (max-subtracted; libc-free expf). */
    float maxl = val[0], inv_t = 1.0f / temp, sum = 0.0f;
    for (int i = 0; i < nsel; i++) { val[i] = st_expf((val[i] - maxl) * inv_t); sum += val[i]; }
    float inv = (sum > 0.0f) ? (1.0f / sum) : 0.0f;
    for (int i = 0; i < nsel; i++) val[i] *= inv;

    /* categorical draw. */
    float r = st_rng_unit(rng), acc = 0.0f;
    for (int i = 0; i < nsel; i++) { acc += val[i]; if (r < acc) return idx[i]; }
    return idx[nsel - 1];                     /* fp slack -> last kept */
}

/* shared core: fill `out` (max_gen bytes) and, when emit!=NULL, stream each
 * byte through emit(ctx, byte). Returns the count produced (>=0), or negative
 * on bad args / OOM. A no-resident-baby caller never reaches here. */
static int st_generate_core(st_model *m, const uint8_t *prompt, int n_prompt,
                            uint8_t *out, int max_gen, float temp, int top_k,
                            uint64_t seed, void (*emit)(void *, int), void *ctx)
{
    if (!m || !m->w) return ST_E_ARG;
    if (max_gen < 0) return ST_E_ARG;
    if (max_gen > ST_GEN_CAP) max_gen = ST_GEN_CAP;
    if (n_prompt < 0) n_prompt = 0;

    /* running context window, capped at the model's max seq. When the prompt is
     * longer than ST_MAXSEQ-1 we keep its TAIL (most recent bytes) so there is
     * always room for at least one fresh sample. */
    uint8_t ctxbuf[ST_MAXSEQ];
    int nctx = 0;
    int pstart = 0;
    if (n_prompt > ST_MAXSEQ - 1) pstart = n_prompt - (ST_MAXSEQ - 1);
    for (int i = pstart; i < n_prompt; i++) ctxbuf[nctx++] = prompt[i];
    if (nctx == 0) ctxbuf[nctx++] = (uint8_t)'\n';   /* a neutral seed byte */

    uint64_t rng = seed ? seed : 0x9E3779B97F4A7C15ULL;

    /* ---- KV-cache path (default): O(1) new position per byte ----
     * Maintain the SAME ctxbuf/nctx semantics as the recompute path; a parallel
     * kv cache holds k/v for ctxbuf[0..filled-1]. Each step we extend the cache
     * up to nctx (kv_step the bytes it hasn't seen — normally just the one fresh
     * byte), sample from the last position's logits, then append/slide. On a
     * window SLIDE the position indices shift, so kv->filled is reset to 0 and
     * the next step re-primes from the slid window — still byte-identical (it
     * just refills via the same per-position reductions). */
    if (st_kv_enabled) {
        st_kvcache *kv = kv_alloc(m);
        if (!kv) return ST_E_OOM;
        float row[V];                 /* this position's logits (V fixed)       */
        int produced = 0;
        int rc = ST_OK;
        for (int g = 0; g < max_gen; g++) {
            /* extend the cache to cover ctxbuf[0..nctx-1]; row holds the logits
             * of the LAST extended position (== what we sample from).
             * pk_nosw.h: one region per byte (reads m->w, which a higher task's
             * adam / load / merge rewrites), not per reply — emit() may print. */
            PK_NOSW_ENTER();
            for (int p = kv->filled; p < nctx; p++) {
                rc = kv_step(m, kv, ctxbuf[p], p, row);
                if (rc != ST_OK) break;
            }
            if (rc != ST_OK) { PK_NOSW_LEAVE(); break; }
            st_gen_hash_row(row);
            int b = st_sample_byte(row, temp, top_k, &rng);
            PK_NOSW_LEAVE();

            out[produced++] = (uint8_t)b;
            if (emit) emit(ctx, b);

            if (nctx < ST_MAXSEQ) {
                ctxbuf[nctx++] = (uint8_t)b;   /* cache stays valid: extend next */
            } else {
                for (int i = 0; i < ST_MAXSEQ - 1; i++) ctxbuf[i] = ctxbuf[i + 1];
                ctxbuf[ST_MAXSEQ - 1] = (uint8_t)b;
                kv->filled = 0;                /* positions shifted -> re-prime  */
            }
        }
        free(kv);
        return produced;
    }

    /* ---- recompute path (the oracle / fallback): full O(nctx) forward/byte ---- */
    float *logits = (float *)malloc((size_t)ST_MAXSEQ * V * sizeof(float));
    if (!logits) return ST_E_OOM;

    int produced = 0;
    for (int g = 0; g < max_gen; g++) {
        PK_NOSW_ENTER();                                       /* as above */
        int rc = st_forward(m, ctxbuf, nctx, logits);
        if (rc != ST_OK) { PK_NOSW_LEAVE(); break; }
        const float *row = logits + (size_t)(nctx - 1) * V;   /* after last byte */
        st_gen_hash_row(row);
        int b = st_sample_byte(row, temp, top_k, &rng);
        PK_NOSW_LEAVE();

        out[produced++] = (uint8_t)b;
        if (emit) emit(ctx, b);

        /* append to context; if full, slide the window forward by one (keep the
         * most recent ST_MAXSEQ bytes — bounded, no growth). */
        if (nctx < ST_MAXSEQ) {
            ctxbuf[nctx++] = (uint8_t)b;
        } else {
            for (int i = 0; i < ST_MAXSEQ - 1; i++) ctxbuf[i] = ctxbuf[i + 1];
            ctxbuf[ST_MAXSEQ - 1] = (uint8_t)b;
        }
    }

    free(logits);
    return produced;
}

int st_generate(st_model *m, const uint8_t *prompt, int n_prompt,
                uint8_t *out, int max_gen, float temp, int top_k, uint64_t seed)
{
    return st_generate_core(m, prompt, n_prompt, out, max_gen, temp, top_k,
                            seed, 0, 0);
}

/* streaming variant (declared in student.h for the chat bridge): identical
 * sampling, but each produced byte is also handed to emit(ctx, byte) as it is
 * generated, so the caller can flush it progressively. */
int st_generate_stream(st_model *m, const uint8_t *prompt, int n_prompt,
                       uint8_t *out, int max_gen, float temp, int top_k,
                       uint64_t seed, void (*emit)(void *, int), void *ctx)
{
    return st_generate_core(m, prompt, n_prompt, out, max_gen, temp, top_k,
                            seed, emit, ctx);
}

/* ================================================================== */
/* eval loss (pure forward)                                           */
/* ================================================================== */

PK_NOSW_FN(float, st_eval_loss, (st_model *m, const uint8_t *bytes, int n, int *n_pred),
           (m, bytes, n, n_pred))
{
    if (n < 2) { if (n_pred) *n_pred = 0; return 0.0f; }
    float *logits = (float *)malloc((size_t)n * V * sizeof(float));
    if (!logits) { if (n_pred) *n_pred = 0; return 0.0f; }
    st_forward(m, bytes, n, logits);
    st_cache *c = (st_cache *)m->cache;
    double loss = 0.0; int np = 0;
    for (int t = 0; t < n - 1; t++) {
        int tgt = bytes[t + 1];
        float p = c->probs[(size_t)t * V + tgt];
        loss += -(double)st_logf(p);
        np++;
    }
    free(logits);
    if (n_pred) *n_pred = np;
    return np ? (float)(loss / np) : 0.0f;
}

/* C1 [ctx-carry]: mean next-byte CE over ONLY the target span [t0,t1) (predict
 * bytes[t] from prefix 0..t-1). Also FNV-1a's the FULL logit ROW that predicted
 * each answer byte (row t-1) into *row_fnv — the window-mechanism proof: with
 * the wide window the answer logits DEPEND on a distant fact byte (hash shifts
 * when it changes); clamped to 64 they cannot (the distant byte is dropped ->
 * identical hash). Pure forward. Bytes are raw; n must be <= ST_MAXSEQ.        */
PK_NOSW_FN(float, st_span_ce, (st_model *m, const uint8_t *bytes, int n, int t0, int t1,
                                uint64_t *row_fnv),
           (m, bytes, n, t0, t1, row_fnv))
{
    uint64_t h = 1469598103934665603ULL;   /* FNV-1a offset basis              */
    if (t0 < 1) t0 = 1;
    if (t1 > n) t1 = n;
    if (n < 2 || t1 <= t0) { if (row_fnv) *row_fnv = h; return 0.0f; }
    float *logits = (float *)malloc((size_t)n * V * sizeof(float));
    if (!logits) { if (row_fnv) *row_fnv = h; return 0.0f; }
    if (st_forward(m, bytes, n, logits) != ST_OK) {
        free(logits); if (row_fnv) *row_fnv = h; return 0.0f;
    }
    st_cache *c = (st_cache *)m->cache;
    double loss = 0.0; int np = 0;
    for (int t = t0; t < t1; t++) {
        int tgt = bytes[t];                             /* predict bytes[t]     */
        float p = c->probs[(size_t)(t - 1) * V + tgt];  /* from row t-1         */
        loss += -(double)st_logf(p);
        np++;
        const unsigned char *lp =
            (const unsigned char *)(logits + (size_t)(t - 1) * V);
        for (size_t b = 0; b < (size_t)V * sizeof(float); b++) {
            h ^= lp[b]; h *= 1099511628211ULL;
        }
    }
    free(logits);
    if (row_fnv) *row_fnv = h;
    return np ? (float)(loss / np) : 0.0f;
}

/* ================================================================== */
/* backward                                                           */
/* ================================================================== */

static float st_backward_q(st_model *m, const uint8_t *bytes, int n,
                           const float *q);

PK_NOSW_FN(float, st_backward, (st_model *m, const uint8_t *bytes, int n), (m, bytes, n))
{
    return st_backward_q(m, bytes, n, NULL);
}

/* D5-c stage 3: the same backward with a soft target q[(n-1)*V] */
PK_NOSW_FN(float, st_backward_soft, (st_model *m, const uint8_t *bytes, int n,
                                     const float *q), (m, bytes, n, q))
{
    if (!q) return 0.0f;
    return st_backward_q(m, bytes, n, q);
}

/* The body of both. q == NULL: the one-hot next byte (st_backward, the same
 * operations in the same order as before the split). Otherwise q is the
 * target distribution per position and the loss is -sum q log p. */
static float st_backward_q(st_model *m, const uint8_t *bytes, int n,
                           const float *q)
{
    ST_DIMS(m);
    st_cache *c = (st_cache *)m->cache;
    if (!c || c->n != n) return 0.0f;
    /* SS-6 contract (audit nit, 2026-06-20): a remote expert fire leaves this
     * node's e_g/e_u/e_h cache STALE for the remote slots, so a backward pass
     * over them would silently corrupt gradients. Training MUST run with the
     * remote hook clear (st_forward fires 0 experts remotely). Fail CLOSED if
     * that contract is ever violated rather than train on poisoned caches.
     * DMOE-A extends this exactly: a bank forward (fired OR dropped a bank
     * expert) took the joint path, which caches no e_g/e_u/e_h — so training
     * runs bank-inactive and the backward fail-closes if a bank slot fired. */
    if (st_last_remote_fired() > 0) return 0.0f;
    if (st_last_bank_fired() > 0 || st_last_bank_dropped() > 0) return 0.0f;
    const float *W = m->w;
    float *G = m->g;
    int np = n - 1;
    if (np < 1) return 0.0f;
    float invN = 1.0f / (float)np;
    float scale = st_rsqrtf((float)D);

    /* grad wrt the final residual stream, [n][D] */
    float *g_rfinal = (float *)calloc((size_t)n * D, sizeof(float));
    if (!g_rfinal) return 0.0f;

    /* ---- output head + loss ---- */
    const float *Out = W + m->o_out;
    float *gOut   = G + m->o_out;
    float *gOnorm = G + m->o_out_norm;
    const float *onorm = W + m->o_out_norm;
    float *rfinal = c->resid + (size_t)L * n * D;
    double loss = 0.0;
    float g_logit[V];      /* V is tier-invariant (byte vocab)  */
    float g_oin[DMAX];     /* [no-vla] bound to the L-tier d_model */
    for (int t = 0; t < n - 1; t++) {
        int tgt = bytes[t + 1];
        const float *pr = c->probs + (size_t)t * V;
        if (!q) {
            loss += -(double)st_logf(pr[tgt]);
            /* dL/dlogit = (p - onehot) * invN */
            for (int o = 0; o < V; o++) g_logit[o] = pr[o] * invN;
            g_logit[tgt] -= invN;
        } else {
            /* dL/dlogit = (p - q) * invN */
            const float *qt = q + (size_t)t * V;
            for (int o = 0; o < V; o++) {
                if (qt[o] > 0.0f) loss += -(double)qt[o] * (double)st_logf(pr[o]);
                g_logit[o] = pr[o] * invN - qt[o] * invN;
            }
        }
        /* logits = Out . o_in : grads */
        const float *oin = c->o_in + (size_t)t * D;
        for (int i = 0; i < D; i++) g_oin[i] = 0.0f;
        mv_bwd(Out, oin, g_logit, g_oin, gOut, V, D);
        /* through out rmsnorm into g_rfinal[t] */
        const float *x = rfinal + (size_t)t * D;
        rmsnorm_bwd(x, onorm, c->o_rstd[t], g_oin,
                    g_rfinal + (size_t)t * D, gOnorm, D);
    }
    /* position n-1 predicts nothing (no target): its g_rfinal stays 0. */

    /* grad wrt residual entering current layer's output (rout), [n][D].
     * We propagate from final back through each layer. */
    float *g_r = g_rfinal;   /* alias; becomes grad wrt rout of layer L-1 */

    for (int l = L - 1; l >= 0; l--) {
        float *rin  = c->resid + (size_t)l * n * D;
        const float *anorm = W + m->o_attn_norm + (size_t)l * D;
        const float *fnorm = W + m->o_ffn_norm  + (size_t)l * D;
        const float *Wq = W + m->o_wq + (size_t)l * D * D;
        const float *Wk = W + m->o_wk + (size_t)l * D * D;
        const float *Wv = W + m->o_wv + (size_t)l * D * D;
        const float *Wo = W + m->o_wo + (size_t)l * D * D;
        float *gAnorm = G + m->o_attn_norm + (size_t)l * D;
        float *gFnorm = G + m->o_ffn_norm  + (size_t)l * D;
        float *gWq = G + m->o_wq + (size_t)l * D * D;
        float *gWk = G + m->o_wk + (size_t)l * D * D;
        float *gWv = G + m->o_wv + (size_t)l * D * D;
        float *gWo = G + m->o_wo + (size_t)l * D * D;

        float *a_in = c->a_in + (size_t)l * n * D;
        float *qL = c->q + (size_t)l * n * D;
        float *kL = c->k + (size_t)l * n * D;
        float *vL = c->v + (size_t)l * n * D;
        float *attn_w = c->attn_w + (size_t)l * n * n;
        float *attn_o = c->attn_o + (size_t)l * n * D;
        float *f_in = c->f_in + (size_t)l * n * D;

        /* g_rout starts as g_r (grad arriving at layer output). The MoE block
         * sits on the residual after attention; we walk it back to attn output. */

        /* ===== MoE backward ===== */
        /* grads wrt f_in[t] and (via residual) accumulate back into g_rout. */
        for (int t = 0; t < n; t++) {
            float *grout = g_r + (size_t)t * D;     /* dL/d(rout_t) */
            /* residual: rout = (rin+Wo a) + moe. moe path: */
            float *fin = f_in + (size_t)t * D;
            int    nk = c->topk_n[(size_t)l * n + t];  /* runtime firing width */
            int   *te = c->topk_e + ((size_t)l * n + t) * E;
            float *tw = c->topk_w + ((size_t)l * n + t) * E;
            float g_fin[DMAX];     /* [no-vla] bound to the L-tier d_model */
            for (int i = 0; i < D; i++) g_fin[i] = 0.0f;
            float g_gate[EMAX];    /* [no-vla] bound to the L-tier expert count */
            for (int e = 0; e < E; e++) g_gate[e] = 0.0f;
            float gw_chosen[KMAX]; /* dL/d(gate-weight tw[j]); KMAX-bound, no-VLA */

            for (int j = 0; j < nk; j++) {
                int e = te[j]; float wj = tw[j];
                const float *w1 = W + m->o_w1 + ((size_t)l * E + e) * DFF * D;
                const float *w3 = W + m->o_w3 + ((size_t)l * E + e) * DFF * D;
                const float *w2 = W + m->o_w2 + ((size_t)l * E + e) * D * DFF;
                float *gw1 = G + m->o_w1 + ((size_t)l * E + e) * DFF * D;
                float *gw3 = G + m->o_w3 + ((size_t)l * E + e) * DFF * D;
                float *gw2 = G + m->o_w2 + ((size_t)l * E + e) * D * DFF;
                float *eg = c->e_g + (((size_t)l * n + t) * E + j) * DFF;
                float *eu = c->e_u + (((size_t)l * n + t) * E + j) * DFF;
                float *eh = c->e_h + (((size_t)l * n + t) * E + j) * DFF;

                /* expert output eo[i] = sum_h w2[i][h] eh[h]; moe += wj*eo
                 * grout flows to eo as wj*grout; and to gate weight wj as
                 * dot(grout, eo). */
                float g_eo[DMAX];   /* [no-vla] bound to the L-tier d_model */
                float g_wj = 0.0f;
                /* recompute eo for the gate-weight grad */
                for (int i = 0; i < D; i++) {
                    const float *w2r = w2 + (size_t)i * DFF;
                    float eo = 0.0f;
                    for (int h = 0; h < DFF; h++) eo += w2r[h] * eh[h];
                    g_wj += grout[i] * eo;
                    g_eo[i] = wj * grout[i];
                }
                /* down proj backward: eo = w2 . eh */
                float g_eh[DFFMAX];   /* [no-vla] bound to the L-tier dff */
                for (int h = 0; h < DFF; h++) g_eh[h] = 0.0f;
                for (int i = 0; i < D; i++) {
                    float ge = g_eo[i];
                    const float *w2r = w2 + (size_t)i * DFF;
                    float *gw2r = gw2 + (size_t)i * DFF;
                    for (int h = 0; h < DFF; h++) {
                        g_eh[h]  += w2r[h] * ge;
                        gw2r[h]  += ge * eh[h];
                    }
                }
                /* eh = silu(eg) * eu */
                for (int h = 0; h < DFF; h++) {
                    float sg = st_silu(eg[h]);
                    float g_sg = g_eh[h] * eu[h];      /* dL/d silu(eg) */
                    float g_u  = g_eh[h] * sg;         /* dL/d eu       */
                    float g_g  = g_sg * st_silu_grad(eg[h]); /* dL/d eg */
                    /* eg = w1 . fin ; eu = w3 . fin */
                    const float *w1h = w1 + (size_t)h * D;
                    const float *w3h = w3 + (size_t)h * D;
                    float *gw1h = gw1 + (size_t)h * D;
                    float *gw3h = gw3 + (size_t)h * D;
                    for (int i = 0; i < D; i++) {
                        g_fin[i] += w1h[i] * g_g + w3h[i] * g_u;
                        gw1h[i]  += g_g * fin[i];
                        gw3h[i]  += g_u * fin[i];
                    }
                }
                /* gate-weight grad for this chosen expert (softmax handled
                 * after the loop, since tw[j] = softmax over the K chosen). */
                gw_chosen[j] = g_wj;
            }
            /* softmax-over-chosen backward: tw[j] = softmax(gate[te[j]]) over
             * the nk chosen. dL/dgate[te[a]] = sum_j gw[j] tw[j](delta_aj-tw[a]). */
            for (int a = 0; a < nk; a++) {
                float ga = 0.0f;
                for (int j = 0; j < nk; j++) {
                    float d = (a == j) ? 1.0f : 0.0f;
                    ga += gw_chosen[j] * tw[j] * (d - tw[a]);
                }
                g_gate[te[a]] += ga;
            }
            /* router logits: gate[e] = router_e . fin */
            for (int e = 0; e < E; e++) {
                float ge = g_gate[e];
                if (ge == 0.0f) continue;
                const float *re = W + m->o_router + ((size_t)l * E + e) * D;
                float *gre = G + m->o_router + ((size_t)l * E + e) * D;
                for (int i = 0; i < D; i++) { g_fin[i] += re[i] * ge; gre[i] += ge * fin[i]; }
            }
            /* through ffn rmsnorm: f_in = rmsnorm(rout_pre); use the cached
             * rout_pre (attn_resid), NOT resid[l+1] which forward overwrote
             * with rout_pre+moe. grad accumulates into grout. */
            rmsnorm_bwd(c->attn_resid + ((size_t)l * n + t) * D, fnorm,
                        c->f_rstd[(size_t)l * n + t],
                        g_fin, grout, gFnorm, D);
            /* (grout already holds the direct residual grad from moe -> rout) */
        }

        /* ===== attention backward ===== */
        /* rout = rin + Wo attn_o ; g_r (==grout) now is full grad wrt rout.
         * Split: grad to rin (residual skip) and to Wo attn_o. */
        float *g_attn_o = (float *)calloc((size_t)n * D, sizeof(float));
        float *g_q = (float *)calloc((size_t)n * D, sizeof(float));
        float *g_k = (float *)calloc((size_t)n * D, sizeof(float));
        float *g_v = (float *)calloc((size_t)n * D, sizeof(float));
        float *g_rin = (float *)calloc((size_t)n * D, sizeof(float));
        if (!g_attn_o || !g_q || !g_k || !g_v || !g_rin) {
            free(g_attn_o); free(g_q); free(g_k); free(g_v); free(g_rin);
            free(g_rfinal); return (float)(loss * invN);
        }
        for (int t = 0; t < n; t++) {
            float *grout = g_r + (size_t)t * D;
            /* residual skip: rin gets grout directly */
            for (int i = 0; i < D; i++) g_rin[(size_t)t * D + i] += grout[i];
            /* Wo attn_o backward */
            mv_bwd(Wo, attn_o + (size_t)t * D, grout,
                   g_attn_o + (size_t)t * D, gWo, D, D);
        }
        /* attn_o[t] = sum_{s<=t} aw[t][s] v[s]  -> g_v, g_aw */
        for (int t = 0; t < n; t++) {
            const float *aw = attn_w + (size_t)t * n;
            const float *gao = g_attn_o + (size_t)t * D;
            float g_aw[ST_MAXSEQ];
            for (int s = 0; s <= t; s++) {
                /* g_v[s] += aw[s]*gao ; g_aw[s] = dot(gao, v[s]) */
                const float *vs = vL + (size_t)s * D;
                float *gvs = g_v + (size_t)s * D;
                float gw = 0.0f;
                for (int i = 0; i < D; i++) { gvs[i] += aw[s] * gao[i]; gw += gao[i] * vs[i]; }
                g_aw[s] = gw;
            }
            /* softmax backward over s=0..t: aw = softmax(score) */
            float dot = 0.0f;
            for (int s = 0; s <= t; s++) dot += g_aw[s] * aw[s];
            /* g_score[s] = aw[s]*(g_aw[s]-dot); score = scale * q.k */
            const float *qt = qL + (size_t)t * D;
            float *gqt = g_q + (size_t)t * D;
            for (int s = 0; s <= t; s++) {
                float gscore = aw[s] * (g_aw[s] - dot) * scale;
                const float *ks = kL + (size_t)s * D;
                float *gks = g_k + (size_t)s * D;
                for (int i = 0; i < D; i++) { gqt[i] += gscore * ks[i]; gks[i] += gscore * qt[i]; }
            }
        }
        /* q,k,v = Wq/Wk/Wv a_in ; backward into a_in and weights */
        for (int t = 0; t < n; t++) {
            const float *ain = a_in + (size_t)t * D;
            float g_ain[DMAX];   /* [no-vla] bound to the L-tier d_model */
            for (int i = 0; i < D; i++) g_ain[i] = 0.0f;
            /* C1 RoPE backward: g_q/g_k arrived as grad wrt the ROTATED q/k
             * (attention used the rotated vectors). Wq/Wk produce the UNROTATED
             * q/k, so transform the gradient by the rotation TRANSPOSE R(t)^T =
             * R(-t) (ssign=-1) BEFORE mv_bwd. v is unrotated -> g_v untouched.
             * Rotation is orthogonal so R^T is its exact analytic adjoint (the
             * grad-check confirms). Done in place: each g_* row feeds only its
             * own mv_bwd, no aliasing. */
            st_rope_apply(g_q + (size_t)t * D, t, D, -1.0f);
            st_rope_apply(g_k + (size_t)t * D, t, D, -1.0f);
            mv_bwd(Wq, ain, g_q + (size_t)t * D, g_ain, gWq, D, D);
            mv_bwd(Wk, ain, g_k + (size_t)t * D, g_ain, gWk, D, D);
            mv_bwd(Wv, ain, g_v + (size_t)t * D, g_ain, gWv, D, D);
            /* a_in = rmsnorm(rin) : into g_rin */
            rmsnorm_bwd(rin + (size_t)t * D, anorm, c->a_rstd[(size_t)l * n + t],
                        g_ain, g_rin + (size_t)t * D, gAnorm, D);
        }

        /* g_rin becomes the grad wrt this layer's input == previous layer rout.
         * Copy it into g_r for the next (lower) iteration. */
        for (int i = 0; i < n * D; i++) g_r[i] = g_rin[i];
        free(g_attn_o); free(g_q); free(g_k); free(g_v); free(g_rin);
    }

    /* ---- embedding backward ---- : resid[0][t] = embed[bytes[t]] */
    float *gEmb = G + m->o_embed;
    for (int t = 0; t < n; t++) {
        float *gr = g_r + (size_t)t * D;
        float *ge = gEmb + (size_t)bytes[t] * D;
        for (int i = 0; i < D; i++) ge[i] += gr[i];
    }

    free(g_rfinal);
    return (float)(loss * invN);
}

/* ================================================================== */
/* optimizer                                                          */
/* ================================================================== */

void st_zero_grad(st_model *m)
{
    PK_NOSW_ENTER();
    for (int i = 0; i < m->n_params; i++) m->g[i] = 0.0f;
    PK_NOSW_LEAVE();
}

void st_adam_step(st_model *m, float lr)
{
    PK_NOSW_ENTER();
    m->adam_t++;
    float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    float bc1 = 1.0f - st_expf((float)m->adam_t * st_logf(b1));
    float bc2 = 1.0f - st_expf((float)m->adam_t * st_logf(b2));
    if (bc1 < 1e-12f) bc1 = 1e-12f;
    if (bc2 < 1e-12f) bc2 = 1e-12f;
    for (int i = 0; i < m->n_params; i++) {
        float g = m->g[i];
        m->mu[i] = b1 * m->mu[i] + (1.0f - b1) * g;
        m->vu[i] = b2 * m->vu[i] + (1.0f - b2) * g * g;
        float mh = m->mu[i] / bc1;
        float vh = m->vu[i] / bc2;
        m->w[i] -= lr * mh / (st_sqrtf(vh) + eps);
    }
    PK_NOSW_LEAVE();
}

/* ================================================================== */
/* persistence — pure serialization into a caller buffer              */
/* ================================================================== */
/*
 *  st_save/st_load are libc-light (no file IO here — the kernel's durable
 *  layer lives a tier up, in student_shell.c). They just (de)serialize the
 *  trainable state into a flat byte blob with a fixed-width header that pins
 *  the blob to THIS build's architecture (vocab/d_model/layers/experts/dff)
 *  and n_params. The payload is w[] + Adam moments mu[]/vu[] + adam_t, so a
 *  reloaded baby continues training from exactly where it slept (not just a
 *  weight snapshot). All scalars are little-endian on the wire by virtue of
 *  raw byte copy — the format is single-host (one node's own ark file), not
 *  a cross-arch interchange, so no endianness swap is performed (honest).
 *
 *  On-disk layout:
 *    [ ST_BLOB_HDR ][ w  : n_params float ][ mu : n_params float ]
 *                   [ vu : n_params float ][ adam_t : int32      ]
 */

/* fixed magic 'N','S','1','W' so a stray file can't be mistaken for weights.
 * SS-2: the header now carries the resident TIER + the RUNTIME dims (m->*), and
 * st_load REFUSES a blob whose tier/dims do not match the resident model (fail-
 * closed -> the caller keeps its fresh-init weights, never loads a wrong shape).
 * The wire format is the SAME 10 u32 fields as before (reserved -> tier), so
 * the M-tier blob byte size + layout is UNCHANGED — only the reserved field,
 * which an M-tier save always wrote as 0, now equals ST_TIER_M (==1).          */
#define ST_BLOB_MAGIC 0x5731534Eu    /* "NS1W" little-endian                 */
#define ST_BLOB_VER   1u

typedef struct {
    uint32_t magic;      /* ST_BLOB_MAGIC                                     */
    uint32_t version;    /* ST_BLOB_VER                                       */
    uint32_t ns_ver;     /* NS_STUDENT_VER (student contract version)         */
    uint32_t n_params;   /* dimension guard (derived from the runtime dims)   */
    uint32_t vocab;      /* ST_VOCAB (tier-invariant)                         */
    uint32_t d_model;    /* m->d      (RUNTIME tier dim)                      */
    uint32_t n_layer;    /* m->nlayer (RUNTIME tier dim)                      */
    uint32_t n_expert;   /* m->nexpert(RUNTIME tier dim)                      */
    uint32_t dff;        /* m->dff    (RUNTIME tier dim)                      */
    uint32_t tier;       /* SS-2: m->tier (ST_TIER_S/_M/_L) — was 'reserved'  */
} ST_BLOB_HDR;

/* Exact byte size of a saved student for THIS build. The caller sizes its
 * durable buffer with this (compile-time-derivable, but a fn keeps n_params
 * encapsulated). */
size_t st_blob_size(const st_model *m)
{
    if (!m) return 0;
    size_t np = (size_t)m->n_params;
    return sizeof(ST_BLOB_HDR)
         + np * sizeof(float)       /* w  */
         + np * sizeof(float)       /* mu */
         + np * sizeof(float)       /* vu */
         + sizeof(int32_t);         /* adam_t */
}

/* Serialize the trainable state into buf[cap]. Returns the number of bytes
 * written, or negative (ST_E_ARG) if buf is too small / model unallocated. */
PK_NOSW_FN(long, st_save, (const st_model *m, void *buf, size_t cap), (m, buf, cap))
{
    if (!m || !m->w || !buf) return ST_E_ARG;
    size_t need = st_blob_size(m);
    if (cap < need) return ST_E_ARG;

    unsigned char *p = (unsigned char *)buf;
    ST_BLOB_HDR h;
    h.magic    = ST_BLOB_MAGIC;
    h.version  = ST_BLOB_VER;
    h.ns_ver   = (uint32_t)NS_STUDENT_VER;
    h.n_params = (uint32_t)m->n_params;
    h.vocab    = (uint32_t)ST_VOCAB;     /* tier-invariant */
    h.d_model  = (uint32_t)m->d;         /* RUNTIME tier dims */
    h.n_layer  = (uint32_t)m->nlayer;
    h.n_expert = (uint32_t)m->nexpert;
    h.dff      = (uint32_t)m->dff;
    h.tier     = (uint32_t)m->tier;      /* SS-2 tier byte */

    size_t np = (size_t)m->n_params, off = 0;
    size_t fbytes = np * sizeof(float);
    for (size_t i = 0; i < sizeof h; i++) p[off + i] = ((const unsigned char *)&h)[i];
    off += sizeof h;
    for (size_t i = 0; i < fbytes; i++) p[off + i] = ((const unsigned char *)m->w )[i];
    off += fbytes;
    for (size_t i = 0; i < fbytes; i++) p[off + i] = ((const unsigned char *)m->mu)[i];
    off += fbytes;
    for (size_t i = 0; i < fbytes; i++) p[off + i] = ((const unsigned char *)m->vu)[i];
    off += fbytes;
    int32_t t = (int32_t)m->adam_t;
    for (size_t i = 0; i < sizeof t; i++) p[off + i] = ((const unsigned char *)&t)[i];
    off += sizeof t;
    return (long)off;
}

/* Load a previously-saved blob into m (which MUST already be st_init'd to the
 * same build — st_load reuses its arena). Verifies magic/version/dims against
 * THIS build and refuses any mismatch (returns negative). On success the
 * weights, Adam moments, and timestep are overwritten and ST_OK is returned. */
/* SS-3: the SINGLE fail-closed predicate for "is `buf` a student blob that
 * EXACTLY matches the resident model `m`'s tier+shape?".  Both st_load (which
 * then copies the payload) and st_merge_cohort (which then averages it) gate on
 * this — one source of truth for the tier/dim guard.  Pure read; never mutates
 * m.  Returns 1 (accept) / 0 (refuse).  A blob shorter than the full payload is
 * refused too (the SS-2 truncation check, folded in here). */
int st_blob_tier_ok(const st_model *m, const void *buf, size_t len)
{
    if (!m || !buf) return 0;
    if (len < sizeof(ST_BLOB_HDR)) return 0;

    ST_BLOB_HDR h;
    const unsigned char *p = (const unsigned char *)buf;
    for (size_t i = 0; i < sizeof h; i++) ((unsigned char *)&h)[i] = p[i];

    /* fail-closed: a blob whose TIER or any dim differs from the resident model
     * is REFUSED.  Never accept a mismatched-shape blob — that would mis-read
     * the flat float payload.  The tier check is the SS-2 addition; the per-dim
     * checks (against the RUNTIME m->* dims) catch any same-tier shape drift. */
    if (h.magic    != ST_BLOB_MAGIC)            return 0;
    if (h.version  != ST_BLOB_VER)              return 0;
    if (h.ns_ver   != (uint32_t)NS_STUDENT_VER) return 0;
    if (h.tier     != (uint32_t)m->tier)        return 0;   /* SS-2 tier guard */
    if (h.n_params != (uint32_t)m->n_params)    return 0;
    if (h.vocab    != (uint32_t)ST_VOCAB)       return 0;
    if (h.d_model  != (uint32_t)m->d)           return 0;
    if (h.n_layer  != (uint32_t)m->nlayer)      return 0;
    if (h.n_expert != (uint32_t)m->nexpert)     return 0;
    if (h.dff      != (uint32_t)m->dff)         return 0;
    if (len < st_blob_size(m))                  return 0;   /* truncated payload */
    return 1;
}

PK_NOSW_FN(int, st_load, (st_model *m, const void *buf, size_t len), (m, buf, len))
{
    if (!m || !m->w || !buf) return ST_E_ARG;
    /* the SAME fail-closed tier/shape guard the merge reuses (SS-3). */
    if (!st_blob_tier_ok(m, buf, len)) return ST_E_ARG;

    const unsigned char *p = (const unsigned char *)buf;
    ST_BLOB_HDR h;
    for (size_t i = 0; i < sizeof h; i++) ((unsigned char *)&h)[i] = p[i];

    size_t np = (size_t)m->n_params;
    size_t fbytes = np * sizeof(float);

    size_t off = sizeof h;
    for (size_t i = 0; i < fbytes; i++) ((unsigned char *)m->w )[i] = p[off + i];
    off += fbytes;
    for (size_t i = 0; i < fbytes; i++) ((unsigned char *)m->mu)[i] = p[off + i];
    off += fbytes;
    for (size_t i = 0; i < fbytes; i++) ((unsigned char *)m->vu)[i] = p[off + i];
    off += fbytes;
    int32_t t = 0;
    for (size_t i = 0; i < sizeof t; i++) ((unsigned char *)&t)[i] = p[off + i];
    m->adam_t = (int)t;
    return ST_OK;
}

/* read one float from a (possibly unaligned) student-blob byte stream at the
 * w[] payload region: w starts immediately after the header. idx is a float
 * index into w[]. Byte copy -> alignment-safe on every arch. */
static float st_blob_w_at(const void *buf, size_t idx)
{
    const unsigned char *p = (const unsigned char *)buf + sizeof(ST_BLOB_HDR);
    p += idx * sizeof(float);
    float f;
    for (size_t i = 0; i < sizeof f; i++) ((unsigned char *)&f)[i] = p[i];
    return f;
}

/* ------------------------------------------------------------------ */
/* SS-3 — same-tier merge cohort (special-structure-mind.md §3.2/§8.4) */
/*                                                                     */
/* The student's OWN, isolated weight merge.  It does NOT call gl_merge */
/* / gl_merge_w and does NOT touch R3's rw[] (the [baby-merge-isolation]*/
/* tripwire); it averages ONLY same-tier student w[] bodies.  Peer-     */
/* symmetric + order-independent by a CANONICAL reduction (into first,  */
/* then peers ascending), plain sum then one divide — no reassociated   */
/* sums, no new transcendental, no libc math (wave-49 one-math).        */
/* ------------------------------------------------------------------ */
PK_NOSW_FN(int, st_merge_cohort, (st_model *into,
                    const void *const *peer_blobs, const size_t *peer_lens,
                    int count), (into, peer_blobs, peer_lens, count))
{
    if (!into || !into->w) return ST_E_ARG;
    if (count < 0) return ST_E_ARG;
    if (count > 0 && (!peer_blobs || !peer_lens)) return ST_E_ARG;

    /* PASS 1 — tier guard: accept only peers whose tier+shape EXACTLY match the
     * resident model (st_blob_tier_ok, the same fail-closed predicate st_load
     * uses).  A cross-tier / wrong-shape / short blob is SKIPPED, never coerced
     * (islands by construction).  We record acceptance so PASS 2 reduces over a
     * fixed canonical set. */
    int accepted = 0;
    for (int k = 0; k < count; k++) {
        if (st_blob_tier_ok(into, peer_blobs[k], peer_lens[k])) accepted++;
    }
    if (accepted == 0) return 0;   /* no cohort peer -> model BYTE-UNCHANGED */

    /* PASS 2 — per-parameter canonical reduction.  For each weight i:
     *   sum = into->w[i]  +  Σ_{accepted peers k, ascending} peer_k.w[i]
     *   into->w[i] = sum / (accepted + 1)
     * The summation order is FIXED (self, then peers by ascending index), so
     * merge(A,{B}) and merge(B,{A}) fold the SAME multiset in the SAME order
     * -> byte-identical, and the result is identical across arches under
     * -ffp-contract=off. */
    float denom = (float)(accepted + 1);
    int np = into->n_params;
    for (int i = 0; i < np; i++) {
        float s = into->w[i];
        for (int k = 0; k < count; k++) {
            if (!st_blob_tier_ok(into, peer_blobs[k], peer_lens[k])) continue;
            s += st_blob_w_at(peer_blobs[k], (size_t)i);
        }
        into->w[i] = s / denom;
    }

    /* Adam moments are optimizer state, not model: averaging foreign moments is
     * meaningless, so the merged model resumes Adam FRESH (standard FedAvg).   */
    for (int i = 0; i < np; i++) { into->mu[i] = 0.0f; into->vu[i] = 0.0f; }
    into->adam_t = 0;
    return accepted;
}

/* ================================================================== */
/* brain self-quantization, stage 1 (st_quant_fake — see student.h)   */
/* ================================================================== */

/* RTN one group of n weights: src a[] -> dequantized b[]. Symmetric absmax,
 * round half away from zero (own code, no libc), clamp to [-qmax-1, qmax].   */
/* the grid, shared by RTN and GPTQ (stage 2): the scale of one group, 0 for
 * an all-zero group, and one value rounded onto it */
static float st_quant_scale(const float *a, int n, int qmax)
{
    float amax = 0.0f;
    for (int i = 0; i < n; i++) {
        float v = a[i] < 0.0f ? -a[i] : a[i];
        if (v > amax) amax = v;
    }
    if (amax == 0.0f) return 0.0f;
    float s = amax / (float)qmax;
#if defined(ST_QUANT_BADSCALE)
    s = s * 0.25f;                                /* negative control: clips */
#endif
    return s;
}

static float st_quant_one(float w, float s, int qmax)
{
    float r = w / s;
    int q = (int)(r >= 0.0f ? r + 0.5f : r - 0.5f);
    if (q > qmax) q = qmax;
    if (q < -qmax - 1) q = -qmax - 1;
    return (float)q * s;
}

static void st_quant_group(const float *a, float *b, int n, int qmax)
{
#if defined(ST_QUANT_NOOP)
    (void)qmax;
    for (int i = 0; i < n; i++) b[i] = a[i];      /* negative control */
#else
    float s = st_quant_scale(a, n, qmax);
    if (s == 0.0f) { for (int i = 0; i < n; i++) b[i] = 0.0f; return; }
    for (int i = 0; i < n; i++) b[i] = st_quant_one(a[i], s, qmax);
#endif
}

/* One 2-D matrix [rows][cols] at w-offset `off`: split into groups per gran,
 * return the packed bytes (int payload + one fp32 scale per group). */
static size_t st_quant_mat(const st_model *src, st_model *dst, int off,
                           int rows, int cols, int bits, int gran)
{
    int total = rows * cols;
    int glen  = gran == ST_QG_TENSOR ? total : gran == ST_QG_G32 ? 32 : cols;
    int qmax  = (1 << (bits - 1)) - 1;
    size_t ngroups = 0;
    for (int g0 = 0; g0 < total; g0 += glen) {
        int n = glen; if (g0 + n > total) n = total - g0;
        st_quant_group(src->w + off + g0, dst->w + off + g0, n, qmax);
        ngroups++;
    }
    return ((size_t)total * (size_t)bits + 7u) / 8u + ngroups * sizeof(float);
}

int st_quant_fake(const st_model *src, st_model *dst, int bits, int gran,
                  size_t *bytes_q)
{
    if (!src || !dst || !src->w || !dst->w || src == dst) return ST_E_ARG;
    if (bits != 8 && bits != 4 && bits != 2) return ST_E_ARG;
    if (gran != ST_QG_ROW && gran != ST_QG_G32 && gran != ST_QG_TENSOR) return ST_E_ARG;
    if (dst->tier != src->tier || dst->d != src->d || dst->dff != src->dff ||
        dst->nlayer != src->nlayer || dst->nexpert != src->nexpert ||
        dst->n_params != src->n_params) return ST_E_ARG;
    ST_DIMS(src);                 /* V is the file-wide ST_VOCAB macro */

    /* SS-4 alive mask: mirror src's so dst selects the same live experts. */
    if (dst->alive) { free(dst->alive); dst->alive = NULL; }
    if (src->alive) {
        dst->alive = (int8_t *)malloc((size_t)E);
        if (!dst->alive) return ST_E_OOM;
        for (int e = 0; e < E; e++) dst->alive[e] = src->alive[e];
    }

    /* fp32 everywhere first (norms, router kept verbatim), then the matrices. */
    int np = src->n_params;
    for (int i = 0; i < np; i++) {
        dst->w[i] = src->w[i]; dst->g[i] = 0.0f; dst->mu[i] = 0.0f; dst->vu[i] = 0.0f;
    }
    dst->adam_t = 0;

    size_t bq = 0, nq = 0;
    bq += st_quant_mat(src, dst, src->o_embed, V, D, bits, gran); nq += (size_t)V * D;
    for (int l = 0; l < L; l++) {
        bq += st_quant_mat(src, dst, src->o_wq + l*D*D, D, D, bits, gran);
        bq += st_quant_mat(src, dst, src->o_wk + l*D*D, D, D, bits, gran);
        bq += st_quant_mat(src, dst, src->o_wv + l*D*D, D, D, bits, gran);
        bq += st_quant_mat(src, dst, src->o_wo + l*D*D, D, D, bits, gran);
        nq += (size_t)4 * D * D;
    }
    for (int x = 0; x < L * E; x++) {
        bq += st_quant_mat(src, dst, src->o_w1 + x*DFF*D, DFF, D, bits, gran);
        bq += st_quant_mat(src, dst, src->o_w3 + x*DFF*D, DFF, D, bits, gran);
        bq += st_quant_mat(src, dst, src->o_w2 + x*D*DFF, D, DFF, bits, gran);
        nq += (size_t)3 * D * DFF;
    }
    bq += st_quant_mat(src, dst, src->o_out, V, D, bits, gran); nq += (size_t)V * D;
    bq += ((size_t)np - nq) * sizeof(float);      /* the fp32 remainder */
    if (bytes_q) *bytes_q = bq;
    return ST_OK;
}

/* ================================================================== */
/* brain self-quantization, stage 2: GPTQ (see student.h)             */
/* ================================================================== */

/* H = sum x x^T of each matrix's inputs over the forward of calib (double).
 * One H per input: a_in (wq/wk/wv) and attn_o (wo) per layer, f_in (w1/w3)
 * and e_h (w2) per (layer, expert) over the positions routed to it, o_in (out). */
typedef struct {
    double *ha, *ho, *hf, *hh, *hout;
    int    *cnt;                                   /* positions per (l, e) */
} st_gq_h;

static void gq_free(st_gq_h *h)
{
    free(h->ha); free(h->ho); free(h->hf); free(h->hh); free(h->hout); free(h->cnt);
}

static void gq_outer(double *H, const float *x, int n)  /* upper triangle */
{
    for (int i = 0; i < n; i++) {
        double xi = (double)x[i];
        if (xi == 0.0) continue;
        double *Hi = H + (size_t)i * n;
        for (int k = i; k < n; k++) Hi[k] += xi * (double)x[k];
    }
}

static void gq_mirror(double *H, int n)
{
    for (int i = 0; i < n; i++)
        for (int k = i + 1; k < n; k++) H[(size_t)k * n + i] = H[(size_t)i * n + k];
}

static int gq_collect(st_model *m, const uint8_t *calib, int ncalib, int seq, st_gq_h *h)
{
    ST_DIMS(m);
    size_t dd = (size_t)D * D, ff = (size_t)DFF * DFF;
    h->ha = (double *)calloc((size_t)L * dd, sizeof(double));
    h->ho = (double *)calloc((size_t)L * dd, sizeof(double));
    h->hf = (double *)calloc((size_t)L * E * dd, sizeof(double));
    h->hh = (double *)calloc((size_t)L * E * ff, sizeof(double));
    h->hout = (double *)calloc(dd, sizeof(double));
    h->cnt = (int *)calloc((size_t)L * E, sizeof(int));
    float *logits = (float *)malloc((size_t)seq * V * sizeof(float));
    if (!h->ha || !h->ho || !h->hf || !h->hh || !h->hout || !h->cnt || !logits) {
        free(logits); return ST_E_OOM;
    }
    for (int o = 0; o + seq <= ncalib; o += seq) {
        int rc = st_forward(m, calib + o, seq, logits);
        if (rc != ST_OK) { free(logits); return rc; }
        const st_cache *c = (const st_cache *)m->cache;
        int n = c->n;
        for (int l = 0; l < L; l++)
            for (int t = 0; t < n; t++) {
                size_t lt = (size_t)l * n + t;
                gq_outer(h->ha + (size_t)l * dd, c->a_in + lt * D, D);
                gq_outer(h->ho + (size_t)l * dd, c->attn_o + lt * D, D);
                int nk = c->topk_n[lt];
                for (int j = 0; j < nk; j++) {
                    int e = c->topk_e[lt * E + j];
                    if (e < 0 || e >= E) continue;
                    gq_outer(h->hf + ((size_t)l * E + e) * dd, c->f_in + lt * D, D);
                    gq_outer(h->hh + ((size_t)l * E + e) * ff,
                             c->e_h + (lt * E + j) * DFF, DFF);
                    h->cnt[l * E + e]++;
                }
            }
        for (int t = 0; t < n; t++) gq_outer(h->hout, c->o_in + (size_t)t * D, D);
    }
    free(logits);
    for (int l = 0; l < L; l++) { gq_mirror(h->ha + (size_t)l * dd, D); gq_mirror(h->ho + (size_t)l * dd, D); }
    for (int x = 0; x < L * E; x++) { gq_mirror(h->hf + (size_t)x * dd, D); gq_mirror(h->hh + (size_t)x * ff, DFF); }
    gq_mirror(h->hout, D);
    return ST_OK;
}

/* sqrt by Newton in double (own code, no libm: the hosted kernel links none) */
static double gq_sqrt(double s)
{
    if (!(s > 0.0)) return 0.0;
    double x = s > 1.0 ? s : 1.0;
    for (int i = 0; i < 200; i++) {
        double nx = 0.5 * (x + s / x);
        if (nx == x) break;
        x = nx;
    }
    return x;
}

/* in-place lower Cholesky of A [n][n]; 0 if not positive definite */
static int gq_chol(double *A, int n)
{
    for (int j = 0; j < n; j++) {
        double s = A[(size_t)j * n + j];
        for (int k = 0; k < j; k++) s -= A[(size_t)j * n + k] * A[(size_t)j * n + k];
        if (!(s > 0.0)) return 0;
        double d = gq_sqrt(s);
        A[(size_t)j * n + j] = d;
        for (int i = j + 1; i < n; i++) {
            double t = A[(size_t)i * n + j];
            for (int k = 0; k < j; k++) t -= A[(size_t)i * n + k] * A[(size_t)j * n + k];
            A[(size_t)i * n + j] = t / d;
        }
        for (int k = j + 1; k < n; k++) A[(size_t)j * n + k] = 0.0;
    }
    return 1;
}

/* U (upper, U^T U = (H + lambda I)^-1) into U [n][n]; 0 on failure.
 * Dead inputs (H_jj == 0) get H_jj = 1, as the reference code does. */
static int gq_hinv_upper(const double *H, int n, double *U, double *T)
{
    double tr = 0.0;
    for (int i = 0; i < n; i++) tr += H[(size_t)i * n + i];
    double lam = 0.01 * tr / (double)n;
    for (size_t i = 0; i < (size_t)n * n; i++) T[i] = H[i];
    for (int i = 0; i < n; i++) {
        if (T[(size_t)i * n + i] == 0.0) T[(size_t)i * n + i] = 1.0;
        T[(size_t)i * n + i] += lam;
    }
    if (!gq_chol(T, n)) return 0;                     /* T = Lc, lower */
    /* Linv into U (lower), by forward substitution, column by column */
    for (size_t i = 0; i < (size_t)n * n; i++) U[i] = 0.0;
    for (int j = 0; j < n; j++) {
        U[(size_t)j * n + j] = 1.0 / T[(size_t)j * n + j];
        for (int i = j + 1; i < n; i++) {
            double s = 0.0;
            for (int k = j; k < i; k++) s += T[(size_t)i * n + k] * U[(size_t)k * n + j];
            U[(size_t)i * n + j] = -s / T[(size_t)i * n + i];
        }
    }
    /* Ainv = Linv^T Linv into T */
    for (int i = 0; i < n; i++)
        for (int j = i; j < n; j++) {
            double s = 0.0;
            for (int k = j; k < n; k++) s += U[(size_t)k * n + i] * U[(size_t)k * n + j];
            T[(size_t)i * n + j] = s; T[(size_t)j * n + i] = s;
        }
    if (!gq_chol(T, n)) return 0;                     /* Ainv = Lu Lu^T */
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) U[(size_t)i * n + j] = j >= i ? T[(size_t)j * n + i] : 0.0;
    return 1;
}

/* GPTQ one [R][C] matrix at w-offset off of dst (fp32 on entry), row scales
 * from src (the original rows). H == NULL or a failed inverse: plain RTN. */
static void gq_mat(const st_model *src, st_model *dst, int off, int R, int C,
                   const double *H, int qmax, double *U, double *T)
{
    float *W = dst->w + off;
    const float *W0 = src->w + off;
    if (!H || !gq_hinv_upper(H, C, U, T)) {
        for (int r = 0; r < R; r++)
            st_quant_group(W0 + (size_t)r * C, W + (size_t)r * C, C, qmax);
        return;
    }
    float *s = (float *)malloc((size_t)R * sizeof(float));
    if (!s) {
        for (int r = 0; r < R; r++)
            st_quant_group(W0 + (size_t)r * C, W + (size_t)r * C, C, qmax);
        return;
    }
    for (int r = 0; r < R; r++) {
        s[r] = st_quant_scale(W0 + (size_t)r * C, C, qmax);
        for (int c = 0; c < C; c++) W[(size_t)r * C + c] = W0[(size_t)r * C + c];
    }
    for (int j = 0; j < C; j++) {
        double d = U[(size_t)j * C + j];
        for (int r = 0; r < R; r++) {
            float *Wr = W + (size_t)r * C;
            float w = Wr[j];
            float q = s[r] == 0.0f ? 0.0f : st_quant_one(w, s[r], qmax);
            Wr[j] = q;
#ifndef ST_GPTQ_NOCOMP
            double err = ((double)w - (double)q) / d;
            const double *Uj = U + (size_t)j * C;
            for (int k = j + 1; k < C; k++) Wr[k] = (float)((double)Wr[k] - err * Uj[k]);
#endif
        }
    }
    free(s);
}

/* sum over rows of (W0 - Wq)_r^T H (W0 - Wq)_r */
static double gq_obj_mat(const float *W0, const float *Wq, int R, int C, const double *H, double *dl)
{
    double tot = 0.0;
    for (int r = 0; r < R; r++) {
        for (int c = 0; c < C; c++) dl[c] = (double)W0[(size_t)r * C + c] - (double)Wq[(size_t)r * C + c];
        for (int i = 0; i < C; i++) {
            if (dl[i] == 0.0) continue;
            const double *Hi = H + (size_t)i * C;
            double a = 0.0;
            for (int k = 0; k < C; k++) a += Hi[k] * dl[k];
            tot += dl[i] * a;
        }
    }
    return tot;
}

/* Stage 3 (research/brain-quant-gptq-seq.md §2): the order in which the
 * matrices are quantized when H is re-collected from the partly quantized copy.
 * seq (default): per layer qkv -> wo -> w1,w3 -> w2, then out (4L+1 stages);
 * -DST_GPTQ_BLOCKSEQ: one stage per layer (the paper's §5 blocks), then out. */
#if defined(ST_GPTQ_BLOCKSEQ)
#define GQ_NSTAGE(L)         ((L) + 1)
#define GQ_STAGE(L, l, sub)  ((sub) < 0 ? (L) : (l))
#else
#define GQ_NSTAGE(L)         (4 * (L) + 1)
#define GQ_STAGE(L, l, sub)  ((sub) < 0 ? 4 * (L) : 4 * (l) + (sub))
#endif

/* mode 0: GPTQ every matrix but embed into dst; mode 1: the objective of q.
 * stage >= 0 (mode 0): only the matrices of that stage (GQ_STAGE). */
static double gq_walk_stage(int mode, const st_model *src, st_model *dst, const st_model *q,
                            const st_gq_h *h, int qmax, int qmax_att, int qmax_out, int stage)
{
    ST_DIMS(src);
    const int qmax_ffn = qmax;
    int nmax = D > DFF ? D : DFF;
    size_t dd = (size_t)D * D, ff = (size_t)DFF * DFF;
    double *U = (double *)malloc((size_t)nmax * nmax * sizeof(double));
    double *T = (double *)malloc((size_t)nmax * nmax * sizeof(double));
    if (!U || !T) { free(U); free(T); return -1.0; }
    double tot = 0.0;
#define GQ_ONE(st, off, R, C, H)                                                  \
    do {                                                                          \
        if (mode == 0) {                                                          \
            if (stage < 0 || stage == (st))                                       \
                gq_mat(src, dst, (off), (R), (C), (H), qmax, U, T);               \
        } else if (H) tot += gq_obj_mat(src->w + (off), q->w + (off), (R), (C), (H), U); \
    } while (0)
    qmax = qmax_att;                      /* stage 5: attention may have its own bits */
    for (int l = 0; l < L; l++) {
        const double *ha = h->ha + (size_t)l * dd, *ho = h->ho + (size_t)l * dd;
#if defined(ST_GPTQ_WRONG_H)
        ho = ha;                          /* negative control: the audit-54 hole */
#endif
        GQ_ONE(GQ_STAGE(L, l, 0), src->o_wq + l * D * D, D, D, ha);
        GQ_ONE(GQ_STAGE(L, l, 0), src->o_wk + l * D * D, D, D, ha);
        GQ_ONE(GQ_STAGE(L, l, 0), src->o_wv + l * D * D, D, D, ha);
        GQ_ONE(GQ_STAGE(L, l, 1), src->o_wo + l * D * D, D, D, ho);
    }
    qmax = qmax_ffn;
    for (int x = 0; x < L * E; x++) {
        int l = x / E;
        const double *hf = h->cnt[x] ? h->hf + (size_t)x * dd : NULL;
        const double *hh = h->cnt[x] ? h->hh + (size_t)x * ff : NULL;
        GQ_ONE(GQ_STAGE(L, l, 2), src->o_w1 + x * DFF * D, DFF, D, hf);
        GQ_ONE(GQ_STAGE(L, l, 2), src->o_w3 + x * DFF * D, DFF, D, hf);
        GQ_ONE(GQ_STAGE(L, l, 3), src->o_w2 + x * D * DFF, D, DFF, hh);
    }
    qmax = qmax_out;                      /* stage 4: out may have its own bits */
    GQ_ONE(GQ_STAGE(L, 0, -1), src->o_out, V, D, h->hout);
#undef GQ_ONE
    free(U); free(T);
    return tot;
}

static double gq_walk(int mode, const st_model *src, st_model *dst, const st_model *q,
                      const st_gq_h *h, int qmax)
{
    return gq_walk_stage(mode, src, dst, q, h, qmax, qmax, qmax, -1);
}

/* a scratch model holding src's weights (st_forward writes the cache, and src
 * is const) */
static int gq_scratch(const st_model *src, st_model *tmp)
{
    if (st_init_tier(tmp, 1u, src->tier) != 0) return ST_E_OOM;
    if (tmp->n_params != src->n_params) { st_free(tmp); return ST_E_ARG; }
    for (int i = 0; i < src->n_params; i++) tmp->w[i] = src->w[i];
    if (src->alive) {
        tmp->alive = (int8_t *)malloc((size_t)src->nexpert);
        if (!tmp->alive) { st_free(tmp); return ST_E_OOM; }
        for (int e = 0; e < src->nexpert; e++) tmp->alive[e] = src->alive[e];
    }
    return ST_OK;
}

/* packed bytes of one [R][C] ST_QG_ROW matrix at bits (0 = fp32, no scales);
 * the st_quant_mat count */
static size_t gq_mat_bytes(int R, int C, int bits)
{
    size_t total = (size_t)R * (size_t)C;
    if (bits == 0) return total * sizeof(float);
    return (total * (size_t)bits + 7u) / 8u + (size_t)R * sizeof(float);
}

/* embed of dst from src at bits (0 = fp32 verbatim), RTN on the row grid */
static void gq_embed(const st_model *src, st_model *dst, int bits)
{
    const int VD = V * src->d;
    if (bits == 0) {
        for (int i = 0; i < VD; i++) dst->w[src->o_embed + i] = src->w[src->o_embed + i];
        return;
    }
    (void)st_quant_mat(src, dst, src->o_embed, V, src->d, bits, ST_QG_ROW);
}

/* st_quant_gptq with embed at be, the attention matrices at ba and out at bo
 * (stage 4; stage 5 adds ba and the 3-bit grid; be == ba == bo == bits is
 * stage 3 itself, the same operations in the same order) */
static int gq_bits_ok(int b) { return b == 8 || b == 4 || b == 3 || b == 2; }

static int gq_mix(const st_model *src, st_model *dst, int bits, int be, int ba, int bo,
                  const uint8_t *calib, int ncalib, int seq, size_t *bytes_q)
{
    if (!calib || seq < 1 || seq > ST_MAXSEQ || ncalib < seq) return ST_E_ARG;
    if (!gq_bits_ok(bo) || !gq_bits_ok(ba) || (!gq_bits_ok(be) && be != 0))
        return ST_E_ARG;
    /* the RTN copy first: arg checks, alive mask, Adam zeroed, embed on the
     * grid and the packed size (the grid and the scales are the same) */
    size_t bq = 0;
    int rc = st_quant_fake(src, dst, bits, ST_QG_ROW, &bq);
    if (rc != ST_OK) return rc;
    if (be != bits) {
        bq = bq - gq_mat_bytes(V, src->d, bits) + gq_mat_bytes(V, src->d, be);
#if !defined(ST_QMIX_EMBH2)
        gq_embed(src, dst, be);          /* H is collected through this embed */
#endif
    }
    if (bo != bits) bq = bq - gq_mat_bytes(V, src->d, bits) + gq_mat_bytes(V, src->d, bo);
    if (ba != bits)
        bq = bq + (size_t)(4 * src->nlayer) *
                  (gq_mat_bytes(src->d, src->d, ba) - gq_mat_bytes(src->d, src->d, bits));
    if (bytes_q) *bytes_q = bq;
    int qmax = (1 << (bits - 1)) - 1, qmax_out = (1 << (bo - 1)) - 1;
    int qmax_att = (1 << (ba - 1)) - 1;
    st_model tmp;
#if defined(ST_GPTQ_NOSEQ)
    /* stage 2: every H from src's fp32 forward */
    rc = gq_scratch(src, &tmp);
    if (rc != ST_OK) return rc;
    st_gq_h h = { 0 };
    rc = gq_collect(&tmp, calib, ncalib, seq, &h);
    st_free(&tmp);
    if (rc == ST_OK && gq_walk_stage(0, src, dst, NULL, &h, qmax, qmax_att, qmax_out, -1) < 0.0) rc = ST_E_OOM;
    gq_free(&h);
#else
    /* stage 3: embed stays RTN (it is a lookup, and it shapes every later X);
     * every other matrix back to fp32, then stage by stage: H from the forward
     * of dst as it is now (earlier stages quantized), quantize this stage */
    const int VD = V * src->d;
    for (int i = 0; i < src->n_params; i++)
        if (i < src->o_embed || i >= src->o_embed + VD) dst->w[i] = src->w[i];
    const int nst = GQ_NSTAGE(src->nlayer);
    for (int st = 0; st < nst && rc == ST_OK; st++) {
        rc = gq_scratch(dst, &tmp);
        if (rc != ST_OK) break;
        st_gq_h h = { 0 };
        rc = gq_collect(&tmp, calib, ncalib, seq, &h);
        st_free(&tmp);
        if (rc == ST_OK && gq_walk_stage(0, src, dst, NULL, &h, qmax, qmax_att, qmax_out, st) < 0.0) rc = ST_E_OOM;
        gq_free(&h);
    }
#endif
#if defined(ST_QMIX_EMBH2)
    if (rc == ST_OK && be != bits) gq_embed(src, dst, be);  /* control: H saw bits */
#endif
    return rc;
}

int st_quant_gptq(const st_model *src, st_model *dst, int bits,
                  const uint8_t *calib, int ncalib, int seq, size_t *bytes_q)
{
    return gq_mix(src, dst, bits, bits, bits, bits, calib, ncalib, seq, bytes_q);
}

int st_quant_gptq_mix(const st_model *src, st_model *dst, int bits,
                      int bits_embed, int bits_out,
                      const uint8_t *calib, int ncalib, int seq, size_t *bytes_q)
{
#if defined(ST_QMIX_SWAP)
    { int t = bits_embed; bits_embed = bits_out; bits_out = t; }   /* control */
#endif
#if defined(ST_QMIX_EMB8)
    if (bits_embed != bits && bits_embed != 0) bits_embed = 8;      /* control */
#endif
    return gq_mix(src, dst, bits, bits_embed, bits, bits_out, calib, ncalib, seq, bytes_q);
}

double st_quant_layer_obj(const st_model *src, const st_model *q,
                          const uint8_t *calib, int ncalib, int seq)
{
    if (!src || !q || !calib || seq < 1 || seq > ST_MAXSEQ || ncalib < seq ||
        q->n_params != src->n_params) return -1.0;
    st_model tmp;
    if (gq_scratch(src, &tmp) != ST_OK) return -1.0;
    st_gq_h h = { 0 };
    int rc = gq_collect(&tmp, calib, ncalib, seq, &h);
    st_free(&tmp);
    double r = rc == ST_OK ? gq_walk(1, src, NULL, q, &h, 0) : -1.0;
    gq_free(&h);
    return r;
}

/* sum over rows of ((W0 - Wq) x)_r^2 for one input vector x */
static double gq_direct_one(const float *W0, const float *Wq, int R, int C, const float *x)
{
    double tot = 0.0;
    for (int r = 0; r < R; r++) {
        double a = 0.0;
        for (int k = 0; k < C; k++)
            a += ((double)W0[(size_t)r * C + k] - (double)Wq[(size_t)r * C + k]) * (double)x[k];
        tot += a * a;
    }
    return tot;
}

int st_quant_class_obj(const st_model *src, const st_model *q,
                       const uint8_t *calib, int ncalib, int seq, double out[ST_QC_N])
{
    if (!src || !q || !calib || !out || seq < 1 || seq > ST_MAXSEQ || ncalib < seq ||
        q->n_params != src->n_params) return ST_E_ARG;
    for (int c = 0; c < ST_QC_N; c++) out[c] = 0.0;
    st_model tmp;
    int rc = gq_scratch(src, &tmp);
    if (rc != ST_OK) return rc;
    ST_DIMS(src);
    float *logits = (float *)malloc((size_t)seq * V * sizeof(float));
    if (!logits) { st_free(&tmp); return ST_E_OOM; }
    const float *W0 = src->w, *Wq = q->w;
    for (int o = 0; o + seq <= ncalib && rc == ST_OK; o += seq) {
        rc = st_forward(&tmp, calib + o, seq, logits);
        if (rc != ST_OK) break;
        const st_cache *c = (const st_cache *)tmp.cache;
        int n = c->n;
        for (int l = 0; l < L; l++)
            for (int t = 0; t < n; t++) {
                size_t lt = (size_t)l * n + t;
                const float *a = c->a_in + lt * D, *ao = c->attn_o + lt * D;
                int ow = l * D * D;
                out[ST_QC_WQ] += gq_direct_one(W0 + src->o_wq + ow, Wq + src->o_wq + ow, D, D, a);
                out[ST_QC_WK] += gq_direct_one(W0 + src->o_wk + ow, Wq + src->o_wk + ow, D, D, a);
                out[ST_QC_WV] += gq_direct_one(W0 + src->o_wv + ow, Wq + src->o_wv + ow, D, D, a);
                out[ST_QC_WO] += gq_direct_one(W0 + src->o_wo + ow, Wq + src->o_wo + ow, D, D, ao);
                int nk = c->topk_n[lt];
                for (int j = 0; j < nk; j++) {
                    int e = c->topk_e[lt * E + j];
                    if (e < 0 || e >= E) continue;
                    int x = l * E + e;
                    const float *fi = c->f_in + lt * D, *eh = c->e_h + (lt * E + j) * DFF;
                    out[ST_QC_W1] += gq_direct_one(W0 + src->o_w1 + x * DFF * D, Wq + src->o_w1 + x * DFF * D, DFF, D, fi);
                    out[ST_QC_W3] += gq_direct_one(W0 + src->o_w3 + x * DFF * D, Wq + src->o_w3 + x * DFF * D, DFF, D, fi);
                    out[ST_QC_W2] += gq_direct_one(W0 + src->o_w2 + x * D * DFF, Wq + src->o_w2 + x * D * DFF, D, DFF, eh);
                }
            }
        for (int t = 0; t < n; t++)
            out[ST_QC_OUT] += gq_direct_one(W0 + src->o_out, Wq + src->o_out, V, D, c->o_in + (size_t)t * D);
    }
    free(logits);
    st_free(&tmp);
    return rc;
}

/* ---- stage 5 (research/brain-quant-alloc.md): the node places the bits ---- */

int st_quant_gptq_bits(const st_model *src, st_model *dst, int bits,
                       int bits_embed, int bits_attn, int bits_out,
                       const uint8_t *calib, int ncalib, int seq, size_t *bytes_q)
{
    return gq_mix(src, dst, bits, bits_embed, bits_attn, bits_out, calib, ncalib, seq, bytes_q);
}

/* the empirical Fisher: F[i] = mean over the windows of g_i^2, g the gradient
 * of that window's mean loss (st_backward on a scratch copy; src untouched) */
int st_quant_fisher(const st_model *src, const uint8_t *calib, int ncalib,
                    int seq, double *F)
{
    if (!src || !calib || !F || seq < 1 || seq > ST_MAXSEQ || ncalib < seq) return ST_E_ARG;
    st_model tmp;
    int rc = gq_scratch(src, &tmp);
    if (rc != ST_OK) return rc;
    float *logits = (float *)malloc((size_t)seq * V * sizeof(float));
    if (!logits) { st_free(&tmp); return ST_E_OOM; }
    for (int i = 0; i < src->n_params; i++) F[i] = 0.0;
    int nw = 0;
    for (int o = 0; o + seq <= ncalib && rc == ST_OK; o += seq) {
        st_zero_grad(&tmp);
        rc = st_forward(&tmp, calib + o, seq, logits);
        if (rc != ST_OK) break;
        st_backward(&tmp, calib + o, seq);
        for (int i = 0; i < src->n_params; i++) F[i] += (double)tmp.g[i] * (double)tmp.g[i];
        nw++;
    }
    free(logits);
    st_free(&tmp);
    if (rc != ST_OK) return rc;
    if (nw == 0) return ST_E_ARG;
    for (int i = 0; i < src->n_params; i++) F[i] /= (double)nw;
    return ST_OK;
}

/* one [R][C] matrix at off, row grid: el == 0 is stage 5's
 * mean(F over it) * ||RTN_b(W) - W||^2; el == 1 is stage 6's
 * sum_i F_i (RTN_b(W)_i - W_i)^2 */
static double qa_omega_mat(const st_model *src, const double *F, int off, int R, int C,
                           int bits, int el)
{
    const float *W = src->w + off;
    int qmax = (1 << (bits - 1)) - 1;
    double fs = 0.0, e2 = 0.0, fe = 0.0;
    for (int r = 0; r < R; r++) {
        const float *Wr = W + (size_t)r * C;
        float s = st_quant_scale(Wr, C, qmax);
        for (int c = 0; c < C; c++) {
            double q = s == 0.0f ? 0.0 : (double)st_quant_one(Wr[c], s, qmax);
            double d = (double)Wr[c] - q;
            e2 += d * d;
#if !defined(ST_QALLOC_FLATFISHER)
            fs += F[off + (size_t)r * C + c];
            if (el) fe += F[off + (size_t)r * C + c] * d * d;
#endif
        }
    }
#if defined(ST_QALLOC_FLATFISHER)
    (void)F; fs = (double)R * (double)C;       /* control: every F = 1 */
    fe = e2;
#endif
    return el ? fe : fs / ((double)R * (double)C) * e2;
}

static double qa_omega(const st_model *src, const double *F, int bits,
                       int bits_embed, int bits_attn, int bits_out, int el)
{
    if (!src || !F || !gq_bits_ok(bits) || !gq_bits_ok(bits_embed) ||
        !gq_bits_ok(bits_attn) || !gq_bits_ok(bits_out)) return -1.0;
    ST_DIMS(src);
    double om = qa_omega_mat(src, F, src->o_embed, V, D, bits_embed, el);
    for (int l = 0; l < L; l++) {
        om += qa_omega_mat(src, F, src->o_wq + l * D * D, D, D, bits_attn, el);
        om += qa_omega_mat(src, F, src->o_wk + l * D * D, D, D, bits_attn, el);
        om += qa_omega_mat(src, F, src->o_wv + l * D * D, D, D, bits_attn, el);
        om += qa_omega_mat(src, F, src->o_wo + l * D * D, D, D, bits_attn, el);
    }
    for (int x = 0; x < L * E; x++) {
        om += qa_omega_mat(src, F, src->o_w1 + x * DFF * D, DFF, D, bits, el);
        om += qa_omega_mat(src, F, src->o_w3 + x * DFF * D, DFF, D, bits, el);
        om += qa_omega_mat(src, F, src->o_w2 + x * D * DFF, D, DFF, bits, el);
    }
    om += qa_omega_mat(src, F, src->o_out, V, D, bits_out, el);
    return om;
}

double st_quant_omega(const st_model *src, const double *F, int bits,
                      int bits_embed, int bits_attn, int bits_out)
{
    return qa_omega(src, F, bits, bits_embed, bits_attn, bits_out, 0);
}

double st_quant_omega_el(const st_model *src, const double *F, int bits,
                         int bits_embed, int bits_attn, int bits_out)
{
    return qa_omega(src, F, bits, bits_embed, bits_attn, bits_out, 1);
}

int st_quant_alloc_pick(const double *omega, int n)
{
    if (!omega || n < 1) return -1;
    int k = 0;
    for (int i = 1; i < n; i++) {
#if defined(ST_QALLOC_INVERT)
        if (omega[i] > omega[k]) k = i;        /* control: the largest */
#else
        if (omega[i] < omega[k]) k = i;
#endif
    }
    return k;
}

/* mean KL(a || b) per position over the windows of win, own exp/log in double
 * around st_expf/st_logf (no libm); -1 on error */
double st_quant_copy_kl(st_model *a, st_model *b, const uint8_t *win, int nwin, int seq)
{
    if (!a || !b || !win || seq < 1 || seq > ST_MAXSEQ || nwin < seq) return -1.0;
    float *la = (float *)malloc((size_t)seq * V * sizeof(float));
    float *lb = (float *)malloc((size_t)seq * V * sizeof(float));
    if (!la || !lb) { free(la); free(lb); return -1.0; }
    double s = 0.0; long np = 0;
    for (int o = 0; o + seq <= nwin; o += seq) {
        if (st_forward(a, win + o, seq, la) != ST_OK || st_forward(b, win + o, seq, lb) != ST_OK) {
            free(la); free(lb); return -1.0;
        }
        for (int t = 0; t < seq; t++) {
            const float *xa = la + (size_t)t * V, *xb = lb + (size_t)t * V;
            float ma = xa[0], mb = xb[0];
            for (int k = 1; k < V; k++) { if (xa[k] > ma) ma = xa[k]; if (xb[k] > mb) mb = xb[k]; }
            double za = 0.0, zb = 0.0;
            for (int k = 0; k < V; k++) { za += st_expf(xa[k] - ma); zb += st_expf(xb[k] - mb); }
            /* KL = sum p_a (log p_a - log p_b), log p = (x - m) - log z */
            double lza = st_logf((float)za), lzb = st_logf((float)zb), kl = 0.0;
            for (int k = 0; k < V; k++) {
                double pa = st_expf(xa[k] - ma) / za;
                if (pa <= 0.0) continue;
                kl += pa * (((double)(xa[k] - ma) - lza) - ((double)(xb[k] - mb) - lzb));
            }
            s += kl; np++;
        }
    }
    free(la); free(lb);
    return np ? s / (double)np : -1.0;
}

/* ================================================================== */
/* one-bit delta, stage 1 (st_delta_sign — see student.h)              */
/* ================================================================== */

static uint32_t st_dq_rng = 0x9E3779B9u;          /* ST_DQ_RANDSIGN only */

static void st_delta_mat(const st_model *base, const st_model *ft, st_model *dst,
                         int off, int n, int mode)
{
    const float *a = base->w + off, *b = ft->w + off;
    float *o = dst->w + off;
    double sum = 0.0; int nz = 0;
    for (int i = 0; i < n; i++) {
        float d = b[i] - a[i];
        sum += d < 0.0f ? -d : d;
        if (d != 0.0f) nz++;
    }
    float alpha = mode == ST_DQ_ZERO ? 0.0f : (float)(sum / (double)n);
    if (mode == ST_DQ_TERNARY) alpha = nz ? (float)(sum / (double)nz) : 0.0f;
    for (int i = 0; i < n; i++) {
        int neg;
        if (mode == ST_DQ_TERNARY && b[i] == a[i]) { o[i] = a[i]; continue; }
        if (mode == ST_DQ_RANDSIGN) {
            st_dq_rng ^= st_dq_rng << 13; st_dq_rng ^= st_dq_rng >> 17; st_dq_rng ^= st_dq_rng << 5;
            neg = (int)(st_dq_rng & 1u);
        } else {
            neg = (b[i] - a[i]) < 0.0f;
        }
        o[i] = a[i] + (neg ? -alpha : alpha);
    }
}

int st_delta_sign(const st_model *base, const st_model *ft, st_model *dst,
                  int mode, size_t *bytes)
{
    if (!base || !ft || !dst || !base->w || !ft->w || !dst->w) return ST_E_ARG;
    if (dst == base || dst == ft) return ST_E_ARG;
    if (mode != ST_DQ_SIGN && mode != ST_DQ_RANDSIGN && mode != ST_DQ_ZERO &&
        mode != ST_DQ_TERNARY) return ST_E_ARG;
    const st_model *s3[2] = { ft, dst };
    for (int k = 0; k < 2; k++)
        if (s3[k]->tier != base->tier || s3[k]->d != base->d || s3[k]->dff != base->dff ||
            s3[k]->nlayer != base->nlayer || s3[k]->nexpert != base->nexpert ||
            s3[k]->n_params != base->n_params) return ST_E_ARG;
    ST_DIMS(base);
    if (dst->alive) { free(dst->alive); dst->alive = NULL; }
    if (ft->alive) {
        dst->alive = (int8_t *)malloc((size_t)E);
        if (!dst->alive) return ST_E_OOM;
        for (int e = 0; e < E; e++) dst->alive[e] = ft->alive[e];
    }
    int np = base->n_params;
    for (int i = 0; i < np; i++) {                /* the fp32 remainder from ft */
        dst->w[i] = ft->w[i]; dst->g[i] = 0.0f; dst->mu[i] = 0.0f; dst->vu[i] = 0.0f;
    }
    dst->adam_t = 0;
    st_dq_rng = 0x9E3779B9u;                      /* same "random" every call */
    size_t nm = 0, nq = 0;
#define DQ(o, cnt) do { st_delta_mat(base, ft, dst, (o), (cnt), mode); nm++; nq += (size_t)(cnt); } while (0)
    DQ(base->o_embed, V * D);
    for (int l = 0; l < L; l++) {
        DQ(base->o_wq + l*D*D, D*D); DQ(base->o_wk + l*D*D, D*D);
        DQ(base->o_wv + l*D*D, D*D); DQ(base->o_wo + l*D*D, D*D);
    }
    for (int x = 0; x < L * E; x++) {
        DQ(base->o_w1 + x*DFF*D, DFF*D); DQ(base->o_w3 + x*DFF*D, DFF*D);
        DQ(base->o_w2 + x*D*DFF, D*DFF);
    }
    DQ(base->o_out, V * D);
#undef DQ
    size_t bits = mode == ST_DQ_TERNARY ? 2u : 1u;
    if (bytes) *bytes = (nq * bits + 7u) / 8u + nm * sizeof(float) + ((size_t)np - nq) * sizeof(float);
    return ST_OK;
}

/* ================================================================== */
/* smarter merge, stage 1 (st_merge_barrier / st_merge_guarded)        */
/* ================================================================== */

static int st_same_shape(const st_model *a, const st_model *b)
{
    return a && b && a->w && b->w && a->tier == b->tier && a->d == b->d &&
           a->dff == b->dff && a->nlayer == b->nlayer &&
           a->nexpert == b->nexpert && a->n_params == b->n_params;
}

static float st_windows_loss(st_model *m, const uint8_t *const *w, int n, int seqlen)
{
    double t = 0.0;
    for (int i = 0; i < n; i++) { int np = 0; t += st_eval_loss(m, w[i], seqlen, &np); }
    return (float)(t / n);
}

float st_merge_barrier(const st_model *a, const st_model *b, st_model *scratch,
                       const uint8_t *const *wins, int n, int seqlen,
                       float *curve)
{
    if (!st_same_shape(a, b) || !st_same_shape(a, scratch) || !wins || n < 1)
        return -1.0f;
    int np = a->n_params;
    float lo[ST_MERGE_NALPHA];
    float top = 0.0f;
    for (int k = 0; k < ST_MERGE_NALPHA; k++) {
        float al = (float)k / (float)(ST_MERGE_NALPHA - 1);
        for (int i = 0; i < np; i++)
            scratch->w[i] = (1.0f - al) * a->w[i] + al * b->w[i];
        lo[k] = st_windows_loss(scratch, wins, n, seqlen);
        if (curve) curve[k] = lo[k];
        if (k == 0 || lo[k] > top) top = lo[k];
    }
    return top - 0.5f * (lo[0] + lo[ST_MERGE_NALPHA - 1]);
}

static int st_cmp_desc(const void *x, const void *y)
{
    float a = *(const float *)x, b = *(const float *)y;
    return (a < b) - (a > b);
}

/* |tau| value at the top-20% boundary of tau = w - init (TIES "trim"). */
static float st_ties_threshold(const float *w, const float *init, int np)
{
    float *mag = (float *)malloc((size_t)np * sizeof(float));
    if (!mag) return -1.0f;
    for (int i = 0; i < np; i++) {
        float t = w[i] - init[i];
        mag[i] = t < 0.0f ? -t : t;
    }
    qsort(mag, (size_t)np, sizeof(float), st_cmp_desc);
    int k = np / 5; if (k < 1) k = 1;
    float thr = mag[k - 1];
    free(mag);
    return thr;
}

/* candidate `c` of (a, b) into dst[] (dst may alias a). */
static void st_merge_candidate(int c, const float *a, const float *b,
                               const float *init, float thrA, float thrB,
                               float *dst, int np)
{
    if (c < ST_MERGE_NALPHA) {
        float al = (float)c / (float)(ST_MERGE_NALPHA - 1);
        for (int i = 0; i < np; i++) dst[i] = (1.0f - al) * a[i] + al * b[i];
        return;
    }
    for (int i = 0; i < np; i++) {               /* TIES, lambda = 1 */
        float ta = a[i] - init[i], tb = b[i] - init[i];
        float ma = ta < 0.0f ? -ta : ta, mb = tb < 0.0f ? -tb : tb;
        if (ma < thrA) ta = 0.0f;
        if (mb < thrB) tb = 0.0f;
        float s = ta + tb, sum = 0.0f; int cnt = 0;
        if (s > 0.0f) {
            if (ta > 0.0f) { sum += ta; cnt++; }
            if (tb > 0.0f) { sum += tb; cnt++; }
        } else if (s < 0.0f) {
            if (ta < 0.0f) { sum += ta; cnt++; }
            if (tb < 0.0f) { sum += tb; cnt++; }
        }
        dst[i] = init[i] + (cnt ? sum / (float)cnt : 0.0f);
    }
}

PK_NOSW_FN(int, st_merge_guarded, (st_model *into, const st_model *peer,
                     const st_model *init, st_model *scratch,
                     const uint8_t *const *val, int n, int seqlen,
                     float *cand_loss), (into, peer, init, scratch, val, n, seqlen, cand_loss))
{
    if (cand_loss)
        for (int c = 0; c <= ST_MERGE_NALPHA; c++) cand_loss[c] = -1.0f;
    if (!st_same_shape(into, peer) || !st_same_shape(into, init) ||
        !st_same_shape(into, scratch) || !val || n < 1) return ST_E_ARG;
    int np = into->n_params;
    float thrA = st_ties_threshold(into->w, init->w, np);
    float thrB = st_ties_threshold(peer->w, init->w, np);
    if (thrA < 0.0f || thrB < 0.0f) return ST_E_OOM;

    int best = -1; float bl = 0.0f;
    for (int c = 0; c <= ST_MERGE_NALPHA; c++) {
#if defined(ST_MERGE_PLAIN_ONLY)
        if (c != ST_MERGE_NALPHA / 2) continue;   /* negative control */
#endif
        st_merge_candidate(c, into->w, peer->w, init->w, thrA, thrB, scratch->w, np);
        float l = st_windows_loss(scratch, val, n, seqlen);
        if (cand_loss) cand_loss[c] = l;
        if (best < 0 || l < bl) { best = c; bl = l; }
    }
    st_merge_candidate(best, into->w, peer->w, init->w, thrA, thrB, into->w, np);
    for (int i = 0; i < np; i++) { into->mu[i] = 0.0f; into->vu[i] = 0.0f; }
    into->adam_t = 0;
    return best;
}

/* ---- smarter merge, stage 3 (research/brain-merge-distill.md) ---- */

/* teacher row: softmax of the mean of two logit rows (temperature 1) */
static void st_teacher_row(const float *la, const float *lb, float *q)
{
    float mx = 0.0f;
    for (int o = 0; o < V; o++) {
#if defined(ST_DISTILL_ONE_TEACHER)
        q[o] = la[o]; (void)lb;                    /* negative control */
#else
        q[o] = 0.5f * (la[o] + lb[o]);
#endif
        if (o == 0 || q[o] > mx) mx = q[o];
    }
    float s = 0.0f;
    for (int o = 0; o < V; o++) { q[o] = st_expf(q[o] - mx); s += q[o]; }
    float inv = 1.0f / s;
    for (int o = 0; o < V; o++) q[o] *= inv;
}

PK_NOSW_FN(int, st_merge_distill, (st_model *into, st_model *peer,
                     const st_model *init, st_model *scratch,
                     const uint8_t *const *xfer, int nx,
                     const uint8_t *const *val, int nv, int seqlen,
                     int steps, float lr, int start, st_distill_stats *stats),
           (into, peer, init, scratch, xfer, nx, val, nv, seqlen, steps, lr,
            start, stats))
{
    st_distill_stats z = { -1, 0, 0.0f, 0.0f, 0 };
    if (stats) *stats = z;
    if (!st_same_shape(into, peer) || !st_same_shape(into, init) ||
        !st_same_shape(into, scratch) || !xfer || nx < 1 || !val || nv < 1 ||
        seqlen < 2 || seqlen > ST_MAXSEQ || steps < 0 ||
        (start != ST_DISTILL_START_PICK && start != ST_DISTILL_START_INIT &&
         start != ST_DISTILL_START_INIT_SWA))
        return ST_E_ARG;
    int np = into->n_params;

    /* the teacher's first parent is `into` as it is now: keep a copy */
    st_model ta;
    if (st_init_tier(&ta, 1, into->tier) != ST_OK) return ST_E_OOM;
    for (int i = 0; i < np; i++) ta.w[i] = into->w[i];
    size_t row = (size_t)seqlen * V;
    float *la = (float *)malloc(row * sizeof(float));
    float *lb = (float *)malloc(row * sizeof(float));
    float *lc = (float *)malloc(row * sizeof(float));
    float *q  = (float *)malloc(row * sizeof(float));
    if (!la || !lb || !lc || !q) {
        free(la); free(lb); free(lc); free(q); st_free(&ta); return ST_E_OOM;
    }

    /* (1) the guarded pick (scratch is free again afterwards) is the best
     * so far; the child starts there or, for stage 3b, from the common
     * ancestor (st_merge_guarded already reset the Adam state) */
    int pick = st_merge_guarded(into, peer, init, scratch, val, nv, seqlen, NULL);
    if (pick < 0) {
        free(la); free(lb); free(lc); free(q); st_free(&ta); return pick;
    }
    float best = st_windows_loss(into, val, nv, seqlen);
    z.init_pick = pick; z.val_before = best;
    for (int i = 0; i < np; i++) scratch->w[i] = into->w[i];   /* best so far */
    if (start != ST_DISTILL_START_PICK)
        for (int i = 0; i < np; i++) into->w[i] = init->w[i];

    /* (2)(3) KL(teacher || child) on the xfer windows, in order */
    for (int s = 1; s <= steps; s++) {
        const uint8_t *x = xfer[(s - 1) % nx];
        st_forward(&ta, x, seqlen, la);
        st_forward(peer, x, seqlen, lb);
        for (int t = 0; t < seqlen - 1; t++)
            st_teacher_row(la + (size_t)t * V, lb + (size_t)t * V, q + (size_t)t * V);
        st_zero_grad(into);
        st_forward(into, x, seqlen, lc);
        st_backward_q(into, x, seqlen, q);
        st_adam_step(into, lr);
        /* (4) keep the best on val (ties keep the earlier) */
        if (s % ST_DISTILL_EVAL == 0) {
            float l = st_windows_loss(into, val, nv, seqlen);
            if (l < best) {
                best = l; z.best_step = s;
                for (int i = 0; i < np; i++) scratch->w[i] = into->w[i];
            }
        }
    }
    for (int i = 0; i < np; i++) {
        into->w[i] = scratch->w[i]; into->mu[i] = 0.0f; into->vu[i] = 0.0f;
    }
    into->adam_t = 0;
    z.val_after = best;
    if (stats) *stats = z;
    free(la); free(lb); free(lc); free(q); st_free(&ta);
    return pick;
}

/* stages 4-7: both starts, keep the lower val; init_start is the init arm */
static int st_distill_sel_run(st_model *into, st_model *peer,
                              const st_model *init, st_model *scratch,
                              const uint8_t *const *xfer, int nx,
                              const uint8_t *const *val, int nv, int seqlen,
                              int steps_pick, float lr_pick, int steps_init,
                              float lr_init, int init_start,
                              st_distill_sel_stats *stats)
{
    st_distill_sel_stats z = { { -1, 0, 0.0f, 0.0f, 0 }, { -1, 0, 0.0f, 0.0f, 0 }, 0 };
    if (stats) *stats = z;
    if (!st_same_shape(into, peer)) return ST_E_ARG;
    int np = into->n_params;
    /* a0 = `into` as it is now (both starts begin from it); kp = the pick
     * child while the init child is made */
    st_model a0, kp;
    if (st_init_tier(&a0, 1, into->tier) != ST_OK) return ST_E_OOM;
    if (st_init_tier(&kp, 1, into->tier) != ST_OK) { st_free(&a0); return ST_E_OOM; }
    for (int i = 0; i < np; i++) a0.w[i] = into->w[i];

    int rp = st_merge_distill(into, peer, init, scratch, xfer, nx, val, nv,
                              seqlen, steps_pick, lr_pick,
                              ST_DISTILL_START_PICK, &z.pick);
    if (rp < 0) { st_free(&a0); st_free(&kp); return rp; }
    for (int i = 0; i < np; i++) { kp.w[i] = into->w[i]; into->w[i] = a0.w[i]; }
    int ri = st_merge_distill(into, peer, init, scratch, xfer, nx, val, nv,
                              seqlen, steps_init, lr_init,
                              init_start, &z.init);
    if (ri < 0) {
        for (int i = 0; i < np; i++) into->w[i] = a0.w[i];
        st_free(&a0); st_free(&kp); return ri;
    }
    /* the merging device does not know whether the parents saw the same
     * data: it keeps the lower val (ties keep pick) */
#if defined(ST_DISTILL_SEL_INVERT)
    z.chose_init = z.init.val_after > z.pick.val_after;
#elif defined(ST_DISTILL_SEL_PICK)
    z.chose_init = 0;
#elif defined(ST_DISTILL_SEL_INIT)
    z.chose_init = 1;
#else
    z.chose_init = z.init.val_after < z.pick.val_after;
#endif
    if (!z.chose_init)
        for (int i = 0; i < np; i++) into->w[i] = kp.w[i];
    /* both runs left the Adam state reset */
    if (stats) *stats = z;
    st_free(&a0); st_free(&kp);
    return rp;
}

PK_NOSW_FN(int, st_merge_distill_sel, (st_model *into, st_model *peer,
                     const st_model *init, st_model *scratch,
                     const uint8_t *const *xfer, int nx,
                     const uint8_t *const *val, int nv, int seqlen,
                     int steps_pick, float lr_pick, int steps_init,
                     float lr_init, st_distill_sel_stats *stats),
           (into, peer, init, scratch, xfer, nx, val, nv, seqlen, steps_pick,
            lr_pick, steps_init, lr_init, stats))
{
    return st_distill_sel_run(into, peer, init, scratch, xfer, nx, val, nv,
                              seqlen, steps_pick, lr_pick, steps_init, lr_init,
                              ST_DISTILL_START_INIT, stats);
}

PK_NOSW_FN(int, st_merge_distill_sel_swa, (st_model *into, st_model *peer,
                     const st_model *init, st_model *scratch,
                     const uint8_t *const *xfer, int nx,
                     const uint8_t *const *val, int nv, int seqlen,
                     int steps_pick, float lr_pick, int steps_init,
                     float lr_init, st_distill_sel_stats *stats),
           (into, peer, init, scratch, xfer, nx, val, nv, seqlen, steps_pick,
            lr_pick, steps_init, lr_init, stats))
{
    return st_distill_sel_run(into, peer, init, scratch, xfer, nx, val, nv,
                              seqlen, steps_pick, lr_pick, steps_init, lr_init,
                              ST_DISTILL_START_INIT_SWA, stats);
}

/* ================================================================== */
/* grad check                                                         */
/* ================================================================== */

/* The 13 weight families, as (offset, size) pairs, filled from the model. */
static int st_families(const st_model *m, int *off, int *sz)
{
    ST_DIMS(m);
    int k = 0;
    off[k] = m->o_embed;     sz[k++] = SZ_EMBED;
    off[k] = m->o_attn_norm; sz[k++] = SZ_ANORM;
    off[k] = m->o_wq;        sz[k++] = SZ_W;
    off[k] = m->o_wk;        sz[k++] = SZ_W;
    off[k] = m->o_wv;        sz[k++] = SZ_W;
    off[k] = m->o_wo;        sz[k++] = SZ_W;
    off[k] = m->o_ffn_norm;  sz[k++] = SZ_FNORM;
    off[k] = m->o_router;    sz[k++] = SZ_ROUTER;
    off[k] = m->o_w1;        sz[k++] = SZ_W1;
    off[k] = m->o_w3;        sz[k++] = SZ_W3;
    off[k] = m->o_w2;        sz[k++] = SZ_W2;
    off[k] = m->o_out_norm;  sz[k++] = SZ_ONORM;
    off[k] = m->o_out;       sz[k++] = SZ_OUT;
    return k;
}

/*
 *  Grad-check: analytic vs central finite differences. Two honesty measures
 *  make this a TRUSTWORTHY check rather than a noisy one:
 *    1) routing is FROZEN during the FD probes — top-K expert SELECTION is
 *       non-differentiable, so an FD step that straddles a routing boundary is
 *       a spurious ~O(10) spike that the analytic gradient (the derivative at
 *       fixed routing, which is exactly what Adam steps) correctly omits.
 *    2) within each weight family we probe the indices with the LARGEST
 *       analytic |gradient|: float32 finite differences of a tiny gradient
 *       (loss change below ~1e-6 of the loss magnitude) underflow to 0 and
 *       produce a meaningless rel-err. Checking the large-gradient indices is
 *       where the derivative is actually resolvable. `stride`/`eps` kept in the
 *       signature for the caller; `probes_per_family` is fixed at a small N.
 *  Returns the max relative error over the probed set.
 */
float st_grad_check(st_model *m, const uint8_t *bytes, int n, int stride, float eps)
{
    ST_DIMS(m);
    (void)stride;
    if (eps <= 0.0f) eps = 1e-2f;
    float *logits = (float *)malloc((size_t)n * V * sizeof(float));
    if (!logits) return 1e30f;

    st_zero_grad(m);
    st_forward(m, bytes, n, logits);   /* this also caches the routing choice */
    st_backward(m, bytes, n);
    /* snapshot the routing decision (chosen ids AND the adaptive per-token
     * width nk) so the FD probes replay the EXACT same selection — the FD step
     * must not straddle a routing boundary (selection AND width are both
     * non-differentiable). The snapshot survives the cache realloc each FD does. */
    st_cache *cc = (st_cache *)m->cache;
    size_t ncells = (size_t)L * n;
    int nroute = (int)(ncells * E);   /* cache topk_e is E-slot (runtime) */
    st_frozen_route = (int *)malloc((size_t)nroute * sizeof(int));
    st_frozen_n     = (int *)malloc(ncells * sizeof(int));
    if (st_frozen_route) for (int i = 0; i < nroute; i++) st_frozen_route[i] = cc->topk_e[i];
    if (st_frozen_n)     for (size_t i = 0; i < ncells; i++) st_frozen_n[i] = cc->topk_n[i];
    st_freeze_routing = (st_frozen_route && st_frozen_n) ? 1 : 0;  /* replay routing */

    int off[16], sz[16];
    int nfam = st_families(m, off, sz);
    const int PROBES = 6;              /* per family, the top-|g| indices      */

    float worst = 0.0f;
    for (int f = 0; f < nfam; f++) {
        /* find the PROBES largest-|analytic-grad| indices in this family */
        int   idx[8];  float mag[8];
        for (int p = 0; p < PROBES; p++) { idx[p] = -1; mag[p] = -1.0f; }
        for (int i = off[f]; i < off[f] + sz[f]; i++) {
            float a = m->g[i]; if (a < 0.0f) a = -a;
            /* insert into the small top list */
            if (a > mag[PROBES - 1]) {
                int p = PROBES - 1;
                while (p > 0 && a > mag[p - 1]) { mag[p] = mag[p - 1]; idx[p] = idx[p - 1]; p--; }
                mag[p] = a; idx[p] = i;
            }
        }
        for (int p = 0; p < PROBES; p++) {
            int i = idx[p];
            if (i < 0) continue;
            float orig = m->w[i];
            m->w[i] = orig + eps; float lp = st_eval_loss(m, bytes, n, NULL);
            m->w[i] = orig - eps; float lm = st_eval_loss(m, bytes, n, NULL);
            m->w[i] = orig;
            float fd  = (lp - lm) / (2.0f * eps);
            float an  = m->g[i];
            float ref = fd < 0.0f ? -fd : fd;
            float aa  = an < 0.0f ? -an : an;
            if (aa > ref) ref = aa;
            if (ref < 1e-2f) continue;     /* below FD resolution — skip */
            float diff = an - fd;
            if (diff < 0.0f) diff = -diff;
            float rel = diff / ref;
#ifdef ST_GC_VERBOSE
            { extern int printf(const char*,...);
              if (rel > 0.03f) printf("  GCV fam=%d idx=%d an=%.6f fd=%.6f rel=%.4f abs=%.6f\n", f, i, an, fd, rel, diff); }
#endif
            if (rel > worst) worst = rel;
        }
    }

    st_freeze_routing = 0;
    free(st_frozen_route); st_frozen_route = NULL;
    free(st_frozen_n);     st_frozen_n     = NULL;
    /* re-run forward so the cache matches the unperturbed weights again. */
    st_forward(m, bytes, n, logits);
    free(logits);
    return worst;
}
