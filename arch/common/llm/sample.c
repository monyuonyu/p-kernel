/*
 *  sample.c — temperature / top-k / top-p (nucleus) sampler + repetition
 *             penalty + EOS stop for the SmolLM2 forward (forward.c).
 *
 *  Step ① of "give p-kernel a real LLM chat" (docs/architecture/
 *  inference-engine.md, conversation.md §3.7): lm_generate() in forward.c is
 *  GREEDY-only and never stops on EOS, which makes every reply deterministic
 *  and run-on. This file adds the missing piece — a real sampler with an
 *  explicit, seedable, libc-free RNG so the same (model, prompt, params, seed)
 *  reproduces byte-for-byte on every target (the wave-49 "one mind, one math"
 *  rule; built -O1 -ffp-contract=off with the rest of arch/common/llm/).
 *
 *  Scope / honesty (conversation.md §2): HOST / Android-side ("身体") code, the
 *  same tier as gguf.c / quant.c / forward.c. It uses malloc/free (once per
 *  call, freed before return — no per-token allocation) and the libc-free
 *  lm_expf() from forward.c. It pulls in NO transcendental libc math and NO
 *  libc rand(); the categorical draw is a self-contained xorshift64*.
 *
 *  Greedy is preserved exactly: temp <= 0 short-circuits to lm_argmax(), so
 *  lm_generate_sampled(..., temp=0, ...) === lm_generate() token-for-token.
 */
#include "forward.h"
#include <stdlib.h>     /* malloc / free — host/Android tier */

/* xorshift64* — a 64-bit PRNG with a full 2^64-1 period, no libc, no globals.
 * State threaded explicitly so the whole generation is reproducible. A zero
 * seed is mapped to a fixed nonzero state (xorshift dies at 0). */
static uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* uniform float in [0,1) from the top 24 bits (single-precision mantissa). */
static float rng_unit(uint64_t *s)
{
    return (float)(rng_next(s) >> 40) * (1.0f / 16777216.0f);  /* 2^-24 */
}

/* one candidate: (token id, working logit, derived probability). */
struct cand { int id; float logit; float prob; };

/* descending insertion-sort by logit over the first `keep` slots of a partial
 * selection. We only ever need the top-k by logit, and vocab is ~49k while k
 * is tiny, so a single O(vocab * k) partial selection beats a full sort. */
static int top_by_logit(const float *logits, int vocab, int k, struct cand *out)
{
    /* k<=0 means "all" — but the caller never asks for all here (top-p path
     * uses a full sort instead). This routine is the top-k cap path. */
    int n = 0;
    for (int v = 0; v < vocab; v++) {
        float lg = logits[v];
        if (n < k) {
            /* insert into the sorted (descending) prefix */
            int i = n++;
            while (i > 0 && out[i - 1].logit < lg) { out[i] = out[i - 1]; i--; }
            out[i].id = v; out[i].logit = lg; out[i].prob = 0.0f;
        } else if (lg > out[k - 1].logit) {
            /* displaces the current minimum of the kept set */
            int i = k - 1;
            while (i > 0 && out[i - 1].logit < lg) { out[i] = out[i - 1]; i--; }
            out[i].id = v; out[i].logit = lg; out[i].prob = 0.0f;
        }
    }
    return n;
}

/* full descending sort of all `n` candidates by logit (qsort comparator). */
static int cmp_logit_desc(const void *a, const void *b)
{
    float la = ((const struct cand *)a)->logit;
    float lb = ((const struct cand *)b)->logit;
    if (la < lb) return  1;
    if (la > lb) return -1;
    return 0;
}

/*
 * Pick the next token from m->logits using the running sequence `seq` (length
 * `nseq`, prompt+generated, for the repetition penalty). Returns the token id,
 * or a negative value never used as a token (caller treats <0 as "greedy").
 */
static int sample_next(lm_model *m, const int *seq, int nseq,
                       float temp, int top_k, float top_p, float rep_pen,
                       uint64_t *rng, struct cand *scratch)
{
    int vocab = m->vocab;
    float *logits = m->logits;

    /* (1) repetition penalty: touch each DISTINCT token already in `seq`.
     * llama.cpp convention: logit>0 -> /rep_pen, logit<0 -> *rep_pen. We scan
     * seq (short: prompt+gen) and apply once per occurrence-set; applying more
     * than once for a repeated id is harmless-but-stronger, so de-dup cheaply
     * by only applying when this is the first occurrence in seq. */
    if (rep_pen > 1.0f) {
        for (int i = 0; i < nseq; i++) {
            int t = seq[i];
            if (t < 0 || t >= vocab) continue;
            int first = 1;
            for (int j = 0; j < i; j++) { if (seq[j] == t) { first = 0; break; } }
            if (!first) continue;
            float lg = logits[t];
            logits[t] = (lg > 0.0f) ? (lg / rep_pen) : (lg * rep_pen);
        }
    }

    /* (2) build the candidate set, capped by top-k if requested. */
    int n;
    if (top_k > 0 && top_k < vocab) {
        n = top_by_logit(logits, vocab, top_k, scratch);
    } else {
        for (int v = 0; v < vocab; v++) {
            scratch[v].id = v; scratch[v].logit = logits[v]; scratch[v].prob = 0.0f;
        }
        n = vocab;
        qsort(scratch, (size_t)n, sizeof(scratch[0]), cmp_logit_desc);
    }

    /* (3) temperature + softmax over the kept candidates (max-subtracted for
     * stability; uses the libc-free lm_expf so math matches everywhere). */
    float maxl = scratch[0].logit;            /* candidates are sorted desc   */
    float inv_t = 1.0f / temp;
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        float p = lm_expf((scratch[i].logit - maxl) * inv_t);
        scratch[i].prob = p;
        sum += p;
    }
    /* normalize */
    float inv_sum = (sum > 0.0f) ? (1.0f / sum) : 0.0f;
    for (int i = 0; i < n; i++) scratch[i].prob *= inv_sum;

    /* (4) top-p (nucleus): keep the smallest prefix whose cumulative prob
     * >= top_p; renormalize over that prefix. scratch is already prob-desc
     * because it is logit-desc and softmax is monotone in the logit. */
    int keep = n;
    if (top_p > 0.0f && top_p < 1.0f) {
        float cum = 0.0f;
        keep = 0;
        for (int i = 0; i < n; i++) {
            cum += scratch[i].prob;
            keep++;
            if (cum >= top_p) break;
        }
        if (keep < 1) keep = 1;
        /* renormalize over the surviving nucleus */
        float s = 0.0f;
        for (int i = 0; i < keep; i++) s += scratch[i].prob;
        float inv = (s > 0.0f) ? (1.0f / s) : 0.0f;
        for (int i = 0; i < keep; i++) scratch[i].prob *= inv;
    }

    /* (5) categorical draw from the surviving mass. */
    float r = rng_unit(rng);
    float acc = 0.0f;
    for (int i = 0; i < keep; i++) {
        acc += scratch[i].prob;
        if (r < acc) return scratch[i].id;
    }
    return scratch[keep - 1].id;   /* fp slack — fall through to the last kept */
}

int lm_generate_sampled(lm_model *m, const int *in, int n_in,
                        int *out, int max_gen,
                        float temp, int top_k, float top_p, float rep_pen,
                        int eos_id, uint64_t seed)
{
    if (max_gen < 0) return LM_E_SHAPE;

    /* temp <= 0  ==>  exact greedy (bit-identical to lm_generate), but with
     * the EOS stop bolted on. The sampler knobs are ignored in this mode. */
    int greedy = (temp <= 0.0f);

    /* explicit, reproducible RNG state (xorshift dies at 0). */
    uint64_t rng = seed ? seed : 0x9E3779B97F4A7C15ULL;

    /* running sequence (prompt + generated) for the repetition penalty, plus
     * the per-call candidate scratch. Single allocation, freed before return —
     * no per-token malloc, matching forward.c's discipline. */
    int          *seq     = NULL;
    struct cand  *scratch = NULL;
    int seq_cap = n_in + max_gen + 1;
    if (seq_cap < 1) seq_cap = 1;
    seq = (int *)malloc((size_t)seq_cap * sizeof(int));
    if (!greedy) scratch = (struct cand *)malloc((size_t)m->vocab * sizeof(struct cand));
    if (!seq || (!greedy && !scratch)) { free(seq); free(scratch); return LM_E_OOM; }

    int nseq = 0;
    lm_reset(m);

    int last = 0;
    for (int i = 0; i < n_in; i++) {
        int rc = lm_forward(m, in[i]);
        if (rc != 0) { free(seq); free(scratch); return rc; }
        seq[nseq++] = in[i];
        last = lm_argmax(m);   /* logits after the last prompt token */
    }

    int produced = 0;
    for (int g = 0; g < max_gen; g++) {
        int tok;
        if (greedy) {
            tok = lm_argmax(m);
        } else {
            tok = sample_next(m, seq, nseq, temp, top_k, top_p, rep_pen,
                              &rng, scratch);
        }
        if (eos_id >= 0 && tok == eos_id) break;   /* stop; EOS not emitted */
        out[produced++] = tok;
        seq[nseq++] = tok;
        int rc = lm_forward(m, tok);
        if (rc != 0) { free(seq); free(scratch); return rc; }
        last = lm_argmax(m);
        (void)last;
    }

    free(seq);
    free(scratch);
    return produced;
}
