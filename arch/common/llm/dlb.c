/*
 *  dlb.c — DLB: test-time DELIBERATION loop (search x verify) + the compounding
 *  (search-distill) ring. depth_iq_path_design.md §3. See dlb.h for the full
 *  frame + honesty gates.
 *
 *  HOSTED-TIER ONLY. Public student.h API only; no student.c internals, no
 *  moe.c/dmn.c edit, no R3 crown. Bare-metal .text untouched -> crown-neutral.
 *  Build one-math: -O1 -ffp-contract=off. No VLA (candidate scratch is bound to
 *  the fixed DLB_TRACE_MAX / ST_GEN_CAP; the ring is static/heap-free).
 */
#include "dlb.h"

#include <stdlib.h>   /* malloc/free for the per-distill logits arena (hosted)  */

/* ------------------------------------------------------------------ */
/* deterministic per-candidate seed: H(query || i) — splitmix over FNV  */
/* ------------------------------------------------------------------ */
static uint64_t dlb_seed(const uint8_t *q, int qn, int i)
{
    uint64_t h = 1469598103934665603ULL;          /* FNV-1a offset */
    for (int k = 0; k < qn; k++) { h ^= q[k]; h *= 1099511628211ULL; }
    h ^= (uint64_t)(uint32_t)i; h *= 1099511628211ULL;
    /* splitmix64 finalizer so nearby i diverge well (path diversity). */
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27; h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return h ? h : 0x9E3779B97F4A7C15ULL;          /* never hand 0 to the RNG */
}

/* draft confidence = exp(-mean CE over the answer span) in (0,1]: the (1-p_max)
 * analogue for the byte mouth. Pure forward via st_span_ce on (query||answer):
 * the answer bytes sit at positions [qn, qn+an) and are scored from their
 * prefix. Reuses student.c's libc-free math (no new transcendental). */
static float dlb_answer_conf(st_model *m, const uint8_t *q, int qn,
                             const uint8_t *ans, int an)
{
    if (an <= 0) return 0.0f;
    uint8_t buf[DLB_TRACE_MAX];
    int n = 0;
    for (int i = 0; i < qn && n < DLB_TRACE_MAX; i++) buf[n++] = q[i];
    int t0 = n;
    for (int i = 0; i < an && n < DLB_TRACE_MAX; i++) buf[n++] = ans[i];
    int t1 = n;
    if (t0 < 1) t0 = 1;                 /* position 0 predicts from nothing */
    if (t1 <= t0) return 0.0f;
    float ce = st_span_ce(m, buf, n, t0, t1, NULL);
    return st_expf(-ce);               /* low CE -> high confidence */
}

/* ------------------------------------------------------------------ */
/* the DLB loop (§3.2): draft fast, revise slow by SEARCH x VERIFY.      */
/* ------------------------------------------------------------------ */
int dlb_answer(st_model *m, const uint8_t *query, int qn,
               uint8_t *out, int max_out,
               const dlb_budget *b, dlb_verify_fn verify, void *vctx,
               dlb_result *info)
{
    if (!m || !out || max_out <= 0 || !b) return ST_E_ARG;
    int K = b->K; if (K < 1) K = 1; if (K > DLB_KMAX) K = DLB_KMAX;
    int mg = b->max_gen; if (mg < 1) mg = 1; if (mg > max_out) mg = max_out;

    if (info) { info->k_used = 0; info->flipped = 0;
                info->draft_score = 0.0f; info->best_score = 0.0f;
                info->draft_conf = 0.0f; }

    /* candidate 0 == the reflex DRAFT (seed H(query,0)). */
    uint8_t cand[DLB_GEN_MAX];
    if (mg > DLB_GEN_MAX) mg = DLB_GEN_MAX;
    int dn = st_generate(m, query, qn, cand, mg, b->temp, b->top_k,
                         dlb_seed(query, qn, 0));
    if (dn < 0) return dn;

    /* best := draft */
    int best_n = dn;
    for (int i = 0; i < dn; i++) out[i] = cand[i];
    float draft_score = verify ? verify(query, qn, cand, dn, vctx) : 0.0f;
    float best_score  = draft_score;
    float draft_conf  = dlb_answer_conf(m, query, qn, cand, dn);
    int   k_used      = 1;

    /* cheapness gate + single-shot short-circuit: think hard ONLY when unsure.
     * theta_easy<=0 disables the early-out; K==1 IS the STUB-SEARCH path (no
     * search) and single-shot production. */
    int deliberate = (K > 1) && !(b->theta_easy > 0.0f && draft_conf >= b->theta_easy);

    if (deliberate) {
        for (int i = 1; i < K; i++) {
            uint8_t c[DLB_GEN_MAX];
            int cn = st_generate(m, query, qn, c, mg, b->temp, b->top_k,
                                 dlb_seed(query, qn, i));
            if (cn < 0) continue;                 /* a bad draw is skipped, not fatal */
            k_used++;
            float sc = verify ? verify(query, qn, c, cn, vctx) : 0.0f;
#ifdef DLB_SABOTAGE_NOVERIFY
            /* ANTI-THEATER falsifier (compile-time ONLY; the production binary has
             * NO runtime switch). Stub the search x verify SELECTION so a searched
             * candidate can never beat the draft -> dlb_answer always returns the
             * single-shot draft -> the [depth-deliberation] CURE arm collapses to
             * the floor -> RED. Proves the cert's gain is produced by dlb.c's
             * verify-selection, not by the harness (the compile-time stub-red
             * anti-theater pattern; the production binary has no such switch). */
            sc = -1.0f;
#endif
            if (sc > best_score) {                /* strict > : ties keep lower index */
                best_score = sc;
                best_n = cn;
                for (int j = 0; j < cn; j++) out[j] = c[j];
            }
        }
    }

    if (info) {
        info->k_used = k_used;
        info->draft_score = draft_score;
        info->best_score  = best_score;
        info->draft_conf  = draft_conf;
        info->flipped     = (best_score > draft_score) ? 1 : 0;
    }
    return best_n;
}

/* ================================================================== */
/* the compounding ring (§3.4) — search-distill, hard-gated             */
/* ================================================================== */
typedef struct {
    uint8_t bytes[DLB_TRACE_MAX];   /* query || answer                        */
    int     len;                    /* total bytes                            */
    int     qn;                     /* split point (answer starts at qn)      */
    int     verified;               /* 1 iff a V-exact checker accepted it     */
    int     rounds_done;            /* distill passes applied so far (budget)  */
} dlb_trace;

static dlb_trace g_ring[DLB_RING_MAX];
static int       g_ring_n = 0;

void dlb_compound_reset(void) { g_ring_n = 0; }

int dlb_compound_enqueue(const uint8_t *query, int qn,
                         const uint8_t *ans, int an, int verified)
{
    if (qn < 0) qn = 0;
    if (an < 0) an = 0;
    if (g_ring_n >= DLB_RING_MAX) return 0;
    if (qn + an > DLB_TRACE_MAX || qn + an < 1) return 0;
    dlb_trace *t = &g_ring[g_ring_n];
    int n = 0;
    for (int i = 0; i < qn; i++) t->bytes[n++] = query[i];
    for (int i = 0; i < an; i++) t->bytes[n++] = ans[i];
    t->len = n; t->qn = qn; t->verified = verified ? 1 : 0;
    t->rounds_done = 0;             /* fresh trace: full distill budget available */
    g_ring_n++;
    return 1;
}

int dlb_compound_pending(int require_verified)
{
    int c = 0;
    for (int i = 0; i < g_ring_n; i++)
        if (!require_verified || g_ring[i].verified) c++;
    return c;
}

int dlb_compound_distill(st_model *m, int rounds, float lr, int require_verified)
{
    if (!m || !m->w) return ST_E_ARG;
    if (rounds < 1) rounds = 1;

    /* one bounded logits arena, sized by the FIXED max trace length (no VLA). */
    float *logits = (float *)malloc((size_t)DLB_TRACE_MAX * ST_VOCAB * sizeof(float));
    if (!logits) return ST_E_OOM;

    int distinct = 0;
    for (int i = 0; i < g_ring_n; i++)
        if ((!require_verified || g_ring[i].verified) &&
            g_ring[i].rounds_done < DLB_TRACE_ROUNDS_MAX) distinct++;

    /* FIXED canonical order: rounds outer, ring ascending inner — one-math
     * deterministic, byte-identical to an all-at-once run (the DMN sleep law).
     * Skipping a budget-exhausted trace REMOVES an element without reordering the
     * rest, so the [0,MAX) distill sequence stays identical on every node. */
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < g_ring_n; i++) {
            dlb_trace *t = &g_ring[i];
            if (require_verified && !t->verified) continue;   /* the HARD GATE */
            if (t->rounds_done >= DLB_TRACE_ROUNDS_MAX) continue; /* budget spent */
            if (t->len < 2) continue;
            st_zero_grad(m);
            st_forward(m, t->bytes, t->len, logits);
            st_backward(m, t->bytes, t->len);
            st_adam_step(m, lr);
            t->rounds_done++;              /* consume one pass of the budget */
        }
    }
    free(logits);
    return distinct;
}

/* Reap spent/unusable traces so a live per-tick feeder cannot re-distill forever
 * or saturate the ring. Keeps ONLY verified traces with budget left; front-packs
 * survivors (relative order preserved -> canonical distill order preserved ->
 * one-math determinism intact). Does NOT reset the whole ring: freshly-enqueued
 * not-yet-distilled traces (rounds_done < MAX) survive. */
void dlb_compound_gc(void)
{
    int w = 0;
    for (int i = 0; i < g_ring_n; i++) {
        dlb_trace *t = &g_ring[i];
        if (t->rounds_done >= DLB_TRACE_ROUNDS_MAX) continue;  /* budget spent  */
        if (!t->verified) continue;                            /* never distills */
        if (w != i) g_ring[w] = *t;
        w++;
    }
    g_ring_n = w;
}
