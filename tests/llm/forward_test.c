/*
 *  forward_test.c — host cert for the M1c Llama forward + greedy generation
 *                   (arch/common/llm/forward.c).
 *
 *  Two certs:
 *
 *  (1) SYNTHETIC SANITY UNIT  [forward-sanity]  (no network, no model file):
 *      Build a TINY but spec-valid all-F32 GGUF in memory (1 layer, d_model 4,
 *      2 Q heads / 1 KV head GQA, head_dim 2, d_ff 4, vocab 3) with hand-set
 *      weights, open it via gguf_open, run lm_forward, and compare its logits
 *      AND its greedy argmax to an INDEPENDENT reference forward written from
 *      scratch in this file (separate code path, same math spec). Exit 0 = PASS.
 *
 *  (2) REAL-MODEL FORWARD  (needs the SmolLM2-135M GGUF, arg 1):
 *      Load the real model, prefill a FIXED prompt of input token ids, greedy-
 *      generate N tokens, print the generated token-id sequence + ms/token.
 *      The external oracle (llama.cpp) is run by run_forward.sh, which diffs
 *      our printed token ids against llama.cpp's greedy output. THIS is the
 *      [llm-sentence] cert. This program only emits "GEN: id id id ..." for the
 *      script to compare; it does not embed the oracle.
 *
 *  Build (wave-49): -O1 -ffp-contract=off (one math everywhere — load-bearing).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#include "../../arch/common/llm/gguf.h"
#include "../../arch/common/llm/forward.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

/* ---------- little-endian byte writer (same shape as gguf_test.c) -------- */
typedef struct { uint8_t *p; size_t cap, len; } buf;
static void bput(buf *b, const void *src, size_t n) {
    if (b->len + n > b->cap) { b->cap = (b->len + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->len, src, n); b->len += n;
}
static void bu8 (buf *b, uint8_t v)  { bput(b, &v, 1); }
static void bu32(buf *b, uint32_t v) { uint8_t t[4]; for(int i=0;i<4;i++)t[i]=(v>>(8*i))&0xff; bput(b,t,4); }
static void bu64(buf *b, uint64_t v) { uint8_t t[8]; for(int i=0;i<8;i++)t[i]=(v>>(8*i))&0xff; bput(b,t,8); }
static void bf32(buf *b, float f)    { uint32_t u; memcpy(&u,&f,4); bu32(b,u); }
static void bstr(buf *b, const char *s){ uint64_t n=strlen(s); bu64(b,n); bput(b,(const void*)s,n); }
static void kv_str(buf *b, const char *k, const char *v){ bstr(b,k); bu32(b,GGUF_T_STRING); bstr(b,v); }
static void kv_u32(buf *b, const char *k, uint32_t v){ bstr(b,k); bu32(b,GGUF_T_UINT32); bu32(b,v); }
static void kv_f32(buf *b, const char *k, float v){ bstr(b,k); bu32(b,GGUF_T_FLOAT32); bf32(b,v); }

/* ================= tiny synthetic model: dims & hand-set weights ========= */
#define TD   4    /* d_model            */
#define TH   2    /* n_head             */
#define TKV  1    /* n_kv_head          */
#define THD  2    /* head_dim = TD/TH   */
#define TFF  4    /* d_ff               */
#define TV   3    /* vocab              */
#define TL   1    /* n_layer            */
#define TBASE 100000.0f
#define TEPS  1e-5f
#define TALIGN 32u

/* All weights. Chosen small/varied so nothing degenerate. Row-major
 * [in=ne0][out=ne1] for the matmul tensors (out_features rows of in_features),
 * which is how ggml stores them and how matmul_tensor reads them. */
static const float W_tok_embd[TV][TD] = {        /* [d_model, vocab]: row v = token v */
    { 0.10f, -0.20f,  0.30f,  0.05f },
    {-0.15f,  0.25f,  0.10f, -0.30f },
    { 0.20f,  0.05f, -0.25f,  0.15f },
};
static const float W_attn_norm[TD] = { 1.1f, 0.9f, 1.0f, 1.05f };
static const float W_ffn_norm [TD] = { 0.95f, 1.0f, 1.1f, 0.9f };
static const float W_out_norm [TD] = { 1.0f, 1.0f, 1.0f, 1.0f };
/* matmul tensors stored as [out][in] in memory (row i = out feature i) */
static const float W_q [TH*THD][TD] = {  /* [d_model, n_head*head_dim] -> rows=out=4 */
    { 0.10f, 0.00f, -0.10f, 0.20f },
    { 0.05f, 0.15f,  0.00f, -0.05f },
    {-0.20f, 0.10f,  0.05f,  0.00f },
    { 0.00f, -0.10f, 0.20f,  0.10f },
};
static const float W_k [TKV*THD][TD] = { /* out=2 */
    { 0.12f, -0.05f, 0.10f,  0.00f },
    {-0.08f,  0.20f, 0.00f,  0.15f },
};
static const float W_v [TKV*THD][TD] = { /* out=2 */
    { 0.05f, 0.10f, -0.15f, 0.20f },
    { 0.20f, -0.05f, 0.10f, 0.00f },
};
static const float W_o [TD][TH*THD] = {  /* [n_head*head_dim, d_model] -> rows=out=4 in=4 */
    { 0.10f, 0.05f, -0.10f, 0.00f },
    { 0.00f, 0.15f,  0.05f, 0.10f },
    {-0.05f, 0.10f,  0.00f, 0.20f },
    { 0.20f, 0.00f,  0.10f, -0.05f },
};
static const float W_gate[TFF][TD] = {   /* [d_model, d_ff] rows=out=4 in=4 */
    { 0.10f, -0.10f, 0.05f, 0.00f },
    { 0.00f,  0.20f, -0.05f, 0.10f },
    { 0.15f,  0.00f, 0.10f, -0.10f },
    {-0.05f,  0.10f, 0.00f,  0.05f },
};
static const float W_up[TFF][TD] = {
    { 0.05f, 0.10f, 0.00f, -0.05f },
    { 0.10f, -0.05f, 0.15f, 0.00f },
    {-0.10f, 0.00f, 0.05f,  0.10f },
    { 0.00f, 0.15f, -0.10f, 0.05f },
};
static const float W_down[TD][TFF] = {   /* [d_ff, d_model] rows=out=4 in=4 */
    { 0.10f, 0.00f, -0.05f, 0.10f },
    { 0.05f, 0.10f, 0.00f, -0.10f },
    {-0.10f, 0.05f, 0.10f,  0.00f },
    { 0.00f, -0.05f, 0.10f, 0.05f },
};

static int build_tiny_gguf(const char *path)
{
    buf b = {0};
    bput(&b, "GGUF", 4);
    bu32(&b, 3);
    /* tensors: token_embd, output_norm, + per layer: attn_norm, q,k,v,output,
     * ffn_norm, gate, up, down = 2 + 9 = 11 */
    bu64(&b, 11);
    bu64(&b, 9);   /* metadata kv count */

    kv_str(&b, "general.architecture", "llama");
    kv_u32(&b, "llama.block_count", TL);
    kv_u32(&b, "llama.embedding_length", TD);
    kv_u32(&b, "llama.attention.head_count", TH);
    kv_u32(&b, "llama.attention.head_count_kv", TKV);
    kv_u32(&b, "llama.feed_forward_length", TFF);
    kv_u32(&b, "llama.context_length", 16);
    kv_f32(&b, "llama.rope.freq_base", TBASE);
    kv_f32(&b, "llama.attention.layer_norm_rms_epsilon", TEPS);

    /* tensor-info table: emit (name, n_dims, dims..., type, offset). We lay the
     * data out contiguously, each tensor 32-aligned. Build a parallel list of
     * (data ptr, n_floats) to write after the table. */
    struct { const char *name; int nd; uint64_t d0, d1; const float *data; uint64_t nf; } T[11];
    int n = 0;
    #define ADD2(nm, d0_, d1_, dat) do { T[n].name=nm; T[n].nd=2; T[n].d0=d0_; T[n].d1=d1_; \
        T[n].data=(const float*)dat; T[n].nf=(uint64_t)(d0_)*(d1_); n++; } while(0)
    #define ADD1(nm, d0_, dat) do { T[n].name=nm; T[n].nd=1; T[n].d0=d0_; T[n].d1=0; \
        T[n].data=(const float*)dat; T[n].nf=(uint64_t)(d0_); n++; } while(0)

    ADD2("token_embd.weight", TD, TV, W_tok_embd);     /* [d_model, vocab] */
    ADD1("output_norm.weight", TD, W_out_norm);
    ADD1("blk.0.attn_norm.weight", TD, W_attn_norm);
    ADD2("blk.0.attn_q.weight", TD, TH*THD, W_q);
    ADD2("blk.0.attn_k.weight", TD, TKV*THD, W_k);
    ADD2("blk.0.attn_v.weight", TD, TKV*THD, W_v);
    ADD2("blk.0.attn_output.weight", TH*THD, TD, W_o);
    ADD1("blk.0.ffn_norm.weight", TD, W_ffn_norm);
    ADD2("blk.0.ffn_gate.weight", TD, TFF, W_gate);
    ADD2("blk.0.ffn_up.weight", TD, TFF, W_up);
    ADD2("blk.0.ffn_down.weight", TFF, TD, W_down);

    /* compute offsets (each tensor 32-aligned within the data section) */
    uint64_t off[11], cur = 0;
    for (int i = 0; i < n; i++) {
        off[i] = cur;
        uint64_t bytes = T[i].nf * 4;
        uint64_t pad = (TALIGN - (bytes % TALIGN)) % TALIGN;
        cur += bytes + pad;
    }
    /* emit tensor infos */
    for (int i = 0; i < n; i++) {
        bstr(&b, T[i].name);
        bu32(&b, (uint32_t)T[i].nd);
        bu64(&b, T[i].d0);
        if (T[i].nd == 2) bu64(&b, T[i].d1);
        bu32(&b, GGML_TYPE_F32);
        bu64(&b, off[i]);
    }
    /* pad header+table to alignment, then data */
    uint64_t pad = (TALIGN - (b.len % TALIGN)) % TALIGN;
    for (uint64_t i = 0; i < pad; i++) bu8(&b, 0);
    for (int i = 0; i < n; i++) {
        uint64_t bytes = T[i].nf * 4;
        for (uint64_t k = 0; k < T[i].nf; k++) bf32(&b, T[i].data[k]);
        uint64_t tp = (TALIGN - (bytes % TALIGN)) % TALIGN;
        for (uint64_t k = 0; k < tp; k++) bu8(&b, 0);
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(b.p); return -1; }
    size_t w = fwrite(b.p, 1, b.len, f);
    fclose(f); free(b.p);
    return (w == b.len) ? 0 : -1;
}

/* ============ INDEPENDENT reference forward (separate code path) ========= */
/* Uses libm (this is a host test, allowed to use libm for the ORACLE side —
 * the device-side forward.c stays libc-free). Implements the same spec from
 * scratch so a bug in forward.c that I also wrote can't hide. Greedy multi-step
 * with its own KV cache. */
#define REFMAXCTX 16
typedef struct {
    float kc[REFMAXCTX][TKV*THD];
    float vc[REFMAXCTX][TKV*THD];
    int   pos;
} refstate;

static void ref_rmsnorm(const float *x, const float *w, int d, float eps, float *y) {
    double ss = 0; for (int i=0;i<d;i++) ss += (double)x[i]*x[i];
    double inv = 1.0/sqrt(ss/d + eps);
    for (int i=0;i<d;i++) y[i] = (float)(x[i]*inv*w[i]);
}
static void ref_matmul(const float *w, int in, int out, const float *x, float *y) {
    for (int i=0;i<out;i++){ double a=0; for(int j=0;j<in;j++) a += (double)w[i*in+j]*x[j]; y[i]=(float)a; }
}
static void ref_rope(float *h, int hd, int pos, float base) {
    for (int j=0;j<hd/2;j++){
        double freq = pow((double)base, -2.0*j/hd);
        double th = pos*freq;
        double c=cos(th), s=sin(th);
        double x0=h[2*j], x1=h[2*j+1];
        h[2*j]   = (float)(x0*c - x1*s);
        h[2*j+1] = (float)(x0*s + x1*c);
    }
}
static void ref_forward(refstate *st, int tok, float *logits) {
    float x[TD];
    for (int i=0;i<TD;i++) x[i] = W_tok_embd[tok][i];
    int pos = st->pos;
    /* layer 0 */
    float xn[TD];
    ref_rmsnorm(x, W_attn_norm, TD, TEPS, xn);
    float q[TH*THD], k[TKV*THD], v[TKV*THD];
    ref_matmul(&W_q[0][0], TD, TH*THD, xn, q);
    ref_matmul(&W_k[0][0], TD, TKV*THD, xn, k);
    ref_matmul(&W_v[0][0], TD, TKV*THD, xn, v);
    for (int h=0;h<TH;h++)  ref_rope(q+h*THD, THD, pos, TBASE);
    for (int h=0;h<TKV;h++) ref_rope(k+h*THD, THD, pos, TBASE);
    for (int i=0;i<TKV*THD;i++){ st->kc[pos][i]=k[i]; st->vc[pos][i]=v[i]; }
    float ao[TH*THD];
    int gper = TH/TKV;
    double scale = 1.0/sqrt((double)THD);
    for (int h=0;h<TH;h++){
        int kvh = h/gper;
        double sc[REFMAXCTX]; double mx=-1e300;
        for (int t=0;t<=pos;t++){
            double d=0; for(int i=0;i<THD;i++) d += (double)q[h*THD+i]*st->kc[t][kvh*THD+i];
            d*=scale; sc[t]=d; if(d>mx)mx=d;
        }
        double sum=0; for(int t=0;t<=pos;t++){ sc[t]=exp(sc[t]-mx); sum+=sc[t]; }
        for (int i=0;i<THD;i++){
            double acc=0;
            for (int t=0;t<=pos;t++) acc += (sc[t]/sum)*st->vc[t][kvh*THD+i];
            ao[h*THD+i]=(float)acc;
        }
    }
    float od[TD];
    ref_matmul(&W_o[0][0], TH*THD, TD, ao, od);
    for (int i=0;i<TD;i++) x[i]+=od[i];
    ref_rmsnorm(x, W_ffn_norm, TD, TEPS, xn);
    float gt[TFF], up[TFF];
    ref_matmul(&W_gate[0][0], TD, TFF, xn, gt);
    ref_matmul(&W_up[0][0], TD, TFF, xn, up);
    for (int i=0;i<TFF;i++){ double si = gt[i]/(1.0+exp(-(double)gt[i])); gt[i]=(float)(si*up[i]); }
    float dn[TD];
    ref_matmul(&W_down[0][0], TFF, TD, gt, dn);
    for (int i=0;i<TD;i++) x[i]+=dn[i];
    /* final */
    ref_rmsnorm(x, W_out_norm, TD, TEPS, xn);
    ref_matmul(&W_tok_embd[0][0], TD, TV, xn, logits);
    st->pos = pos+1;
}
static int ref_argmax(const float *l, int n){ int b=0; for(int i=1;i<n;i++) if(l[i]>l[b])b=i; return b; }

/* ============================== synthetic cert =========================== */
static int run_sanity(void)
{
    printf("\n=== [forward-sanity] TINY HAND-WEIGHT CERT (no network) ===\n");
    char path[] = "/tmp/pkernel_fwd_synth_XXXXXX";
    int fd = mkstemp(path); if (fd>=0) close(fd);
    if (build_tiny_gguf(path) != 0) { printf("  FAIL build_tiny_gguf\n"); return 1; }

    gguf_file gf;
    int rc = gguf_open(&gf, path);
    if (rc != GGUF_OK) { printf("  FAIL gguf_open: %s\n", gguf_strerror(rc)); unlink(path); return 1; }
    lm_model m;
    rc = lm_load(&m, &gf);
    if (rc != LM_OK) { printf("  FAIL lm_load: %s\n", lm_strerror(rc)); gguf_close(&gf); unlink(path); return 1; }

    /* config readback */
    CHECK(m.n_layer==TL && m.d_model==TD && m.n_head==TH && m.n_kv_head==TKV &&
          m.head_dim==THD && m.d_ff==TFF && m.vocab==TV, "config parsed from GGUF metadata");
    CHECK(m.rope_base==TBASE, "rope_base == 100000 (from metadata)");

    /* drive a 3-token sequence through both DUT and the independent reference,
     * comparing logits each step + the greedy token. */
    int seq[3] = { 0, 2, 1 };
    refstate ref; memset(&ref, 0, sizeof(ref)); ref.pos = 0;
    lm_reset(&m);
    int allmatch = 1, argmatch = 1;
    double worst = 0;
    for (int s=0; s<3; s++) {
        rc = lm_forward(&m, seq[s]);
        if (rc != LM_OK) { printf("  FAIL lm_forward step %d: %s\n", s, lm_strerror(rc)); allmatch=0; break; }
        float rl[TV];
        ref_forward(&ref, seq[s], rl);
        for (int v=0; v<TV; v++) {
            double diff = fabs((double)m.logits[v] - rl[v]);
            double rel = diff / (1e-6 + fabs((double)rl[v]));
            if (rel > worst) worst = rel;
            if (rel > 1e-3 && diff > 1e-4) allmatch = 0;
        }
        if (lm_argmax(&m) != ref_argmax(rl, TV)) argmatch = 0;
        printf("    step %d: DUT logits [% .5f % .5f % .5f]  ref [% .5f % .5f % .5f]  argmax DUT=%d ref=%d\n",
               s, m.logits[0], m.logits[1], m.logits[2], rl[0], rl[1], rl[2],
               lm_argmax(&m), ref_argmax(rl, TV));
    }
    printf("    worst relative logit error vs independent reference: %.2e\n", worst);
    CHECK(allmatch, "DUT logits match independent reference (rel<1e-3) every step");
    CHECK(argmatch, "DUT greedy argmax matches independent reference every step");

    lm_free(&m); gguf_close(&gf); unlink(path);
    return (allmatch && argmatch) ? 0 : 1;
}

/* ============================== real-model run =========================== */
/* FIXED prompt token ids. These are SmolLM2 token ids; run_forward.sh derives
 * the same ids from llama.cpp's tokenizer (tokenizer out of scope, M1d) and
 * passes them so both sides see identical input. Defaults below are llama.cpp's
 * tokenization of "The capital of France is" (BOS=1 + the 5 word-pieces); the
 * script overrides via argv if it tokenizes differently. */
static long now_us(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec*1000000L+ts.tv_nsec/1000; }

static int run_real(const char *path, int *prompt, int n_in, int n_gen)
{
    printf("\n=== REAL-MODEL FORWARD (SmolLM2-135M) ===\n");
    gguf_file gf;
    int rc = gguf_open(&gf, path);
    if (rc != GGUF_OK) { printf("gguf_open(%s): %s\n", path, gguf_strerror(rc)); return 2; }
    lm_model m;
    rc = lm_load(&m, &gf);
    if (rc != LM_OK) { printf("lm_load: %s\n", lm_strerror(rc)); gguf_close(&gf); return 2; }
    printf("  config: L=%d d=%d nh=%d nkv=%d hd=%d ff=%d vocab=%d base=%.0f eps=%.1e\n",
           m.n_layer, m.d_model, m.n_head, m.n_kv_head, m.head_dim, m.d_ff,
           m.vocab, (double)m.rope_base, (double)m.rms_eps);

    int *out = malloc(sizeof(int)*n_gen);
    long t0 = now_us();
    rc = lm_generate(&m, prompt, n_in, out, n_gen);
    long t1 = now_us();
    if (rc < 0) { printf("lm_generate: %s\n", lm_strerror(rc)); free(out); lm_free(&m); gguf_close(&gf); return 2; }

    int n_fwd = n_in + n_gen;     /* total forward calls */
    double ms_tok = (double)(t1 - t0) / 1000.0 / (double)n_fwd;
    printf("  prompt ids:");  for (int i=0;i<n_in;i++) printf(" %d", prompt[i]); printf("\n");
    /* machine-readable line for the oracle script */
    printf("GEN:");          for (int i=0;i<n_gen;i++) printf(" %d", out[i]); printf("\n");
    printf("  ms/token: %.1f  (%d forwards: %d prompt + %d generated, plain C no SIMD)\n",
           ms_tok, n_fwd, n_in, n_gen);

    free(out); lm_free(&m); gguf_close(&gf);
    return 0;
}

/* probe mode: forward a full token sequence, then print the top-2 next-token
 * logits and their gap. Used by run_forward.sh to classify a greedy divergence
 * as a genuine numerical near-tie (tiny gap) vs a structural bug (large gap). */
static int run_probe(const char *path, int *seq, int n_seq, int expect_oracle_tok)
{
    gguf_file gf; lm_model m;
    if (gguf_open(&gf, path) != GGUF_OK) return 2;
    if (lm_load(&m, &gf) != LM_OK) { gguf_close(&gf); return 2; }
    lm_reset(&m);
    for (int i = 0; i < n_seq; i++) if (lm_forward(&m, seq[i]) != LM_OK) { lm_free(&m); gguf_close(&gf); return 2; }
    int a = lm_argmax(&m);
    float la = m.logits[a];
    float lo = (expect_oracle_tok >= 0 && expect_oracle_tok < m.vocab) ? m.logits[expect_oracle_tok] : 0.0f;
    /* second-best */
    float l2 = -3e38f; int s2 = -1;
    for (int v = 0; v < m.vocab; v++) if (v != a && m.logits[v] > l2) { l2 = m.logits[v]; s2 = v; }
    printf("PROBE argmax=%d logit=%.6f  2nd=%d logit=%.6f  gap=%.6f", a, la, s2, l2, la - l2);
    if (expect_oracle_tok >= 0)
        printf("  oracle_tok=%d logit=%.6f  delta_to_argmax=%.6f", expect_oracle_tok, lo, la - lo);
    printf("\n");
    lm_free(&m); gguf_close(&gf);
    return 0;
}

int main(int argc, char **argv)
{
    /* probe subcommand: forward_test <gguf> probe <oracle_tok> <id...> */
    if (argc >= 4 && strcmp(argv[2], "probe") == 0) {
        int oracle_tok = atoi(argv[3]);
        int seq[512]; int n = 0;
        for (int i = 4; i < argc && n < 512; i++) seq[n++] = atoi(argv[i]);
        return run_probe(argv[1], seq, n, oracle_tok);
    }

    /* argv[1] (optional) = real GGUF path.
     * argv[2..] (optional) = "n_in n_gen tok0 tok1 ... tok{n_in-1}" for the
     * fixed prompt; if absent, a built-in default prompt is used. */
    int sanity_rc = run_sanity();

    int real_rc = 0;
    if (argc >= 2) {
        /* default prompt: SmolLM2 ids for "The capital of France is"
         * (BOS=1). The script overrides these to match its own tokenization. */
        int defprompt[] = { 1, 504, 5538, 282, 7138, 314 };
        int n_in = (int)(sizeof(defprompt)/sizeof(defprompt[0]));
        int n_gen = 8;
        int *prompt = defprompt;
        int parsed[256];
        if (argc >= 4) {
            n_in  = atoi(argv[2]);
            n_gen = atoi(argv[3]);
            if (n_in > 256) n_in = 256;
            for (int i=0;i<n_in && 4+i<argc;i++) parsed[i]=atoi(argv[4+i]);
            prompt = parsed;
        }
        real_rc = run_real(argv[1], prompt, n_in, n_gen);
    } else {
        printf("\n[note] no real GGUF arg — synthetic sanity cert only.\n");
    }

    printf("\n=== SUMMARY: %d pass, %d fail ===\n", g_pass, g_fail);
    return (g_fail==0 && sanity_rc==0 && real_rc==0) ? 0 : 1;
}
