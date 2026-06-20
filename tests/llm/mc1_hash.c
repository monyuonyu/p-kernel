/*
 *  mc1_hash.c — MC-1 full-teacher equivalence + speedup harness.
 *
 *  Runs a REAL SmolLM2-135M forward (every matmul routed through the size gate
 *  + the row-partitioned pool) and prints:
 *
 *    HASH 0x....            FNV-1a over the final-step logit buffer.
 *    FWDMS  <ms_per_forward> wall-clock per forward call.
 *
 *  run_mc1.sh invokes this at NW in {1,2,4,8} (PKERNEL_MATMUL_THREADS) and
 *  asserts the HASH is byte-identical across every NW (the teacher forward is
 *  byte-identical => one mind, one math), and reports the per-NW timing as the
 *  [par-matmul-speedup] full-forward number.
 *
 *  Usage:  mc1_hash <gguf> <n_in> <n_steps> <tok0..tok{n_in-1}>
 *          (n_steps forwards over the prompt; hash of the LAST step's logits.)
 *
 *  Build: -O1 -ffp-contract=off (one mind, one math). Needs the GGUF; if the
 *  file is absent run_mc1.sh defers this and uses the synthetic equiv instead.
 */
#define _POSIX_C_SOURCE 200112L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../arch/common/llm/gguf.h"
#include "../../arch/common/llm/forward.h"

static uint64_t fnv1a(const float *y, int n)
{
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t *)y;
    size_t bytes = (size_t)n * sizeof(float);
    for (size_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628253ULL; }
    return h;
}

static double now_wall(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <gguf> <n_in> <n_steps> <tok...>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    int n_in    = atoi(argv[2]);
    int n_steps = atoi(argv[3]);
    int prompt[512]; int np = 0;
    for (int i = 4; i < argc && np < 512; i++) prompt[np++] = atoi(argv[i]);
    if (np < n_in) n_in = np;
    if (n_in < 1)  { fprintf(stderr, "no prompt ids\n"); return 2; }

    gguf_file gf;
    if (gguf_open(&gf, path) != GGUF_OK) { fprintf(stderr, "gguf_open failed\n"); return 2; }
    lm_model m;
    if (lm_load(&m, &gf) != LM_OK) { fprintf(stderr, "lm_load failed\n"); gguf_close(&gf); return 2; }

    lm_reset(&m);
    /* prefill the prompt, then step n_steps greedy forwards; hash the LAST. */
    uint64_t h = 0;
    int total_fwd = 0;
    double t0 = now_wall();
    for (int i = 0; i < n_in; i++) {
        if (lm_forward(&m, prompt[i]) != LM_OK) { fprintf(stderr, "fwd fail\n"); return 2; }
        total_fwd++;
    }
    h = fnv1a(m.logits, m.vocab);
    int tok = lm_argmax(&m);
    for (int s = 1; s < n_steps; s++) {
        if (lm_forward(&m, tok) != LM_OK) { fprintf(stderr, "fwd fail\n"); return 2; }
        total_fwd++;
        h = fnv1a(m.logits, m.vocab);
        tok = lm_argmax(&m);
    }
    double dt = now_wall() - t0;

    printf("HASH 0x%016llx\n", (unsigned long long)h);
    printf("FWDMS %.2f\n", dt * 1000.0 / (double)total_fwd);
    printf("VOCAB %d  FORWARDS %d  ARGMAX %d\n", m.vocab, total_fwd, tok);

    lm_free(&m);
    gguf_close(&gf);
    return 0;
}
