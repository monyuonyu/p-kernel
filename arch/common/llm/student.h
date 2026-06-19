/*
 *  student.h — the Cradle baby: a from-scratch, organism-native MoE student.
 *
 *  NS-1 (docs/architecture/native-student.md §A.2/§B.6, the FIRST HEARTBEAT):
 *  a THIRD network — NOT R3's rw[] (key->value single-token classifier), NOT
 *  the dtr sensor body (4ch->3class) — but a white-paper baby that LEARNS to
 *  predict the next RAW BYTE from a teacher (SmolLM2-135M, run via forward.c).
 *
 *  Decided architecture (native-student.md §A.2, the "born small" baby):
 *    - vocab     = 256 RAW BYTES          (mergeless tokenizer, no OOV; §A.5)
 *    - E         = 4 experts, top-k = 2   (MoE sparse firing; §A.1/§A.2)
 *    - d_model   = 128
 *    - n_layers  = 4
 *  Per layer: RMSNorm -> causal self-attention (residual) -> RMSNorm -> MoE
 *  SwiGLU FFN (residual). Final RMSNorm -> output projection -> 256 logits.
 *
 *  Honesty / scope:
 *    - Single node. NO distribution (NS-2), NO vocab growth, NO weight
 *      diffusion / merge (NS-2+). ONE frontier: does the baby LEARN.
 *    - It has its OWN libc-free forward AND its OWN libc-free, GRAD-CHECKED
 *      backward (it does NOT call dtr_train_batch — LM-4 V.0 rule: the slow
 *      layer is the baby's OWN weights). reused only the *math style*.
 *    - libc-light: <stdint.h>/<stddef.h> + a single host malloc/free for the
 *      weight/scratch arena; ALL transcendental math is self-contained here
 *      (one-math rule, wave-49: build -O1 -ffp-contract=off).
 *    - Attention is SINGLE-HEAD (head_dim == d_model). The doc lists heads as a
 *      later growth lever; NS-1 keeps one head so the analytic backward stays
 *      grad-checkable end-to-end. Honestly noted, not hidden.
 */
#ifndef PKERNEL_LLM_STUDENT_H
#define PKERNEL_LLM_STUDENT_H

/* NS-1 carries its own version (modver registry; compatibility.md): the
 * Cradle baby (organism-native MoE student) contract. v1 = the born-small
 * baby (256-byte vocab, E=4 top-2, d=128, 4 layers) with its own libc-free
 * grad-checked forward+backward. */
#define NS_STUDENT_VER  1

#include <stdint.h>
#include <stddef.h>

/* ---- the baby's decided dimensions (native-student.md §A.2) ---- */
#define ST_VOCAB    256   /* raw bytes — mergeless, OOV-free                */
#define ST_DMODEL   128
#define ST_NLAYER   4
#define ST_NEXPERT  4
#define ST_TOPK     2     /* K_min — the floor firing width (legacy fixed K) */
#define ST_DFF      256   /* per-expert SwiGLU hidden (= 2 * d_model)       */
#define ST_MAXSEQ   64    /* training/eval context cap (NS-1 fixture len)   */

/* ---- adaptive top-K (SS-1, special-structure-mind.md §4) ----
 * Firing width is now a DETERMINISTIC function of (weights, bytes): an easy
 * token (one expert dominates the router gate -> big margin) fires K_min
 * experts; a hard/ambiguous token (flat gate -> small margin) widens toward
 * K_MAX = E. The hardness signal is the ROUTER MARGIN gate[top1]-gate[topj]
 * (the cheapest order-stable signal under -ffp-contract=off — no new
 * transcendental, so (weights,bytes) -> identical K on every target).
 *
 * [no-vla] DISCIPLINE: K is now a RUNTIME value, so every scratch array that
 * was sized by K is bound to the FIXED maximum ST_KMAX (= E). Never a VLA.   */
#define ST_KMIN     ST_TOPK     /* floor: always fire at least this many      */
#define ST_KMAX     ST_NEXPERT  /* ceiling: at most all E experts (bounded)   */
/* Margin threshold (in router-logit nats). While the gap from top1 to the
 * next candidate is BELOW this, the token is "ambiguous" and we admit that
 * expert. Larger THETA -> more tokens widen. A compile-time const so the
 * widening is one-math deterministic across targets (wave-49).
 *
 * 0.30 chosen by a THETA sweep on the NS-1 teacher fixture (run_ss1.sh): it
 * widens a measurable amount on hard tokens (mean firing width ~2.1, ambiguous
 * tokens -> up to K_MAX) WITHOUT regressing held-out loss vs fixed K=2 — the
 * [no-loss-regression] gate. Aggressive THETA (>=0.5) over-widens the tiny
 * baby during early training and DOES regress (honest: the win is small until
 * the baby is bigger; the MECHANISM is what ships here). A fixed-K=2 build is
 * recovered with -DST_K_THETA=0.0f (the router then never widens). */
#ifndef ST_K_THETA
#define ST_K_THETA  0.30f
#endif

/* Return codes. */
#define ST_OK      0
#define ST_E_OOM (-1)
#define ST_E_ARG (-2)

/*
 *  The baby. All weights live in one contiguous arena (`w`) so the grad-check
 *  can perturb any scalar by index and the optimizer can step uniformly. The
 *  matching gradient arena (`g`) and Adam moments (`mu`,`vu`) are parallel.
 */
typedef struct {
    int   n_params;     /* total trainable scalars                          */
    float *w;           /* [n_params]  weights                              */
    float *g;           /* [n_params]  gradient accumulator                 */
    float *mu;          /* [n_params]  Adam 1st moment                      */
    float *vu;          /* [n_params]  Adam 2nd moment                      */
    int    adam_t;      /* Adam timestep                                    */

    /* offsets into w[] for each named weight family (set by st_init). */
    int o_embed;        /* [VOCAB][D]                                       */
    int o_attn_norm;    /* [L][D]                                           */
    int o_wq, o_wk, o_wv, o_wo;   /* each [L][D][D]                         */
    int o_ffn_norm;     /* [L][D]                                           */
    int o_router;       /* [L][E][D]                                        */
    int o_w1, o_w3;     /* each [L][E][DFF][D]  (SwiGLU gate/up)            */
    int o_w2;           /* [L][E][D][DFF]       (SwiGLU down)               */
    int o_out_norm;     /* [D]                                              */
    int o_out;          /* [VOCAB][D]  (untied output head)                 */

    /* forward activation cache (sized for MAXSEQ; reused per backward). */
    void *cache;        /* opaque st_cache *                                */
    void *mem;          /* the single malloc'd arena base (st_free frees)   */
} st_model;

/* Allocate + LCG-seed all weights (small-random embed/out, He-ish linears,
 * RMSNorm gains = 1). 0 on success. */
int  st_init(st_model *m, uint32_t seed);
void st_free(st_model *m);

/* Forward a sequence of `n` raw bytes (each 0..255). Writes per-position
 * next-byte logits into logits[n*VOCAB] (row t = logits after byte t).
 * Caches activations for a subsequent st_backward. Returns 0/negative. */
int  st_forward(st_model *m, const uint8_t *bytes, int n, float *logits);

/* Mean next-byte cross-entropy loss over a sequence (predict byte t+1 from
 * prefix 0..t). Pure forward, NO cache mutation needed for eval. Returns loss
 * in nats; writes the per-call token count to *n_pred if non-NULL. */
float st_eval_loss(st_model *m, const uint8_t *bytes, int n, int *n_pred);

/* ---- generation (step ⑥, the chat mouth) ----
 * Feed `prompt` (n_prompt raw bytes) as context, then autoregressively sample
 * up to max_gen next bytes into out[] (byte-level, 256 vocab, OOV-free). temp
 * + top_k sampling with a seeded, libc-free xorshift RNG (reproducible; built
 * -O1 -ffp-contract=off). temp<=0 is greedy. The running context is CAPPED at
 * ST_MAXSEQ and max_gen is clamped to an internal ~96-byte cap (bounded/fast).
 * `out` must have room for max_gen bytes. Returns the count produced, or a
 * negative ST_E_* on bad args / OOM. */
int  st_generate(st_model *m, const uint8_t *prompt, int n_prompt,
                 uint8_t *out, int max_gen, float temp, int top_k, uint64_t seed);

/* Streaming variant: identical sampling, but each produced byte is also handed
 * to emit(ctx, byte) AS IT IS GENERATED, so the caller can flush it
 * progressively (the slow baby's reply appears as it thinks). `out` still
 * receives the full sequence. emit may be NULL (== st_generate). */
int  st_generate_stream(st_model *m, const uint8_t *prompt, int n_prompt,
                        uint8_t *out, int max_gen, float temp, int top_k,
                        uint64_t seed, void (*emit)(void *, int), void *ctx);

/* Accumulate the gradient of the mean next-byte cross-entropy of `bytes`
 * (length n) into m->g (which the caller zeroed). Must follow an st_forward on
 * the SAME bytes. Returns the loss it differentiated. */
float st_backward(st_model *m, const uint8_t *bytes, int n);

/* One Adam step using the accumulated m->g, then zero m->g. */
void st_adam_step(st_model *m, float lr);

/* Zero the gradient accumulator. */
void st_zero_grad(st_model *m);

/* Grad-check: analytic gradient (one forward+backward on `bytes`) vs central
 * finite differences over a strided spread of every weight family. Returns the
 * max relative error. (`stride` picks the spread; eps the FD step.) */
float st_grad_check(st_model *m, const uint8_t *bytes, int n, int stride, float eps);

/* ---- persistence (pure serialization; file IO lives a tier up) ---- */

/* Exact byte size of a saved student for THIS build (header + w + Adam
 * moments + timestep). Use to size the durable buffer. */
size_t st_blob_size(const st_model *m);

/* Serialize the trainable state (w + mu + vu + adam_t) into buf[cap].
 * Returns bytes written, or negative (ST_E_ARG) when cap is too small. */
long st_save(const st_model *m, void *buf, size_t cap);

/* Load a saved blob into m (already st_init'd to the SAME build — st_load
 * reuses its arena). Verifies magic/version/dims; refuses any mismatch.
 * Returns ST_OK on success, negative on reject. */
int  st_load(st_model *m, const void *buf, size_t len);

/* ---- firing-width observability (SS-1) ----
 * Read-only: the number of experts the LAST st_forward fired on its FINAL
 * (most recent) token, final layer — the "answer token" firing width. This is
 * the amoeba organ-ring's usage signal (heavy task -> wider region) that the
 * UI/observability will later read. NOT wired to galaxy in this wave. Returns
 * 0 before any forward. Deterministic in (weights, bytes). */
int  st_last_fire_width(void);
/* Mean firing width across ALL (layer, token) experts of the last st_forward,
 * scaled by 1000 (integer, libc-free): e.g. 2500 == mean 2.5 experts. Lets the
 * observability layer see the whole forward's sparsity, not just one token. */
int  st_last_fire_width_mean_milli(void);
/* Copy the chosen expert ids of the LAST forward's final-token final layer into
 * out[max] (the "answer token"); returns the count written (== firing width,
 * <= max). Lets a cert assert the SAME bytes select the IDENTICAL experts. */
int  st_last_fire_experts(int *out, int max);

/* TEST HOOK (SS-1): run the REAL adaptive router-margin selection on a
 * caller-supplied gate-logit vector of length ST_NEXPERT. Writes the chosen
 * expert ids into chosen[ST_NEXPERT] (first nk valid) and returns the firing
 * width nk (ST_TOPK..ST_NEXPERT). Lets a cert exercise the margin-widening on
 * a CRAFTED flat vs peaked gate without depending on training luck. This calls
 * the exact production selection (no logic duplicated). */
int  st_router_pick_width(const float *gate, int *chosen);

/* libc-free math, exposed for the test's hand-checks / reuse. */
float st_expf(float x);
float st_logf(float x);
float st_rsqrtf(float x);

#endif /* PKERNEL_LLM_STUDENT_H */
