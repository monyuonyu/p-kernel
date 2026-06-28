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

/* ---- the baby's decided dimensions (native-student.md §A.2) ----
 *
 * SS-2 (special-structure-mind.md §3.2): the four model dims D/DFF/L/E are now
 * TIER-SELECTABLE at runtime (S/M/L) instead of compile-time fixed. They live
 * in a const tier table (student.c) and are copied into st_model by st_init
 * from a tier byte. The DEFAULT tier is M, whose values are EXACTLY the legacy
 * NS-1/SS-1 dims below — so the M-tier model is BYTE-IDENTICAL to the pre-SS-2
 * baby ([m-identical] cert). V (byte vocab) and K_min stay FIXED across tiers.
 *
 * ST_DMODEL/ST_NLAYER/ST_NEXPERT/ST_DFF are KEPT as the M-tier (== legacy)
 * values so existing callers, the persistence header, and the SS-1 KMAX wiring
 * read the same numbers as before. The L-tier (max) values are ST_*_MAX, which
 * bound EVERY stack scratch array (the [no-vla] gate, §3.2): a scratch array is
 * NEVER sized by the runtime m->* value, always by the fixed ST_*_MAX.         */
#define ST_VOCAB    256   /* raw bytes — mergeless, OOV-free (FIXED all tiers) */
#define ST_DMODEL   128   /* == ST_D_M : the default (M) tier d_model           */
#define ST_NLAYER   4     /* == ST_L_M : the default (M) tier layer count       */
#define ST_NEXPERT  4     /* == ST_E_M : the default (M) tier expert count      */
#define ST_TOPK     2     /* K_min — the floor firing width (FIXED all tiers)   */
#define ST_DFF      256   /* == ST_DFF_M : default (M) per-expert SwiGLU hidden */
#define ST_MAXSEQ   64    /* training/eval context cap (FIXED all tiers)        */

/* ---- the three tiers (SS-2, §3.2) ----
 * Discrete {D, DFF, L, E} dim-sets selected at st_init from a tier byte. NOT
 * arbitrary runtime dims (that re-opens VLAs + makes every node pair a merge-
 * island). M is EXACTLY today's baby. S is smaller (weak device), L larger
 * (strong device). K_min=ST_TOPK and V=ST_VOCAB are tier-invariant. The merge
 * cohort is the tier (SS-3): S averages with S, L with L — never across tiers. */
#define ST_TIER_S   0
#define ST_TIER_M   1     /* DEFAULT — byte-identical to the pre-SS-2 student   */
#define ST_TIER_L   2
#define ST_TIER_DEFAULT  ST_TIER_M
#define ST_NTIER    3

/* S tier — small (weak device): half the width, fewer experts/layers. */
#define ST_D_S      64
#define ST_DFF_S    128
#define ST_L_S      2
#define ST_E_S      2

/* M tier — EXACTLY today's dims (the running model; do NOT change). */
#define ST_D_M      ST_DMODEL   /* 128 */
#define ST_DFF_M    ST_DFF      /* 256 */
#define ST_L_M      ST_NLAYER   /* 4   */
#define ST_E_M      ST_NEXPERT  /* 4   */

/* L tier — large (strong device): wider, deeper, more experts. */
#define ST_D_L      256
#define ST_DFF_L    512
#define ST_L_L      6
#define ST_E_L      8

/* The MAX (== L-tier) values. EVERY stack scratch array sized by a dim binds to
 * these fixed constants ([no-vla]); the malloc'd arena/cache may size by the
 * runtime m->* (heap, bounded). _Static_assert (student.c) pins MAX == L tier. */
#define ST_D_MAX    ST_D_L
#define ST_DFF_MAX  ST_DFF_L
#define ST_L_MAX    ST_L_L
#define ST_E_MAX    ST_E_L

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
#define ST_KMIN     ST_TOPK     /* floor: always fire at least this many        */
/* SS-2: the FIXED scratch ceiling is the L-tier expert count (ST_E_MAX), so the
 * per-token K scratch is bound to a compile-time constant across ALL tiers (no
 * VLA). The RUNTIME widening ceiling is the model's own m->nexpert (<= ST_KMAX),
 * applied in router_pick — an easy token fires K_min, a hard one widens toward
 * the resident model's expert count, never past ST_KMAX.                       */
#define ST_KMAX     ST_E_MAX    /* ceiling: at most all L-tier experts (bounded) */
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

    /* ---- SS-2 runtime dims (set by st_init from a tier byte) ----
     * The forward/backward/router/adam loops + the o_* offsets read THESE
     * (runtime), never the legacy #defines. Every stack scratch array stays
     * bound to ST_*_MAX (fixed) so these runtime dims never produce a VLA.
     * tier is one of ST_TIER_S/M/L; M (default) reproduces the legacy dims
     * exactly (the [m-identical] cert). d<=ST_D_MAX, dff<=ST_DFF_MAX,
     * nlayer<=ST_L_MAX, nexpert<=ST_E_MAX (st_init enforces / clamps).        */
    int    tier;        /* ST_TIER_S / _M / _L                              */
    int    d;           /* d_model   (== ST_D_<tier>)                       */
    int    dff;         /* expert SwiGLU hidden (== ST_DFF_<tier>)          */
    int    nlayer;      /* layer count (== ST_L_<tier>)                     */
    int    nexpert;     /* expert count E (== ST_E_<tier>; K_MAX ceiling)   */

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

    /* ---- SS-4 function-preserving expert growth (ss4-function-preserving-
     * growth-plan.md §1.3) ----
     * Per-expert liveness mask, nexpert-sized, separate heap block (NOT in the
     * weight arena — it is not a trainable scalar). 1 = a real, selectable
     * expert; 0 = a DEAD (grown-but-not-resurrected) expert whose effective
     * router logit is forced to -inf in router_pick so it is PROVABLY never in
     * the chosen top-K set, for EVERY input. The DEAD expert's router row + W2
     * are also zeroed (defense-in-depth), but the alive mask is THE load-bearing
     * exactness mechanism (a 0 logit alone can still be admitted by the
     * margin-widening router). When `alive` is NULL (the default for every
     * pre-SS-4 / never-grown model) router selection is BYTE-IDENTICAL to the
     * pre-SS-4 path (the all-alive case) — the [grow-noop-identity] gate. */
    int8_t *alive;      /* [nexpert] liveness mask, or NULL == all-alive     */

    /* forward activation cache (sized for MAXSEQ; reused per backward). */
    void *cache;        /* opaque st_cache *                                */
    void *mem;          /* the single malloc'd arena base (st_free frees)   */
} st_model;

/* Allocate + LCG-seed all weights (small-random embed/out, He-ish linears,
 * RMSNorm gains = 1). 0 on success. This is the DEFAULT-tier (M) entry point:
 * st_init(m,seed) == st_init_tier(m,seed,ST_TIER_DEFAULT), so every existing
 * caller (boot/birth/chat) keeps the byte-identical M baby — 0.9.x behaviour is
 * unchanged. */
int  st_init(st_model *m, uint32_t seed);

/* SS-2: tier-selectable init. `tier` is ST_TIER_S / _M / _L; an out-of-range
 * tier clamps to ST_TIER_DEFAULT (fail-safe, never undersizes the arena). The
 * model's d/dff/nlayer/nexpert are set from the const tier table; n_params and
 * the o_* offsets are computed from THOSE runtime dims. The same LCG seed +
 * same tier reproduces the same weights deterministically. Returns 0 / ST_E_*. */
int  st_init_tier(st_model *m, uint32_t seed, int tier);
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

/* ---- SS-3 same-tier merge cohorts (special-structure-mind.md §3.2, §8.4) ----
 *
 * The student is NEVER folded into R3's fleet crown (gl_merge of rw[]); it has
 * its OWN, isolated, student-only weight merge so heterogeneous tiers can never
 * threaten the R3 collective ([baby-merge-isolation]). The merge cohort is the
 * TIER: S averages with S, M with M, L with L — a cross-tier blob is REFUSED by
 * construction (an honest island; the only cross-tier bridge is distillation,
 * out of scope here).
 *
 * st_blob_tier_ok: return 1 iff `buf` is a well-formed student blob whose
 * tier/dims/n_params/vocab EXACTLY match the resident model `m` (the SAME fail-
 * closed predicate st_load uses, factored out so the merge can REFUSE a peer
 * blob WITHOUT mutating m). 0 (refused) on any mismatch/short buffer. Pure
 * read; does not touch m. */
int  st_blob_tier_ok(const st_model *m, const void *buf, size_t len);

/* st_merge_cohort: average the WEIGHTS (w[]) of `into` with `count` SAME-TIER
 * peer student blobs, folding the plain (uniform) mean back into into->w[]. The
 * resident model `into` is itself a cohort member: the mean is over {into} U the
 * accepted peers, so a single-peer merge is the 2-way average (into + peer)/2.
 *
 * HONESTY (Path-W, memory moment_2026_06_12_wave41_one_mind): naive weight-
 * averaging is the MECHANISM, and it CONVERGES two minds toward a SHARED /
 * compatible objective. It is LOSSY for two minds that learned DIFFERENT facts
 * (one fact survives, one decays toward chance) — union-replay / Fisher recovery
 * of divergent facts is Path-W² and OUT OF SCOPE here. This merges weights, not
 * memories.
 *
 * Determinism / one-math (wave-49): the reduction is a fixed CANONICAL order
 * (into first, then peers in ASCENDING index), a plain sum then a single divide
 * by the accepted count — no reassociated sums, no new transcendental, no libc
 * math. So merge(A,B) is byte-identical to merge(B,A) up to the accepted-set
 * being the same set, and the result is identical across arches under
 * -O1 -ffp-contract=off.
 *
 * Tier guard (islands by construction): each peer blob is checked with
 * st_blob_tier_ok BEFORE it is summed; a peer of a DIFFERENT tier/shape is
 * SILENTLY SKIPPED (not coerced) — never mixed in. If NO peer is accepted the
 * model is left BYTE-UNCHANGED (the [ss3-cohort-island] guarantee). The Adam
 * moments mu[]/vu[] are RESET to 0 and adam_t to 0 after a successful merge
 * (averaging foreign optimizer state is meaningless; the merged model resumes
 * Adam fresh — the standard FedAvg choice, honestly noted).
 *
 * peer_blobs[i] / peer_lens[i] describe peer i's serialized st_save() blob.
 * It MUST NOT call gl_merge/gl_merge_w and MUST NOT touch R3's rw[]
 * (the [baby-merge-isolation] tripwire). Returns the number of peers ACCEPTED
 * (0..count), or negative ST_E_ARG on bad args. */
int  st_merge_cohort(st_model *into,
                     const void *const *peer_blobs, const size_t *peer_lens,
                     int count);

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

/* ---- SS-4 function-preserving expert growth (ss4-function-preserving-growth-
 * plan.md §1) ----
 *
 * Grow the resident student's expert count m->nexpert from its current value up
 * to `e_new` (e_new in [m->nexpert, ST_E_MAX]) by ADDING DEAD experts. This is
 * the EXACTLY function-preserving growth event (§1.3, summary verdict): the
 * output of st_forward is BYTE-IDENTICAL for EVERY input after the grow, ε = 0.
 *
 * Mechanism (each new expert e' in [E_old, e_new), every layer):
 *   - router row  := 0      (gate logit gt[e'] = Σ 0·fin = exact 0 for all in)
 *   - W2 (down)   := 0      (eo[e'] = Σ 0·eh = exact 0, regardless of W1/W3)
 *   - alive[e']   := 0      (router_pick forces its effective logit to -inf, so
 *                            it is PROVABLY never in the chosen top-K set ⇒ nk
 *                            and every other tw[j] are bit-unchanged) — THIS is
 *                            the exactness mechanism; the two zeroings are
 *                            redundant defense-in-depth.
 *   - W1/W3       := clone of the busiest incumbent (a latent warm-start for a
 *                    later, SEPARATE, deliberately-ε resurrection step; it
 *                    contributes NOTHING while W2=0 + alive=0).
 *
 * The growth is a RUNTIME heap event: it reshards the malloc'd weight/Adam arena
 * to the new strides (incumbent blocks copied verbatim; new slots DEAD-init),
 * sets m->nexpert/n_params, and reallocates the nexpert-sized alive[] mask. It
 * does NOT touch the static R3 rw[] crown (a different network / TU; §5) and
 * NEVER introduces a runtime-sized stack array (all scratch stays bound to
 * ST_*_MAX; §3.2). Incumbent Adam moments are RETAINED at their new strides; the
 * new slots' moments start at 0 (FedAvg-style, §1.1 step 4).
 *
 * e_new == m->nexpert is a legal no-op (the [grow-noop-identity] gate). Returns
 * ST_OK, or ST_E_ARG (e_new < nexpert or > ST_E_MAX), or ST_E_OOM. A grown model
 * forms a DISTINCT (tier, nexpert) merge-cohort by construction — st_blob_tier_ok
 * already refuses a blob whose n_expert differs ([grow-cohort], §3.2/§4). */
int  st_grow_experts(st_model *m, int e_new);

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

/* ---------------------------------------------------------------------------
 * Thread T — the lesson BRIDGE corpus seam (T-fix-b / T-1; cradle.c).
 *
 * A TEACHER node delivers a TEXT lesson over the mesh; the STUDENT pulls it and
 * trains its baby on it (the g_lesson_ring corpus source -> the real distill
 * path). cradle.c owns the ring + the ingest + the in-process [cradle-teach]
 * cert; student_shell.c's window() reads cradle_window_src() FIRST (the live
 * lesson) and its static TEACHER_FIXTURE second — a node with NO lesson trains
 * byte-identically to today. The shared geometry constants live HERE so cradle.c
 * and student_shell.c compute the SAME train/held split the cert relies on. */
#define CRADLE_RING_BYTES  6144   /* lesson ring cap (>= 4096 body + headroom)  */
#define CRADLE_SEQLEN      32     /* == ST_DMN_SEQLEN (the production tick win)  */

/* The corpus-source switch: returns the live lesson ring (and its length in
 * *len_out) when populated past the live threshold, else NULL (the caller reads
 * its TEACHER_FIXTURE). Gated by cradle_set_enabled (Arm A). Pure read. */
const uint8_t *cradle_window_src(int *len_out);

/* Install a fetched lesson body into the ring (length-prefixed binary; refuses
 * empty/too-big/too-small without truncating). Returns bytes installed or -1.
 * Called by the transport (cradle_net.c) after a p-fs pull. */
int  cradle_lesson_ingest(const uint8_t *body, int len);

/* Freeze the lesson ring for the duration of a sliced DMN consolidation batch
 * (cooperative-yield-plan.md §3.3): while frozen, cradle_lesson_ingest DEFERS
 * (returns 0, ring unchanged) so the corpus window() trains on is byte-stable
 * for the whole batch. The student drives this around its batch; default is
 * unfrozen, so any caller that never touches it is unaffected. */
void cradle_lesson_freeze(int on);

/* The CANONICAL live lesson (T-fix-c): unify live == cert. cradle_canon_budget()
 * returns the cert's CT_CERT_BUDGET (== the composed length). cradle_compose_canon
 * composes into out[cap] (cap >= cradle_canon_budget()) the SAME trainable, train/
 * held-structured lesson the [cradle-teach] cert proves (via the static
 * ct_build_lesson) — byte-identical to the in-proc cert's lesson, so the live wire
 * teaches the EXACT lesson the cert certifies. Returns the composed length
 * (== CT_CERT_BUDGET) or -1 (NULL out / cap too small); *probe_off (if non-NULL)
 * gets the held-probe offset (== the production train_end). The live teacher verb
 * emits cradle_compose_canon() bytes instead of a short hand-typed string. */
int  cradle_canon_budget(void);
int  cradle_compose_canon(uint8_t *out, int cap, int *probe_off);

/* observability / test hooks (pure). */
int  cradle_lesson_len(void);
void cradle_set_enabled(int on);    /* 0 = teaching OFF (Arm A): fixture only  */
int  cradle_get_enabled(void);
void cradle_lesson_clear(void);

/* The [cradle-teach] cert (thread-t-impl-plan.md §1): an in-process 2-node demo
 * where A teaches a lesson, B pulls + trains, and B then KNOWS a held-out probe
 * it was NEVER directly trained on (generalization, weight-resident). Three
 * falsification arms (teaching-off / scrambled / never-taught) each go RED.
 * `emit` is a line-printer (may be NULL). Returns 0 on PASS, else fail count. */
int  cradle_teach_self_test(void (*emit)(const char *));

/* [cradle-live] self-report (the multi-process teacher-convergence harness, the
 * T-fix-b DEFERRED [live] row): prints ONE uniquely-greppable observability line
 *   [cradle-live] ring_len=<n> probe_loss=<L> chance=<C>
 * computed off the LIVE corpus source at the PRODUCTION held boundary (reuses
 * cradle_window_src / cradle_corpus_len / heldout_loss — NO new math). Pure read
 * (no training, no save). Hosted-only (lives in student_shell.c). emit prints. */
void cradle_live_probe(void (*emit)(const char *));

/* ---- KV cache (wave-kv-cache): incremental generation ----
 * st_generate caches per-layer per-position K/V across generation steps so a
 * NEW token computes only its OWN position's q/k/v and attends over the CACHED
 * k/v of prior positions (O(1) new position vs O(nctx) recompute). This is the
 * st_generate path ONLY; st_forward / st_backward / training are UNCHANGED.
 *
 * The cached generation is BYTE-IDENTICAL to the no-cache recompute (same
 * logits, same sampled bytes, same FNV hash, both arches, -O1 -ffp-contract=
 * off): caching changes WHAT is recomputed, never the reduction/rounding order
 * of any single position's attention sum. The cache is bounded to ST_MAXSEQ
 * positions sized at the MAX (L) tier dims (heap, no VLA, no growth).
 *
 * These are TEST/observability hooks (NOT a generation-API change — st_generate
 * keeps its exact signature). st_kv_set_enabled flips the cached vs recompute
 * path so a cert can run BOTH and assert byte-identical; the per-step logit FNV
 * proves the LOGITS (not just the sampled bytes) match. Caching is ON by
 * default (the point: chat speed). */
void     st_kv_set_enabled(int on);     /* 1 = KV cache (default), 0 = recompute */
int      st_kv_get_enabled(void);
void     st_gen_logit_hash_reset(void); /* reset the per-step logit FNV-1a       */
uint64_t st_gen_logit_hash(void);       /* FNV-1a over every step's sampled row   */

/* ---- SS-6: cross-node expert firing with local fallback ----
 * special-structure-mind.md §5 + §8 item 7. The F4 capstone for the
 * conversational mind: "複数ノードをまたぐ一回の forward".
 *
 * The student is MoE. When the SS-1 adaptive router WIDENS K beyond K_min on
 * a HARD token, the EXTRA experts (slots j >= K_min) that SS-5 placement says
 * live on a PEER node may be computed REMOTELY — the peer runs that expert's
 * SwiGLU on the [D] input vector (tiny on the wire, ~D floats) and returns its
 * [D] output, which is summed into moe[] EXACTLY as the single-node forward
 * would (CRITIQUE GATE #3: a FIXED canonical reduction order; remote == single-
 * node BIT-FOR-BIT). The local K_min experts ALWAYS run locally.
 *
 * SURVIVAL CONTRACT (mirror of dtk_infer / dkva): a remote expert fires ONLY
 * when (a) the router widened (hard token) AND (b) a peer hosts that expert
 * (SS-5) AND (c) FULL-degrade with >= 2 region members. Each remote call has a
 * hard timeout; on timeout / absent peer the expert is RECOMPUTED LOCALLY
 * (never stall a token — lose the WIDTH, not correctness). The forward then
 * reports an honest `degraded(k/n)` width.
 *
 * The transport is provided by the CALLER via a callback (the kernel wires it
 * to a DRPC remote-expert call; the in-process cert wires it to a second
 * st_model / a stub). student.c owns the GATING, the canonical SUM, and the
 * fallback; it does NOT invent a new transport. */

/* Compute ONE expert's [D] SwiGLU output for a peer. Inputs: the layer index,
 * the global expert id (== chosen expert id in [0,nexpert)), the f_in[D]
 * rmsnorm'd input vector, and the tier dims d/dff. On SUCCESS, fill out[d] with
 * the expert's down-projection output (the SAME math student.c computes
 * locally) and return 0. On timeout / absent / refused, return < 0 (student.c
 * then recomputes that expert LOCALLY). `ctx` is the caller's opaque handle. */
typedef int (*st_remote_expert_fn)(int layer, int expert_id,
                                   const float *fin, int d, int dff,
                                   float *out, void *ctx);

/* Decide whether chosen-expert slot j (the j-th fired expert this token, with
 * expert id `expert_id`) is ELIGIBLE to fire remotely. The kernel's predicate
 * is: j >= K_min AND !st_expert_is_local(expert_id) AND degrade==FULL AND
 * region members >= 2. The cert supplies an explicit predicate so it can drive
 * BOTH the equiv path (remote succeeds) and the fallback path (remote refuses).
 * Return non-zero to attempt the remote call, 0 to keep it local. */
typedef int (*st_remote_gate_fn)(int layer, int j, int kmin, int expert_id,
                                  void *ctx);

/* Install the remote-expert transport + gating predicate (NULL clears, the
 * single-node default). When EITHER is NULL, st_forward is byte-IDENTICAL to
 * the pre-SS-6 single-node forward (every expert local). Set ONLY around the
 * generation path; training/backward MUST run with the hook clear (the remote
 * path does not cache e_g/e_u/e_h for the backward). */
void st_set_remote_expert(st_remote_expert_fn fn, st_remote_gate_fn gate,
                          void *ctx);

/* SS-6 observability (read-only, set by the LAST st_forward):
 *  - st_last_remote_fired(): how many expert outputs this forward took from a
 *    REMOTE peer (0 on the single-node path).
 *  - st_last_remote_fallback(): how many remote attempts TIMED OUT / were
 *    refused and were recomputed locally (the honest degraded count).
 * Both are deterministic in (weights, bytes, gate). */
int st_last_remote_fired(void);
int st_last_remote_fallback(void);

/* The EXACT per-expert SwiGLU a peer must run for a remote expert (special-
 * structure-mind.md §5): out[d] = w2_e . (silu(w1_e.fin) * (w3_e.fin)),
 * UNWEIGHTED (the router weight is applied by st_forward's canonical sum). The
 * kernel's DRPC remote-expert handler and any in-process stub call THIS so the
 * remote [D] output is bit-identical to the local MoE branch — that bit-
 * identity is what makes a remote forward == a single-node forward. Returns
 * ST_OK or ST_E_ARG on bad args. No model mutation, no cache writes. */
int st_expert_forward_ref(const st_model *m, int layer, int expert_id,
                          const float *fin, float *out);

#endif /* PKERNEL_LLM_STUDENT_H */
