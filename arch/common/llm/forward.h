/*
 *  forward.h — SmolLM2-135M (plain Llama) forward pass + greedy generation.
 *
 *  Milestone M1c (docs/architecture/inference-engine.md §1, §5):
 *  given a GGUF model already opened by M1a (gguf.c) and the M1b
 *  dequantize-while-you-matmul kernels (quant.c), run a full Llama forward and
 *  produce next-token logits, then greedy-argmax to generate tokens. This is
 *  "running the teacher" (conversation.md §3.7): the thing a volunteer uses to
 *  harvest soft targets to distill into the Cradle baby.
 *
 *  Scope / honesty (conversation.md §2): host / Android-side ("身体") code, the
 *  same tier as gguf.c / quant.c — it uses malloc (one alloc at lm_load, freed
 *  at lm_free; the per-step scratch lives in the model struct, no per-token
 *  malloc). It is libc-light: <stdint.h>/<stddef.h> + malloc/free only; ALL
 *  transcendental math (exp for softmax/SiLU, the rsqrt for RMSNorm, sin/cos
 *  for RoPE) is libc-free and self-contained in forward.c so "one math
 *  everywhere" holds (wave-49: build -O1 -ffp-contract=off).
 *
 *  NOTHING is hardcoded: every dimension (n_layer, d_model, n_head, n_kv_head,
 *  head_dim, d_ff, vocab, rope_freq_base, rms_eps) is read from the GGUF
 *  metadata at lm_load. Feed a different plain-Llama GGUF and it just runs.
 *
 *  NOT in scope (deferred to M1d): the BPE tokenizer. lm_generate() takes
 *  PRE-TOKENIZED input token ids and returns OUTPUT token ids.
 */
#ifndef PKERNEL_LLM_FORWARD_H
#define PKERNEL_LLM_FORWARD_H

/* M1c carries its own version (modver registry; compatibility.md): the
 * Llama forward + greedy generation contract. v1 = SmolLM2-family forward
 * (pre-tokenized ids in, ids out), self-contained transcendental math. */
#define LLM_FORWARD_VER  1

#include <stdint.h>
#include <stddef.h>
#include "gguf.h"

/* Model config read from GGUF metadata + resolved tensor pointers. */
typedef struct {
    /* config (all from metadata, never hardcoded) */
    int      n_layer;
    int      d_model;       /* embedding_length          */
    int      n_head;        /* attention.head_count      */
    int      n_kv_head;     /* attention.head_count_kv   */
    int      head_dim;      /* d_model / n_head           */
    int      d_ff;          /* feed_forward_length        */
    int      vocab;
    float    rope_base;     /* rope.freq_base             */
    float    rms_eps;       /* layer_norm_rms_epsilon     */
    int      max_ctx;       /* context_length (cap for KV) */

    /* the underlying mmap'd file (not owned by us; caller closes) */
    const gguf_file *gf;

    /* resolved tensor handles */
    const gguf_tensor *tok_embd;     /* [d_model, vocab]  Q8_0  */
    const gguf_tensor *out_norm;     /* [d_model]         F32   */
    /* per-layer (n_layer entries each, malloc'd) */
    const gguf_tensor **attn_norm;   /* [d_model] F32           */
    const gguf_tensor **attn_q;      /* [d_model, n_head*head_dim]    Q8_0 */
    const gguf_tensor **attn_k;      /* [d_model, n_kv_head*head_dim] Q8_0 */
    const gguf_tensor **attn_v;      /* [d_model, n_kv_head*head_dim] Q8_0 */
    const gguf_tensor **attn_out;    /* [n_head*head_dim, d_model]    Q8_0 */
    const gguf_tensor **ffn_norm;    /* [d_model] F32           */
    const gguf_tensor **ffn_gate;    /* [d_model, d_ff] Q8_0    */
    const gguf_tensor **ffn_up;      /* [d_model, d_ff] Q8_0    */
    const gguf_tensor **ffn_down;    /* [d_ff, d_model] Q8_0    */

    /* KV cache: [n_layer][max_ctx][n_kv_head*head_dim] (post-RoPE K, raw V) */
    float  *kcache;
    float  *vcache;
    int     n_kv;            /* d_kv = n_kv_head * head_dim     */
    int     pos;             /* number of cached positions so far */

    /* per-step scratch (sized at load; no per-token malloc) */
    float *x;        /* d_model            hidden state            */
    float *xn;       /* d_model            normed hidden           */
    float *q;        /* n_head*head_dim    query                   */
    float *k;        /* n_kv*?             key  (current pos)       */
    float *v;        /* n_kv               value (current pos)     */
    float *attn_o;   /* n_head*head_dim    attention output        */
    float *scores;   /* max_ctx            attention scores        */
    float *ffn_g;    /* d_ff              gate                     */
    float *ffn_u;    /* d_ff              up                       */
    float *ffn_d;    /* d_model           down                     */
    float *logits;   /* vocab                                       */
} lm_model;

/* Return codes. */
#define LM_OK          0
#define LM_E_META    (-1)   /* a required metadata key is missing      */
#define LM_E_TENSOR  (-2)   /* a required tensor is missing / wrong    */
#define LM_E_OOM     (-3)   /* malloc failed                           */
#define LM_E_TYPE    (-4)   /* unexpected tensor type (need Q8_0/F32)  */
#define LM_E_SHAPE   (-5)   /* a tensor shape disagrees with config    */

const char *lm_strerror(int e);

/* Resolve config + tensors from an already-opened GGUF; allocate scratch+KV.
 * gf must outlive the model (we hold pointers into its mmap). 0 on success. */
int  lm_load(lm_model *m, const gguf_file *gf);
void lm_free(lm_model *m);

/* Reset the KV cache (pos -> 0) so a new sequence can be decoded. */
void lm_reset(lm_model *m);

/* Forward ONE token at the current position (uses + appends to KV cache).
 * Writes next-token logits into m->logits (length vocab). Returns 0 / negative.
 * `pos` advances by one. */
int  lm_forward(lm_model *m, int token_id);

/* argmax over m->logits (greedy). */
int  lm_argmax(const lm_model *m);

/* Greedy generation: prefill `n_in` prompt tokens, then generate `n_gen` more,
 * each by argmax of the logits. The generated token ids are written to out[]
 * (length >= n_gen). Returns the number generated (== n_gen) or negative.
 * KV cache is reset at entry. */
int  lm_generate(lm_model *m, const int *in, int n_in, int *out, int n_gen);

/* Libc-free math used internally — exposed for the unit test's hand-checks. */
float lm_expf(float x);
float lm_rsqrtf(float x);   /* 1/sqrt(x) */
void  lm_sincosf(float a, float *s, float *c);

#endif /* PKERNEL_LLM_FORWARD_H */
