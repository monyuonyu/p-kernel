/*
 *  student_test.c — host cert for the Cradle baby (arch/common/llm/student.c),
 *  NS-1 "first heartbeat" (docs/architecture/20-architecture/native-student.md §B.6/§B.7).
 *
 *  Four falsifiable certs (honest-growth discipline — NO fake progress):
 *
 *    [baby-gradcheck]    analytic vs central finite-diff gradients agree
 *                        (max relative error over a strided spread of every
 *                        weight family). The foundation: without it nothing
 *                        else is trustworthy.
 *    [honest-baby]       BEFORE distillation the baby is near-random: held-out
 *                        next-byte loss ~ ln(256) = 5.545 nats. Proves it
 *                        starts ignorant — no pre-baked knowledge.
 *    [distill-loss-drops] after N sleep rounds on teacher byte-targets the
 *                        held-out next-byte loss MEASURABLY drops. The headline.
 *    [distill-grounded]  a SCRAMBLED-teacher control (shuffled target bytes)
 *                        yields NO meaningful gain — the real gain is teaching,
 *                        not an artifact of "any training lowers a metric".
 *
 *  TEACHER -> BYTE BRIDGE (chosen: SEQUENCE-LEVEL, native-student.md §B.6 (a)):
 *    The teacher (SmolLM2-135M via forward.c) greedily generates text; the baby
 *    learns to predict the next RAW BYTE of that text. Simplest valid "learn to
 *    babble like the teacher talks", and it side-steps the teacher(50k BPE) vs
 *    baby(256 byte) vocab-mismatch entirely (§A.5 honest gap) — exactly the
 *    NS-1 recommendation to keep one frontier.
 *
 *    The teacher run is SLOW (~250ms/token), so it is done ONCE by `harvest`
 *    mode (run.sh, when a GGUF is present) into a tiny text fixture; the cert
 *    LOADS the fixture and never re-runs the teacher. If no fixture exists the
 *    cert falls back to a built-in deterministic byte stream (clearly labelled),
 *    so the gradcheck + learning machinery is still exercised in CI.
 *
 *  Build (wave-49): -O1 -ffp-contract=off (one math everywhere).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../arch/common/llm/student.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

static double now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---------- training corpus (bytes) ---------- */
/* Loaded from the teacher fixture if present; else a built-in fallback. */
static uint8_t *g_corpus = NULL;
static int      g_corpus_n = 0;
static const char *g_corpus_src = "(none)";

/* built-in fallback: a short, structured English-ish byte stream. NOT random —
 * it has real byte-level regularity for the baby to learn (so CI without a
 * teacher still shows learning), but it is honestly labelled NOT the teacher. */
static const char FALLBACK_TEXT[] =
    "the cat sat on the mat. the dog ran in the sun. "
    "she said the sea is blue and the sky is blue too. "
    "the cat and the dog ran to the sea and sat on the sand. "
    "the sun set and the sky was red. the cat slept on the mat again. "
    "the dog ran on the sand and the cat sat in the sun by the sea. ";

static int load_corpus(const char *fixture_path)
{
    if (fixture_path) {
        FILE *f = fopen(fixture_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < (1 << 20)) {
                g_corpus = (uint8_t *)malloc(sz);
                if (g_corpus && fread(g_corpus, 1, sz, f) == (size_t)sz) {
                    g_corpus_n = (int)sz; g_corpus_src = "teacher fixture";
                    fclose(f); return 0;
                }
                free(g_corpus); g_corpus = NULL;
            }
            fclose(f);
        }
    }
    /* fallback */
    int n = (int)sizeof(FALLBACK_TEXT) - 1;
    g_corpus = (uint8_t *)malloc(n);
    memcpy(g_corpus, FALLBACK_TEXT, n);
    g_corpus_n = n;
    g_corpus_src = "built-in fallback (NO teacher present)";
    return 0;
}

/* slice the corpus into a window of length len starting at off (wrapping). */
static void window(uint8_t *dst, int off, int len)
{
    for (int i = 0; i < len; i++) dst[i] = g_corpus[(off + i) % g_corpus_n];
}

/* ====================================================================== */
/* [baby-gradcheck]                                                       */
/* ====================================================================== */
static int cert_gradcheck(void)
{
    printf("\n[baby-gradcheck] analytic vs finite-diff gradients\n");
    st_model m;
    if (st_init(&m, 0xA11CE) != ST_OK) { printf("  init OOM\n"); return 1; }
    /* a short, content-rich window so every weight family sees real signal */
    uint8_t buf[12];
    for (int i = 0; i < 12; i++) buf[i] = (uint8_t)("abcdXYZ. 012"[i % 12]);
    /* st_grad_check freezes routing (top-K selection is non-differentiable) and
     * probes each weight family's largest-|gradient| indices with eps=1e-2 (the
     * regime where float32 FD is resolvable). */
    double t0 = now_ms();
    float maxrel = st_grad_check(&m, buf, 12, 0, 1e-2f);
    double t1 = now_ms();
    printf("  n_params=%d  max_rel_err=%.5f  (%.0f ms)\n",
           m.n_params, maxrel, t1 - t0);
    /* PASS bar: 5e-2. float32 central differences over a 4-layer net with
     * transcendentals (exp/silu/rmsnorm) carry ~1-3%% truncation+roundoff even
     * at the best eps; we report the real number, never inflate the bar. */
    int ok = (maxrel < 5e-2f);
    CHECK(ok, "[baby-gradcheck] analytic gradients match finite differences");
    st_free(&m);
    return ok ? 0 : 1;
}

/* ====================================================================== */
/* training + eval helpers                                                */
/* ====================================================================== */

/* mean held-out loss over `count` windows starting after `train_end`. */
static float heldout_loss(st_model *m, int seqlen, int train_end, int count)
{
    uint8_t buf[ST_MAXSEQ];
    double sum = 0.0; int got = 0;
    for (int w = 0; w < count; w++) {
        int off = train_end + w * seqlen;
        window(buf, off, seqlen);
        int np = 0;
        float l = st_eval_loss(m, buf, seqlen, &np);
        if (np) { sum += l; got++; }
    }
    return got ? (float)(sum / got) : 0.0f;
}

/* run `rounds` sleep rounds over the first `train_windows` windows.
 *
 * scramble==0 : REAL teaching — train on the teacher's actual byte windows.
 * scramble==1 : the GROUNDED CONTROL — every training window is replaced by
 *   UNIFORM-RANDOM bytes (native-student.md §B.6: "shuffle the teacher targets
 *   / use random bytes"). Identical #updates, identical optimiser, identical
 *   data volume — but the next-byte mapping is unlearnable, so held-out loss on
 *   the REAL corpus cannot drop below chance. This is what isolates "real
 *   teaching" from "any training nudges a metric". */
static void distill(st_model *m, int seqlen, int train_windows, int rounds,
                    float lr, int scramble)
{
    uint8_t buf[ST_MAXSEQ];
    uint32_t rng = 0x5EED1234u;
    float *logits = (float *)malloc((size_t)seqlen * ST_VOCAB * sizeof(float));
    for (int r = 0; r < rounds; r++) {
        for (int w = 0; w < train_windows; w++) {
            if (scramble) {
                for (int i = 0; i < seqlen; i++) {
                    rng = rng * 1664525u + 1013904223u;
                    buf[i] = (uint8_t)((rng >> 16) & 0xff);  /* uniform 0..255 */
                }
            } else {
                window(buf, w * seqlen, seqlen);
            }
            st_zero_grad(m);
            st_forward(m, buf, seqlen, logits);
            st_backward(m, buf, seqlen);
            st_adam_step(m, lr);
        }
    }
    free(logits);
}

/* ====================================================================== */
/* [honest-baby] + [distill-loss-drops] + [distill-grounded]              */
/* ====================================================================== */
static int cert_distill(void)
{
    const int seqlen = 32;
    const int rounds = 30;
    const float lr = 3e-3f;
    /* split corpus into train / held-out windows */
    int total_windows = g_corpus_n / seqlen;
    if (total_windows < 4) total_windows = 4;
    int train_windows = total_windows * 3 / 4; if (train_windows < 2) train_windows = 2;
    int held_windows  = total_windows - train_windows; if (held_windows < 1) held_windows = 1;
    int train_end = train_windows * seqlen;

    float chance = 0.0f; /* ln(256) */
    { /* compute ln(256) via the baby's own libc-free log for an apples-apples ref */
      chance = st_logf(256.0f);
    }

    printf("\n[honest-baby] + [distill-loss-drops] + [distill-grounded]\n");
    printf("  corpus: %s, %d bytes; seqlen=%d, train=%d win, held=%d win, "
           "rounds=%d, lr=%.4f\n",
           g_corpus_src, g_corpus_n, seqlen, train_windows, held_windows, rounds, lr);
    printf("  chance next-byte loss ln(256) = %.4f nats\n", chance);

    /* ---- real-teaching model ---- */
    st_model m;
    st_init(&m, 0x0BABE);
    float pre = heldout_loss(&m, seqlen, train_end, held_windows);
    printf("  [honest-baby] pre-distill held-out loss = %.4f nats "
           "(chance %.4f)\n", pre, chance);
    /* honest-baby PASS: the fresh baby is within a small margin of chance,
     * i.e. it knows essentially nothing. */
    int honest_ok = (pre > chance - 0.30f);
    CHECK(honest_ok, "[honest-baby] fresh baby starts near-random");

    double t0 = now_ms();
    distill(&m, seqlen, train_windows, rounds, lr, 0 /*real*/);
    double t1 = now_ms();
    float post = heldout_loss(&m, seqlen, train_end, held_windows);
    printf("  [distill-loss-drops] post-distill held-out loss = %.4f nats "
           "(%.0f ms for %d rounds x %d win)\n",
           post, t1 - t0, rounds, train_windows);
    printf("  delta = %.4f nats (%.1f%% of chance)\n",
           pre - post, 100.0f * (pre - post) / chance);
    int drop_ok = (post < pre - 0.20f);
    CHECK(drop_ok, "[distill-loss-drops] held-out loss measurably drops");
    st_free(&m);

    /* ---- grounded control: random-byte targets (same effort, no signal) ---- */
    st_model ms;
    st_init(&ms, 0x0BABE);   /* same seed -> same starting point */
    float spre = heldout_loss(&ms, seqlen, train_end, held_windows);
    distill(&ms, seqlen, train_windows, rounds, lr, 1 /*random bytes*/);
    float spost = heldout_loss(&ms, seqlen, train_end, held_windows);
    printf("  [distill-grounded] random-byte control: held-out %.4f -> %.4f "
           "(delta %.4f)\n", spre, spost, spre - spost);
    /* grounded PASS: training on random bytes gains far less than real teaching
     * AND the held-out loss on the REAL corpus stays near chance (the model
     * learned nothing transferable). Real drop must be >= 2x the control drop. */
    float real_drop = pre - post;
    float scr_drop  = spre - spost;
    int grounded_ok = (scr_drop < real_drop * 0.5f) && (spost > chance - 0.30f);
    printf("  real_drop=%.4f  control_drop=%.4f  (grounded needs control < "
           "0.5x real AND control held-out stays near chance %.2f)\n",
           real_drop, scr_drop, chance);
    CHECK(grounded_ok, "[distill-grounded] gain vanishes under random-byte teacher");
    st_free(&ms);

    return (honest_ok && drop_ok && grounded_ok) ? 0 : 1;
}

/* ====================================================================== */
int main(int argc, char **argv)
{
    const char *fixture = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) fixture = argv[++i];
    }
    /* default fixture location (written by harvest mode); silently fall back */
    if (!fixture) fixture = "tests/llm/student_teacher.bytes";

    load_corpus(fixture);

    printf("=== NS-1 Cradle baby cert ===\n");
    printf("arch: vocab=%d E=%d topk=%d d_model=%d layers=%d dff=%d\n",
           ST_VOCAB, ST_NEXPERT, ST_TOPK, ST_DMODEL, ST_NLAYER, ST_DFF);

    int rc = 0;
    rc |= cert_gradcheck();
    rc |= cert_distill();

    printf("\nSUMMARY: %d PASS, %d FAIL\n", g_pass, g_fail);
    free(g_corpus);
    return (g_fail == 0 && rc == 0) ? 0 : 1;
}
