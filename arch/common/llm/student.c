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
 *  No positional encoding and a single attention head: NS-1 deliberately keeps
 *  the smallest faithful organism so the analytic backward is grad-checkable
 *  end to end. RoPE / multi-head / vocab-merges are later growth levers
 *  (native-student.md §A.2/§A.4/§A.5), honestly deferred — see the NS report.
 */
#include "student.h"

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
    m->w = m->g = m->mu = m->vu = NULL;
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

/* `ne` = the resident model's expert count (m->nexpert) — the RUNTIME widening
 * ceiling (K_min..ne).  Scratch is bound to EMAX (fixed L-tier) — no VLA.  The
 * frozen-route slot stride is ne (the cache's E-slot stride; see cache_get). */
static int router_pick(const float *gate, int *topk_e, float *topk_w, int ne)
{
    /* full descending order of the ne experts by gate logit (E tiny: selection
     * sort).  order[0] = top1.  Scratch bound to EMAX (compile-time) — no VLA. */
    int order[EMAX];
    int used[EMAX];
    for (int e = 0; e < ne; e++) used[e] = 0;
    for (int r = 0; r < ne; r++) {
        int best = -1; float bv = -1e30f;
        for (int e = 0; e < ne; e++) {
            if (used[e]) continue;
            if (best < 0 || gate[e] > bv) { bv = gate[e]; best = e; }
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
         * from top1 to it is below THETA and we are under the model's E. */
        nk = K;
        float top1 = gate[order[0]];
        while (nk < ne && (top1 - gate[order[nk]]) < ST_K_THETA) nk++;
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

int st_forward(st_model *m, const uint8_t *bytes, int n, float *logits)
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
            int   *te = c->topk_e + ((size_t)l * n + t) * E;
            float *tw = c->topk_w + ((size_t)l * n + t) * E;
            int    nk = router_pick(gt, te, tw, E);
            c->topk_n[(size_t)l * n + t] = nk;   /* runtime firing width      */

            float moe[DMAX];   /* [no-vla] bound to the L-tier d_model */
            for (int i = 0; i < D; i++) moe[i] = 0.0f;
            for (int j = 0; j < nk; j++) {
                int e = te[j]; float wj = tw[j];
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
                /* down: out[D] = w2 . eh, weighted by wj */
                for (int i = 0; i < D; i++) {
                    const float *w2r = w2 + (size_t)i * DFF;
                    float acc = 0.0f;
                    for (int h = 0; h < DFF; h++) acc += w2r[h] * eh[h];
                    moe[i] += wj * acc;
                }
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
    {
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

    float *logits = (float *)malloc((size_t)ST_MAXSEQ * V * sizeof(float));
    if (!logits) return ST_E_OOM;

    uint64_t rng = seed ? seed : 0x9E3779B97F4A7C15ULL;

    int produced = 0;
    for (int g = 0; g < max_gen; g++) {
        int rc = st_forward(m, ctxbuf, nctx, logits);
        if (rc != ST_OK) break;
        const float *row = logits + (size_t)(nctx - 1) * V;   /* after last byte */
        int b = st_sample_byte(row, temp, top_k, &rng);

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

float st_eval_loss(st_model *m, const uint8_t *bytes, int n, int *n_pred)
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

/* ================================================================== */
/* backward                                                           */
/* ================================================================== */

float st_backward(st_model *m, const uint8_t *bytes, int n)
{
    ST_DIMS(m);
    st_cache *c = (st_cache *)m->cache;
    if (!c || c->n != n) return 0.0f;
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
        loss += -(double)st_logf(pr[tgt]);
        /* dL/dlogit = (p - onehot) * invN */
        for (int o = 0; o < V; o++) g_logit[o] = pr[o] * invN;
        g_logit[tgt] -= invN;
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
    for (int i = 0; i < m->n_params; i++) m->g[i] = 0.0f;
}

void st_adam_step(st_model *m, float lr)
{
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
long st_save(const st_model *m, void *buf, size_t cap)
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
int st_load(st_model *m, const void *buf, size_t len)
{
    if (!m || !m->w || !buf) return ST_E_ARG;
    if (len < sizeof(ST_BLOB_HDR)) return ST_E_ARG;

    ST_BLOB_HDR h;
    const unsigned char *p = (const unsigned char *)buf;
    for (size_t i = 0; i < sizeof h; i++) ((unsigned char *)&h)[i] = p[i];

    /* fail-closed: a blob whose TIER or any dim differs from the resident model
     * is REFUSED (the caller keeps its fresh-init weights).  Never load a
     * mismatched-shape blob — that would mis-read the flat float payload.  The
     * tier check is the SS-2 addition; the per-dim checks (now against the
     * RUNTIME m->* dims) catch any same-tier shape drift across builds too. */
    if (h.magic    != ST_BLOB_MAGIC)        return ST_E_ARG;
    if (h.version  != ST_BLOB_VER)          return ST_E_ARG;
    if (h.ns_ver   != (uint32_t)NS_STUDENT_VER) return ST_E_ARG;
    if (h.tier     != (uint32_t)m->tier)    return ST_E_ARG;   /* SS-2 tier guard */
    if (h.n_params != (uint32_t)m->n_params)    return ST_E_ARG;
    if (h.vocab    != (uint32_t)ST_VOCAB)   return ST_E_ARG;
    if (h.d_model  != (uint32_t)m->d)       return ST_E_ARG;
    if (h.n_layer  != (uint32_t)m->nlayer)  return ST_E_ARG;
    if (h.n_expert != (uint32_t)m->nexpert) return ST_E_ARG;
    if (h.dff      != (uint32_t)m->dff)     return ST_E_ARG;

    size_t np = (size_t)m->n_params;
    size_t fbytes = np * sizeof(float);
    size_t need = st_blob_size(m);
    if (len < need) return ST_E_ARG;   /* truncated payload — refuse */

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
