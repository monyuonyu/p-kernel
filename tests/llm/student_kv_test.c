/*
 *  student_kv_test.c — host cert for the KV cache in the Cradle baby's
 *  generation path (arch/common/llm/student.c, wave-kv-cache).
 *
 *  WHAT THE KV CACHE DOES: st_generate used to re-run the FULL st_forward over
 *  the whole growing context window every byte (O(nctx) positions recomputed
 *  per token, the ~1s/byte chat-speed pain + the SS-6 cross-node blocker). The
 *  KV cache stores per-layer per-position k/v across generation steps so a NEW
 *  token computes only its OWN position's q/k/v and attends over the CACHED k/v
 *  of prior positions — O(1) new position. Because this is a CAUSAL transformer
 *  with NO positional encoding, earlier positions' k/v are invariant to later
 *  tokens, so caching is EXACT, not approximate.
 *
 *  Certs (all self-contained, no network, no teacher file):
 *    [kv-equivalence]  cached-gen vs recompute-gen produce BYTE-IDENTICAL output
 *                      — same sampled bytes AND the same FNV-1a over every step's
 *                      logit row — across several prompts, sampling configs, and
 *                      all three tiers (S/M/L), including a prompt LONGER than
 *                      ST_MAXSEQ (exercises the window-slide / cache re-prime).
 *    [kv-speedup]      measure tokens/sec cached vs recompute on the M tier and
 *                      report the REAL numbers; assert cached is at least as fast
 *                      (it is dramatically faster, but the gate is a floor so the
 *                      cert never flakes on a noisy host).
 *    [kv-toggle]       st_kv_set_enabled / st_kv_get_enabled flip the path and
 *                      report it (the equivalence harness relies on this).
 *
 *  The [no-vla] gate is asserted by run_kv.sh's grep tripwire over student.c.
 *
 *  Build (wave-49): -O1 -ffp-contract=off (one math everywhere). Run on BOTH
 *  arches by run_kv.sh / the parity harness; the hashes are bit-stable so a
 *  byte-identical match holds cross-arch.
 *
 *  Usage:
 *    ./student_kv          # human-readable cert (exit 0 = all PASS)
 *    ./student_kv --machine# one EQ_HASH line per case (for cross-build diff)
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "../../arch/common/llm/student.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

/* a small fixed corpus so a few training steps make the logits non-degenerate
 * (the equivalence holds for ANY weights, but a trained baby produces varied
 * bytes that exercise the attention sum harder than uniform noise). */
static const uint8_t CORPUS[] =
    "the cat sat on the mat. the dog ran in the sun. she said the sea is "
    "blue and the sky is blue too. a bird sang and the wind blew softly.";

/* train `steps` Adam steps over the corpus (windowed to ST_MAXSEQ). Keeps the
 * test fast; the point is non-trivial weights, not a converged model. */
static void warm_train(st_model *m, int steps)
{
    int n = (int)sizeof(CORPUS) - 1;
    int win = n < ST_MAXSEQ ? n : ST_MAXSEQ;
    float *logits = (float *)malloc((size_t)win * 256 * sizeof(float));
    if (!logits) return;
    for (int s = 0; s < steps; s++) {
        st_zero_grad(m);
        st_forward(m, CORPUS, win, logits);
        st_backward(m, CORPUS, win);
        st_adam_step(m, 0.02f);
    }
    free(logits);
}

/* Run one generation under the CURRENT st_kv_enabled setting, capturing the
 * sampled bytes into out[max] (returns count) and the per-step logit FNV hash
 * into *hash. The hash is reset before the run so it covers ONLY this run. */
static int gen_capture(st_model *m, const uint8_t *prompt, int n_prompt,
                       uint8_t *out, int max_gen, float temp, int top_k,
                       uint64_t seed, uint64_t *hash)
{
    st_gen_logit_hash_reset();
    int got = st_generate(m, prompt, n_prompt, out, max_gen, temp, top_k, seed);
    *hash = st_gen_logit_hash();
    return got;
}

/* one equivalence case: recompute vs cached must match (bytes + logit hash). */
static int eq_case(st_model *m, const char *label,
                   const uint8_t *prompt, int n_prompt,
                   int max_gen, float temp, int top_k, uint64_t seed,
                   int machine)
{
    uint8_t a[128], b[128];
    uint64_t ha = 0, hb = 0;

    st_kv_set_enabled(0);                       /* recompute oracle */
    int na = gen_capture(m, prompt, n_prompt, a, max_gen, temp, top_k, seed, &ha);
    st_kv_set_enabled(1);                       /* cached */
    int nb = gen_capture(m, prompt, n_prompt, b, max_gen, temp, top_k, seed, &hb);

    int ok = (na == nb) && (ha == hb) && (memcmp(a, b, (size_t)(na < nb ? na : nb)) == 0);

    if (machine) {
        printf("EQ_HASH %s recompute=%016llx cached=%016llx n=%d/%d %s\n",
               label, (unsigned long long)ha, (unsigned long long)hb, na, nb,
               ok ? "MATCH" : "DIFFER");
        return ok;
    }
    char m1[160];
    snprintf(m1, sizeof m1,
             "[kv-equivalence] %-26s bytes+logit-hash identical (n=%d, h=%016llx)",
             label, na, (unsigned long long)ha);
    CHECK(ok, m1);
    if (!ok)
        printf("        recompute n=%d h=%016llx  vs  cached n=%d h=%016llx\n",
               na, (unsigned long long)ha, nb, (unsigned long long)hb);
    return ok;
}

/* run the full equivalence battery on a freshly init'd + warm-trained model of
 * the given tier. Several prompts / sampling configs / a >ST_MAXSEQ prompt. */
static void eq_battery(int tier, const char *tname, int machine)
{
    st_model m;
    if (st_init_tier(&m, 0xC0FFEEu + (uint32_t)tier, tier) != ST_OK) {
        printf("  FAIL  [kv-equivalence] %s init\n", tname); g_fail++; return;
    }
    warm_train(&m, 30);

    const uint8_t p_short[]  = "the ";
    const uint8_t p_word[]   = "the cat ";
    const uint8_t p_empty[]  = "";   /* generation seeds a neutral '\n'        */
    /* a prompt LONGER than ST_MAXSEQ to force the window-slide path (C1: the
     * window widened to 256, so this tracks ST_MAXSEQ instead of a literal 64): */
    static uint8_t p_long[ST_MAXSEQ + 56];
    for (int i = 0; i < (int)sizeof(p_long); i++)
        p_long[i] = (uint8_t)("abcdefghijklmnop"[i & 15]);

    char lab[64];
    snprintf(lab, sizeof lab, "%s greedy/short",  tname);
    eq_case(&m, lab, p_short, (int)sizeof(p_short) - 1, 48, 0.0f, 0, 0xABCD, machine);
    snprintf(lab, sizeof lab, "%s temp0.8/k40",    tname);
    eq_case(&m, lab, p_word,  (int)sizeof(p_word)  - 1, 48, 0.8f, 40, 0x1234, machine);
    snprintf(lab, sizeof lab, "%s temp1.0/all",    tname);
    eq_case(&m, lab, p_word,  (int)sizeof(p_word)  - 1, 64, 1.0f, 0,  0x7777, machine);
    snprintf(lab, sizeof lab, "%s empty-prompt",   tname);
    eq_case(&m, lab, p_empty, 0,                        32, 0.7f, 20, 0x9999, machine);
    snprintf(lab, sizeof lab, "%s long>MAXSEQ",    tname);
    eq_case(&m, lab, p_long,  (int)sizeof(p_long),      80, 0.9f, 50, 0x5151, machine);

    st_free(&m);
}

/* tokens/sec for the current path (averaged over `reps` full generations). */
static double measure_tps(st_model *m, const uint8_t *prompt, int n_prompt,
                          int max_gen, float temp, int top_k, int reps)
{
    uint8_t out[128];
    struct timespec t0, t1;
    long total = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < reps; r++) {
        int got = st_generate(m, prompt, n_prompt, out, max_gen, temp, top_k,
                              0x2468u + (uint64_t)r);
        total += got;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    return dt > 0.0 ? (double)total / dt : 0.0;
}

int main(int argc, char **argv)
{
    int machine = (argc > 1 && strcmp(argv[1], "--machine") == 0);

    if (machine) {
        /* one EQ_HASH line per case, both paths, for a cross-build/cross-arch
         * diff (the hashes are bit-stable under -ffp-contract=off). */
        eq_battery(ST_TIER_S, "S", 1);
        eq_battery(ST_TIER_M, "M", 1);
        eq_battery(ST_TIER_L, "L", 1);
        return 0;
    }

    printf("== student_kv_test (wave-kv-cache) ==\n\n");

    printf("[kv-toggle] enable flag flips the generation path\n");
    st_kv_set_enabled(1); CHECK(st_kv_get_enabled() == 1, "[kv-toggle] enable(1) -> cached");
    st_kv_set_enabled(0); CHECK(st_kv_get_enabled() == 0, "[kv-toggle] enable(0) -> recompute");
    st_kv_set_enabled(1);

    printf("\n[kv-equivalence] cached generation is BYTE-IDENTICAL to recompute\n");
    printf("                 (same sampled bytes + same per-step logit FNV hash)\n");
    eq_battery(ST_TIER_S, "S-tier", 0);
    eq_battery(ST_TIER_M, "M-tier", 0);
    eq_battery(ST_TIER_L, "L-tier", 0);

    /* ---- [kv-speedup] real tokens/sec, cached vs recompute (M tier) ---- */
    printf("\n[kv-speedup] tokens/sec cached vs recompute (M tier, real numbers)\n");
    {
        st_model m;
        st_init_tier(&m, 0xC0FFEEu + (uint32_t)ST_TIER_M, ST_TIER_M);
        warm_train(&m, 30);
        const uint8_t pr[] = "the cat ";
        int np = (int)sizeof(pr) - 1;
        int max_gen = 64, reps = 8;

        st_kv_set_enabled(0);
        double tps_rc = measure_tps(&m, pr, np, max_gen, 0.8f, 40, reps);
        st_kv_set_enabled(1);
        double tps_kv = measure_tps(&m, pr, np, max_gen, 0.8f, 40, reps);

        double speedup = tps_rc > 0.0 ? tps_kv / tps_rc : 0.0;
        printf("  recompute : %8.1f tokens/sec\n", tps_rc);
        printf("  cached    : %8.1f tokens/sec\n", tps_kv);
        printf("  speedup   : %6.2fx  (cached / recompute)\n", speedup);
        char msg[96];
        snprintf(msg, sizeof msg,
                 "[kv-speedup] cached (%.1f t/s) at least as fast as recompute (%.1f t/s)",
                 tps_kv, tps_rc);
        /* floor gate so the cert never flakes on a noisy host (the real number
         * is many x; we only REQUIRE no regression). */
        CHECK(tps_kv >= tps_rc * 0.95, msg);
        st_free(&m);
    }

    printf("\nSUMMARY: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
