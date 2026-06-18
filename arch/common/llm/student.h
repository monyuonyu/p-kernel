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
#define ST_TOPK     2
#define ST_DFF      256   /* per-expert SwiGLU hidden (= 2 * d_model)       */
#define ST_MAXSEQ   64    /* training/eval context cap (NS-1 fixture len)   */

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

/* libc-free math, exposed for the test's hand-checks / reuse. */
float st_expf(float x);
float st_logf(float x);
float st_rsqrtf(float x);

#endif /* PKERNEL_LLM_STUDENT_H */
