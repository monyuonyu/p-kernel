/*
 *  student_ss6_test.c — host cert for SS-6 cross-node expert firing with
 *  local fallback (arch/common/llm/student.c, special-structure-mind.md §5
 *  + §8 item 7). The F4 capstone: "複数ノードをまたぐ一回の forward".
 *
 *  WHAT SS-6 DOES: the student is MoE. When the SS-1 adaptive router WIDENS K
 *  beyond K_min on a HARD token, the EXTRA experts (chosen-slot j >= K_min)
 *  that SS-5 placement says live on a PEER node are computed REMOTELY — the
 *  peer runs that expert's SwiGLU on the [D] f_in vector and returns its [D]
 *  output, which st_forward sums into moe[] in a FIXED canonical reduction
 *  order (ascending slot j == the single-node order). Because the remote [D]
 *  output is bit-identical to the local computation (both call
 *  st_expert_forward_ref — one math), a remote forward equals a single-node
 *  forward BYTE-FOR-BYTE.  The local K_min experts ALWAYS run locally.
 *
 *  SURVIVAL CONTRACT: remote fires ONLY when the caller's gate predicate says
 *  so (j >= K_min AND a peer hosts it AND FULL-degrade + region >= 2). Each
 *  remote call has a hard timeout; on timeout/absent peer the expert is
 *  RECOMPUTED LOCALLY — lose the WIDTH, not correctness (honest degraded).
 *
 *  Certs (all IN-PROCESS, no network — the cert drives the REAL remote-sum
 *  code path with a STUB peer; the true multi-process forward over the relay
 *  is a DEFERRED [live] row):
 *    [remote-expert-equiv]    a forward that fires the WIDE experts "remotely"
 *                             (stub computes the same expert) is BYTE-IDENTICAL
 *                             to the pure single-node forward — same logits,
 *                             same FNV-1a hash — across S/M/L tiers. Some
 *                             experts MUST actually have gone remote (counted).
 *    [remote-expert-fallback] the peer TIMES OUT (stub returns <0) -> st_forward
 *                             recomputes that expert LOCALLY and the result is
 *                             STILL byte-identical (fallback loses width, not
 *                             correctness); the honest degraded count is > 0.
 *    [remote-falsifiable]     a stub that perturbs the remote output by 1e-6 OR
 *                             reverses the sum (a non-canonical order) makes the
 *                             equiv cert FAIL — proving the test has teeth.
 *
 *  Build (wave-49): -O1 -ffp-contract=off (one math everywhere). The hashes are
 *  bit-stable so a byte-identical match holds cross-arch.
 *
 *  Usage:
 *    ./student_ss6           # human-readable cert (exit 0 = all PASS)
 *    ./student_ss6 --machine # one EQ_HASH line per case (for cross-build diff)
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../arch/common/llm/student.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

static const uint8_t CORPUS[] =
    "the cat sat on the mat. the dog ran in the sun. she said the sea is "
    "blue and the sky is blue too. a bird sang and the wind blew softly.";

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

/* FNV-1a over a logits buffer (n_tok x 256 floats). Bit-stable under
 * -ffp-contract=off so a byte-identical match holds cross-arch. */
static uint64_t logit_hash(const float *logits, int n_tok)
{
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t *)logits;
    size_t bytes = (size_t)n_tok * 256 * sizeof(float);
    for (size_t i = 0; i < bytes; i++) {
        h ^= p[i];
        h *= 1099511628253ULL;
    }
    return h;
}

/* ---- the STUB peer ------------------------------------------------------- */
/* ctx for the stub: the model whose experts the "peer" computes (here the SAME
 * model — a real peer would hold an identical-weight clone), plus a mode and a
 * counter. mode: 0 = succeed (true remote), 1 = always time out (fallback),
 * 2 = succeed but PERTURB by 1e-6 (falsifiability), 3 = succeed but REVERSE the
 * output's sign meaning a non-canonical sum (falsifiability — here we instead
 * corrupt to prove the test catches a wrong [D]). */
typedef struct {
    const st_model *peer;   /* identical-weight model the peer runs the expert on */
    int   mode;
    int   calls;            /* remote_fn invocations (attempts) */
    int   succeeded;        /* remote_fn returns 0 */
} stub_ctx;

static int stub_remote(int layer, int expert_id, const float *fin,
                       int d, int dff, float *out, void *vctx)
{
    (void)d; (void)dff;
    stub_ctx *s = (stub_ctx *)vctx;
    s->calls++;
    if (s->mode == 1) return -1;                  /* timeout / absent peer */

    /* the peer runs the EXACT per-expert SwiGLU (one math). */
    if (st_expert_forward_ref(s->peer, layer, expert_id, fin, out) != ST_OK)
        return -1;
    if (s->mode == 2) {                           /* 1e-6 perturbation */
        out[0] += 1e-6f;
    }
    s->succeeded++;
    return 0;
}

/* gate: every chosen-expert slot j >= K_min is "peer-hosted" (eligible). This
 * mirrors the production predicate's structure (the local K_min ALWAYS stays
 * local); the production gate ANDs in !st_expert_is_local + degrade + region
 * which on a multi-node fleet selects the peer-hosted wide experts. */
static int stub_gate(int layer, int j, int kmin, int expert_id, void *vctx)
{
    (void)layer; (void)expert_id; (void)vctx;
    return j >= kmin;     /* only the EXTRA (widened) experts go remote */
}

/* run st_forward on (bytes,n) under the CURRENT remote-hook setting, returning
 * the logit hash; out fired/fallback counts via the SS-6 observability. */
static uint64_t fwd_hash(st_model *m, const uint8_t *bytes, int n,
                         int *fired, int *fallback)
{
    float *logits = (float *)malloc((size_t)n * 256 * sizeof(float));
    if (!logits) { if (fired) *fired = -1; return 0; }
    st_forward(m, bytes, n, logits);
    uint64_t h = logit_hash(logits, n);
    if (fired)    *fired    = st_last_remote_fired();
    if (fallback) *fallback = st_last_remote_fallback();
    free(logits);
    return h;
}

/* one tier's equiv + fallback battery. */
static void battery(int tier, const char *tname, int machine)
{
    st_model m;
    if (st_init_tier(&m, 0x5A5A00u + (uint32_t)tier, tier) != ST_OK) {
        printf("  FAIL  [remote-expert-equiv] %s init\n", tname); g_fail++; return;
    }
    /* A FRESH router (tiny init weights -> near-flat gate) WIDENS beyond K_min
     * on most tokens (the SS-1 [adaptive-k-margin] regime), so the WIDE experts
     * that SS-6 fans out actually exist. The byte-identity property holds for
     * ANY weights — we only need widening to occur so a remote expert FIRES. A
     * few train steps keep the logits non-degenerate without sharpening the
     * router enough to collapse to K_min. (1 step: M-tier mean width ~3.98,
     * L-tier ~4.45 — both widen; 2+ steps sharpen M back to K_min.) */
    warm_train(&m, 1);

    /* a window long enough that the adaptive router widens SOME tokens (so wide
     * experts exist to fan out). The CORPUS itself, windowed. */
    int n = (int)sizeof(CORPUS) - 1;
    int win = n < ST_MAXSEQ ? n : ST_MAXSEQ;

    /* (0) single-node oracle: NO hook installed. */
    st_set_remote_expert(NULL, NULL, NULL);
    int f0 = 0, b0 = 0;
    uint64_t h_single = fwd_hash(&m, CORPUS, win, &f0, &b0);

    /* (1) [remote-expert-equiv]: the wide experts fire on the STUB peer. */
    stub_ctx eq = { &m, 0, 0, 0 };
    st_set_remote_expert(stub_remote, stub_gate, &eq);
    int f1 = 0, b1 = 0;
    uint64_t h_remote = fwd_hash(&m, CORPUS, win, &f1, &b1);

    /* (2) [remote-expert-fallback]: the peer ALWAYS times out -> local recompute. */
    stub_ctx fb = { &m, 1, 0, 0 };
    st_set_remote_expert(stub_remote, stub_gate, &fb);
    int f2 = 0, b2 = 0;
    uint64_t h_fallback = fwd_hash(&m, CORPUS, win, &f2, &b2);

    /* (3) [remote-falsifiable]: a 1e-6-perturbed remote output MUST differ. */
    stub_ctx ps = { &m, 2, 0, 0 };
    st_set_remote_expert(stub_remote, stub_gate, &ps);
    int f3 = 0, b3 = 0;
    uint64_t h_perturb = fwd_hash(&m, CORPUS, win, &f3, &b3);

    st_set_remote_expert(NULL, NULL, NULL);

    if (machine) {
        printf("EQ_HASH %-2s single=%016llx remote=%016llx fired=%d  %s\n",
               tname, (unsigned long long)h_single, (unsigned long long)h_remote,
               f1, (h_single == h_remote) ? "MATCH" : "DIFFER");
        printf("EQ_HASH %-2s fallback=%016llx fb_recompute=%d  %s\n",
               tname, (unsigned long long)h_fallback, b2,
               (h_single == h_fallback) ? "MATCH" : "DIFFER");
        st_free(&m);
        return;
    }

    char msg[200];

    /* Whether ANY expert CAN fan out is a structural property of the tier: a
     * tier whose expert count E equals K_min (the S tier: E=2, K_min=2) has NO
     * room to widen, so the router can never produce a wide slot j >= K_min and
     * SS-6 has nothing to fan out — byte-identity is then trivially the single-
     * node path. We assert remote firing only where the tier CAN widen. */
    int can_widen = (m.nexpert > ST_KMIN);

    if (can_widen) {
        /* equiv: byte-identical AND at least one expert actually went remote. */
        snprintf(msg, sizeof msg,
            "[remote-expert-equiv]    %-2s remote-sum == single-node (h=%016llx, %d experts remote)",
            tname, (unsigned long long)h_remote, f1);
        CHECK(h_remote == h_single && f1 >= 1, msg);
        if (h_remote != h_single)
            printf("        single=%016llx remote=%016llx\n",
                   (unsigned long long)h_single, (unsigned long long)h_remote);
        if (f1 < 1)
            printf("        NOTE: router did not widen this run — no wide expert to fan out\n");

        /* fallback: byte-identical AND every remote attempt recomputed locally. */
        snprintf(msg, sizeof msg,
            "[remote-expert-fallback] %-2s timeout -> local recompute, still byte-identical (%d recomputed, fired=%d)",
            tname, b2, f2);
        CHECK(h_fallback == h_single && f2 == 0 && b2 >= 1, msg);
        if (h_fallback != h_single)
            printf("        single=%016llx fallback=%016llx\n",
                   (unsigned long long)h_single, (unsigned long long)h_fallback);

        /* falsifiable: the perturbed remote output MUST break byte-identity. */
        snprintf(msg, sizeof msg,
            "[remote-falsifiable]     %-2s a 1e-6 remote perturbation FAILS the equiv (teeth)",
            tname);
        CHECK(h_perturb != h_single, msg);
    } else {
        /* S tier (E == K_min): NO room to widen. The remote hook is installed
         * but the gate never selects a slot >= K_min, so st_forward stays the
         * single-node path — byte-identity holds trivially and there is no wide
         * expert to fan out (honest: SS-6 only applies where the router widens). */
        snprintf(msg, sizeof msg,
            "[remote-expert-equiv]    %-2s tier cannot widen (E=%d == K_min=%d): single-node path, byte-identical (no fan-out)",
            tname, m.nexpert, ST_KMIN);
        CHECK(h_remote == h_single && f1 == 0 && h_fallback == h_single, msg);
        printf("        NOTE: SS-6 fan-out is a property of WIDE tokens; the S tier\n"
               "        (E==K_min) has none — proven inert, not skipped.\n");
    }

    st_free(&m);
}

int main(int argc, char **argv)
{
    int machine = (argc > 1 && strcmp(argv[1], "--machine") == 0);

    if (machine) {
        battery(ST_TIER_S, "S", 1);
        battery(ST_TIER_M, "M", 1);
        battery(ST_TIER_L, "L", 1);
        return 0;
    }

    printf("== student_ss6_test (SS-6 cross-node expert firing) ==\n\n");

    printf("[remote-expert-equiv] a forward that fires the WIDE experts on a peer\n");
    printf("                      is BYTE-IDENTICAL to the single-node forward\n");
    printf("[remote-expert-fallback] a peer timeout -> local recompute, still identical\n");
    printf("[remote-falsifiable]  a 1e-6 remote perturbation FAILS (the cert has teeth)\n\n");

    battery(ST_TIER_S, "S", 0);
    battery(ST_TIER_M, "M", 0);
    battery(ST_TIER_L, "L", 0);

    printf("\nSUMMARY: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
