/*
 *  llm_shell.c — the bridge that lets the RUNNING kernel generate real text.
 *
 *  Step ① of "give p-kernel a real LLM chat": prove the kernel itself (not a
 *  host test harness) loads the SmolLM2-135M GGUF, tokenizes a prompt, runs the
 *  self-built forward + sampler, detokenizes, and prints the generated text.
 *
 *  This TU is the SEAM between two worlds:
 *    - the kernel side (usermain.c) is built with the T-Kernel placeholder libc
 *      include path and only knows the kernel's `print` primitive;
 *    - the llm engine (gguf/quant/tokenizer/forward/sample) is host/Android-tier
 *      ("身体") code built with the REAL system libc (malloc/mmap/<stdint.h>).
 *  So usermain.c calls exactly ONE function here — llm_shell_run() — passing a
 *  plain `void (*emit)(const char*)` line-printer. Everything libc-heavy stays
 *  on this side of the seam; the kernel side never includes a single llm header.
 *
 *  Honesty (conversation.md §2): host/Android tier. Uses malloc + the engine's
 *  mmap loader; built -O1 -ffp-contract=off with the rest of arch/common/llm/.
 *  Kept OFF the boot path — it only runs when the `llm` shell verb is invoked.
 */
#include "gguf.h"
#include "tokenizer.h"
#include "forward.h"
#include <stdlib.h>     /* malloc / free                    */
#include <string.h>     /* strlen / memcpy                  */
#include <stdio.h>      /* snprintf (output formatting only)*/
#include <time.h>       /* clock_gettime — load/gen timing  */

/* Default model path; overridable via $PKERNEL_LLM_GGUF. */
#ifndef PKERNEL_LLM_DEFAULT_GGUF
#define PKERNEL_LLM_DEFAULT_GGUF "/tmp/smollm2-135m.gguf"
#endif

extern char *getenv(const char *);

typedef void (*emit_fn)(const char *);

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/*
 *  llm_shell_run — load (if needed), tokenize `prompt`, generate up to
 *  `max_gen` tokens, detokenize, and stream the result through `emit`.
 *
 *    prompt   NUL-terminated UTF-8 prompt (may be empty).
 *    max_gen  max tokens to generate (clamped to a sane ceiling).
 *    temp     0 => greedy (deterministic); >0 => sampled.
 *    top_k/top_p/rep_pen/seed  sampler knobs (ignored when temp==0).
 *    emit     line printer (the kernel's `print`); never NULL.
 *
 *  Returns 0 on success, negative on error (model missing / load / OOM).
 *
 *  The model + tokenizer are loaded fresh per call and freed before return:
 *  simple, leak-free, and keeps a model-less boot completely unaffected (the
 *  138MB mmap only ever appears while this verb is running).
 */
int llm_shell_run(const char *prompt, int max_gen,
                  float temp, int top_k, float top_p, float rep_pen,
                  unsigned long long seed, emit_fn emit)
{
    char line[512];

    const char *path = getenv("PKERNEL_LLM_GGUF");
    if (!path || !path[0]) path = PKERNEL_LLM_DEFAULT_GGUF;

    if (max_gen <= 0)   max_gen = 32;
    if (max_gen > 256)  max_gen = 256;   /* keep the shell responsive          */

    snprintf(line, sizeof line, "[llm] model: %s\r\n", path);
    emit(line);

    /* ---- open the GGUF (mmap) ---- */
    double t0 = now_ms();
    gguf_file gf;
    int rc = gguf_open(&gf, path);
    if (rc != GGUF_OK) {
        snprintf(line, sizeof line,
                 "[llm] gguf_open failed: %s (set PKERNEL_LLM_GGUF to a SmolLM2 .gguf)\r\n",
                 gguf_strerror(rc));
        emit(line);
        return -1;
    }

    /* ---- resolve config + tensors + scratch ---- */
    lm_model m;
    rc = lm_load(&m, &gf);
    if (rc != LM_OK) {
        snprintf(line, sizeof line, "[llm] lm_load failed: %s\r\n", lm_strerror(rc));
        emit(line);
        gguf_close(&gf);
        return -2;
    }

    /* ---- build the BPE tokenizer from the same GGUF metadata ---- */
    tokenizer tk;
    rc = tok_load(&tk, &gf);
    if (rc != TOK_OK) {
        snprintf(line, sizeof line, "[llm] tok_load failed: %s\r\n", tok_strerror(rc));
        emit(line);
        lm_free(&m);
        gguf_close(&gf);
        return -3;
    }
    double t_load = now_ms() - t0;

    snprintf(line, sizeof line,
             "[llm] loaded: L=%d d=%d vocab=%d eos=%d  (%.0f ms)\r\n",
             m.n_layer, m.d_model, m.vocab, tk.eos_id, t_load);
    emit(line);

    /* ---- tokenize the prompt (add_bos=0, matching the M1c oracle) ---- */
    int   plen   = (int)strlen(prompt);
    int   pcap   = plen + 16;
    int32_t *pids = (int32_t *)malloc((size_t)pcap * sizeof(int32_t));
    int    *in    = (int *)malloc((size_t)pcap * sizeof(int));
    int    *out   = (int *)malloc((size_t)max_gen * sizeof(int));
    if (!pids || !in || !out) {
        emit("[llm] out of memory\r\n");
        free(pids); free(in); free(out);
        tok_free(&tk); lm_free(&m); gguf_close(&gf);
        return -4;
    }

    int n_in = tok_encode(&tk, prompt, (size_t)plen, /*add_bos=*/0, pids, pcap);
    if (n_in < 0) {
        snprintf(line, sizeof line, "[llm] tok_encode failed: %s\r\n", tok_strerror(n_in));
        emit(line);
        free(pids); free(in); free(out);
        tok_free(&tk); lm_free(&m); gguf_close(&gf);
        return -5;
    }
    if (n_in == 0) {
        /* empty prompt: seed with BOS if the model has one, else token 0. */
        in[0] = (tk.bos_id >= 0) ? (int)tk.bos_id : 0;
        n_in = 1;
    } else {
        for (int i = 0; i < n_in; i++) in[i] = (int)pids[i];
    }

    /* echo prompt ids so the run is reproducible/inspectable */
    {
        int off = snprintf(line, sizeof line, "[llm] prompt ids (%d):", n_in);
        for (int i = 0; i < n_in && off < (int)sizeof line - 12; i++)
            off += snprintf(line + off, sizeof line - (size_t)off, " %d", in[i]);
        snprintf(line + off, sizeof line - (size_t)off, "\r\n");
        emit(line);
    }

    /* ---- generate ---- */
    double g0 = now_ms();
    int n_out;
    if (temp <= 0.0f) {
        snprintf(line, sizeof line, "[llm] mode=greedy  max_gen=%d\r\n", max_gen);
        emit(line);
        n_out = lm_generate_sampled(&m, in, n_in, out, max_gen,
                                    0.0f, 0, 0.0f, 1.0f, tk.eos_id, 0);
    } else {
        snprintf(line, sizeof line,
                 "[llm] mode=sampled  temp=%.2f top_k=%d top_p=%.2f rep_pen=%.2f seed=%llu  max_gen=%d\r\n",
                 (double)temp, top_k, (double)top_p, (double)rep_pen, seed, max_gen);
        emit(line);
        n_out = lm_generate_sampled(&m, in, n_in, out, max_gen,
                                    temp, top_k, top_p, rep_pen,
                                    tk.eos_id, (uint64_t)seed);
    }
    double t_gen = now_ms() - g0;

    if (n_out < 0) {
        snprintf(line, sizeof line, "[llm] generation failed: %s\r\n", lm_strerror(n_out));
        emit(line);
        free(pids); free(in); free(out);
        tok_free(&tk); lm_free(&m); gguf_close(&gf);
        return -6;
    }

    /* ---- detokenize prompt + generated for a readable transcript ---- */
    {
        int  seqn = n_in + n_out;
        int32_t *seq = (int32_t *)malloc((size_t)(seqn > 0 ? seqn : 1) * sizeof(int32_t));
        size_t tcap = (size_t)seqn * 16 + 64;
        char *text = (char *)malloc(tcap);
        if (seq && text) {
            for (int i = 0; i < n_in;  i++) seq[i]         = (int32_t)in[i];
            for (int i = 0; i < n_out; i++) seq[n_in + i]  = (int32_t)out[i];
            int nb = tok_decode(&tk, seq, seqn, text, tcap - 1);
            if (nb >= 0) {
                text[nb] = '\0';
                emit("[llm] --- text -------------------------------------------\r\n");
                emit(text);
                emit("\r\n[llm] -------------------------------------------------\r\n");
            } else {
                emit("[llm] (detokenize buffer too small)\r\n");
            }
        }
        free(seq);
        free(text);
    }

    {
        double ms_tok = (n_out > 0) ? (t_gen / (double)n_out) : 0.0;
        snprintf(line, sizeof line,
                 "[llm] generated %d tokens in %.0f ms (%.1f ms/token)\r\n",
                 n_out, t_gen, ms_tok);
        emit(line);
    }

    free(pids); free(in); free(out);
    tok_free(&tk);
    lm_free(&m);
    gguf_close(&gf);
    return 0;
}

/*
 *  llm_shell_cmd — the kernel-facing entry: parse a raw arg string into the
 *  sampler knobs, then run. Keeps ALL libc-heavy parsing (strtod/strtol) on
 *  this side of the seam so usermain.c stays trivial (it only NUL-terminates
 *  the shell line and hands it over).
 *
 *  Syntax (flags optional, in any order, BEFORE the prompt):
 *    llm [-t temp] [-k topk] [-p topp] [-r reppen] [-s seed] [-n maxgen] <prompt...>
 *  Examples:
 *    llm The capital of France is
 *    llm -t 0.8 -k 40 -p 0.95 -s 42 Once upon a time
 *  Defaults: temp=0 (greedy), top_k=40, top_p=0.95, rep_pen=1.1, seed=1234,
 *            max_gen=32. (top_k/top_p/rep_pen/seed only bite when temp>0.)
 */
int llm_shell_cmd(const char *args, emit_fn emit)
{
    float temp = 0.0f, top_p = 0.95f, rep_pen = 1.1f;
    int   top_k = 40, max_gen = 32;
    unsigned long long seed = 1234ULL;

    const char *p = args ? args : "";
    while (*p == ' ' || *p == '\t') p++;

    /* parse leading "-x val" flags */
    while (p[0] == '-' && p[1] && p[2]) {
        char f = p[1];
        const char *v = p + 2;
        while (*v == ' ' || *v == '\t') v++;
        char *end = NULL;
        switch (f) {
            case 't': temp    = (float)strtod(v, &end); break;
            case 'p': top_p   = (float)strtod(v, &end); break;
            case 'r': rep_pen = (float)strtod(v, &end); break;
            case 'k': top_k   = (int)strtol(v, &end, 10); break;
            case 'n': max_gen = (int)strtol(v, &end, 10); break;
            case 's': seed    = strtoull(v, &end, 10); break;
            default:
                end = NULL;   /* unknown flag: stop flag parsing, treat as prompt */
                break;
        }
        if (!end || end == v) break;     /* not a recognized flag value */
        p = end;
        while (*p == ' ' || *p == '\t') p++;
    }

    return llm_shell_run(p, max_gen, temp, top_k, top_p, rep_pen, seed, emit);
}
