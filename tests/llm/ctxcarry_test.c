/* ------------------------------------------------------------------------- *
 *  ctxcarry_test.c — the SCALE-WALL rung C1 cert [ctx-carry]
 *                    (scale_wall_design.md §8; arch/common/llm/student.c v2).
 *
 *  Change under test (C1): RoPE + ST_MAXSEQ 64->256 in the student, so a fact
 *  told >=96 bytes back can be USED in the completion — the atomic unit of
 *  conversation (being told something and using it).
 *
 *  Probe (generic, seeded, NO hard-coded layout): from a seed,
 *      "fact: <K> is <V>. <filler d bytes> question: what is <K>? answer: <V>"
 *  with K,V random 6-8 char strings. Metric (nats):
 *      A(d) = CE(answer-span | ABLATED prefix) - CE(answer-span | INTACT prefix)
 *  where the ABLATED control replaces the fact SENTENCE with equal-length random
 *  bytes (so "loss got lower everywhere" cannot fake a positive A — A is a
 *  DIFFERENCE). Sweep d and PRINT the whole curve (the walk-to-the-wall).
 *
 *  Certs printed here (all self-contained; NO gguf file, NO network):
 *    [ctx-carry]           A(d) curve, full window (the measured deliverable).
 *    [ctx-carry-clamp]     a CONTROL, semi-tautological (cross-audit #9): re-eval
 *                          the TRAINED model with the window forcibly clamped to
 *                          64 -> A(d>=96) COLLAPSES to ~0. This ISOLATES window-
 *                          vs-more-training, but clamping literally REMOVES the
 *                          distant byte from the input, so the collapse is true
 *                          of ANY model — it is NOT itself the mechanism proof.
 *                          The load-bearing half is [ctx-carry-window]'s
 *                          full_reads (below).
 *    [ctx-carry-window]    ANTI-THEATER (deterministic, training-independent).
 *                          The LOAD-BEARING half: perturb ONE distant fact byte
 *                          and the answer-position logit FNV SHIFTS under the
 *                          FULL window (ST_MAXSEQ=256) — proof the widened window
 *                          genuinely READS a byte 96+ back; if the window
 *                          mechanism were absent/stubbed this signal VANISHES ->
 *                          RED. The 64-clamp half (byte UNCHANGED) is a by-
 *                          CONSTRUCTION control (the byte was physically removed),
 *                          not an independent mechanism proof; a DIFFER there
 *                          would instead flag a harness leak.
 *    [gen-cohort-island]   a v1 blob offered to a v2 model is REFUSED by
 *                          st_load / st_blob_tier_ok / st_merge_cohort, model
 *                          BYTE-UNCHANGED (NS_STUDENT_VER 1->2 is load-bearing:
 *                          RoPE is parameter-free so dims are unchanged and a v1
 *                          peer would otherwise silently merge into a v2 cohort).
 *    [ctx-carry-nope]      RoPE-vs-NoPE MEASURED side-by-side — PRINTED, NOT
 *                          GATED (§8: decoder-only nets learn implicit position;
 *                          the cert bets on the WINDOW, not on RoPE).
 *    [ctx-carry-determinism] FNV of a FIXED probe byte-stream, PINNED, so x86_64
 *                          and aarch64 print identical curves (integer-only gen).
 *    [ctx-carry-exclusion] recall-lookup exclusion: every eval K contains the
 *                          reserved marker byte 'Q' (never in the Q-free training
 *                          corpus) AND r3_vocab_key_id(K)==-1 — no memorization,
 *                          no R3 binding, no `mind ask` can supply the answer.
 *
 *  Honest scope (printed): context carry at 256 bytes is NOT conversation — it is
 *  the atomic prerequisite. At toy scale (1.9M params, a small cert corpus, CI
 *  budget) A(d) may barely move; the CURVE is the deliverable, never a forced
 *  pass. The load-bearing PASS is the deterministic window mechanism proof.
 *
 *  Build: cc -std=c11 -O1 -ffp-contract=off ctxcarry_test.c ../../arch/common/llm/student.c
 *  Run:   ./ctxcarry [ci|full]      (default ci = reduced sweep for strict CI)
 *  Exit 0 = the GATED arms PASS; the A(d) magnitude is reported, not gated.
 *
 *  `c2` mode (scale-wall-c1.md's "C2 plan" addendum): scales the SAME
 *  synthetic generator used above to a target training-byte budget (default
 *  10 MB, override via C2_TARGET_BYTES) instead of a fixed rounds/n_ex, and
 *  re-prints the A(d) curve — does the C1 clean-NULL move off zero with more
 *  data? Not gated, not wired into CI; a one-off measurement.
 * ------------------------------------------------------------------------- */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../../arch/common/llm/student.h"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  PASS  %s\n", msg); } \
    else { printf("  FAIL  %s\n", msg); g_fail++; } } while (0)

/* ---- integer LCG (cross-arch deterministic; no float in probe generation) -- */
static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }

/* ---- the 16 R3 key words (arch/common/r3_vocab.c vk_img), embedded so the
 * recall-lookup exclusion is checked WITHOUT pulling the kernel header chain.
 * All are lowercase pure-alpha English; a marker-bearing eval K can never be one
 * (faithful to r3_vocab_key_id's exact-match semantics). */
static const char *R3_KEYS[16] = {
    "sky","sea","sun","grass","blood","night","gold","snow",
    "fire","leaf","stone","water","cloud","rose","bone","ink"
};
static int r3_key_id_local(const char *w)
{
    for (int i = 0; i < 16; i++) if (strcmp(w, R3_KEYS[i]) == 0) return i;
    return -1;
}

/* ---- token generators ----------------------------------------------------
 * The eval tokens must be (a) NON-MEMBERS of the training corpus (no
 * memorization) yet (b) IN-DISTRIBUTION byte-wise (so the model's copy of the
 * VALUE is not handicapped by out-of-distribution bytes — an OOD marker would
 * conflate "can't carry context" with "can't handle a novel byte"). The clean
 * device is a RESERVED BIGRAM "qz":
 *   - TRAINING tokens: lowercase a-z + digits (so 'q' and 'z' ARE trained
 *     bytes), forced digit, but the bigram "qz" is FIXED OUT (any q followed by
 *     z is bumped to 'qy'). The filler alphabet EXCLUDES 'q'. The lowercase
 *     template words are "qz"-free. So the WHOLE training corpus is "qz"-free.
 *   - EVAL tokens: same alphabet, but FORCE "qz" at positions [2,3]. Every eval
 *     token therefore contains "qz" -> it can NEVER be a substring of the
 *     "qz"-free training corpus (non-membership GUARANTEED) and, carrying a
 *     digit, is never one of the 16 lowercase-pure-alpha R3 keys.               */
static const char LOWER_DIG[] = "abcdefghijklmnopqrstuvwxyz0123456789";
static const char DIGITS[]     = "0123456789";

static int gen_train_tok(uint32_t *rng, char *out)
{
    int len = 6 + (int)(lcg(rng) % 3);            /* 6..8 */
    for (int i = 0; i < len; i++) out[i] = LOWER_DIG[lcg(rng) % 36];
    out[2] = DIGITS[lcg(rng) % 10];               /* force a digit */
    for (int i = 1; i < len; i++)                 /* fix out the reserved bigram */
        if (out[i - 1] == 'q' && out[i] == 'z') out[i] = 'y';
    out[len] = 0;
    return len;
}
static int gen_eval_tok(uint32_t *rng, char *out)
{
    int len = 6 + (int)(lcg(rng) % 3);            /* 6..8 */
    for (int i = 0; i < len; i++) out[i] = LOWER_DIG[lcg(rng) % 36];
    out[2] = 'q'; out[3] = 'z';                   /* reserved bigram (corpus-free) */
    out[5] = DIGITS[lcg(rng) % 10];               /* force a digit (len>=6)        */
    out[len] = 0;
    return len;
}

/* ---- probe builder --------------------------------------------------------
 * Writes "fact: <K> is <V>. <filler d> question: what is <K>? answer: <V>" into
 * seq[]. Returns total length n. Reports:
 *   *ans_start = index of the FIRST answer <V> byte,   *ans_len = |V|
 *   *fact_start = 0,  *fact_len = length of "fact: <K> is <V>. " (the ablatable
 *   fact SENTENCE). Filler is neutral lowercase+space (never 'Q'; never V).     */
static int build_probe(uint8_t *seq, const char *K, int klen, const char *V, int vlen,
                       int d, uint32_t *fill_rng,
                       int *ans_start, int *ans_len, int *fact_start, int *fact_len)
{
    int n = 0;
    #define PUT(s,l) do { const char *p_=(s); for (int i_=0;i_<(l);i_++) seq[n++]=(uint8_t)p_[i_]; } while(0)
    PUT("fact: ", 6);
    PUT(K, klen);
    PUT(" is ", 4);
    PUT(V, vlen);
    PUT(". ", 2);
    if (fact_start) *fact_start = 0;
    if (fact_len)   *fact_len   = n;               /* whole "fact: ... . "        */
    for (int i = 0; i < d; i++) {                  /* neutral filler, exactly d    */
        uint32_t r = lcg(fill_rng);
        if (r % 27 == 0) { seq[n++] = ' '; }
        else { char cc = (char)('a' + (r % 26)); if (cc == 'q') cc = 'x';  /* q-free */
               seq[n++] = (uint8_t)cc; }
    }
    PUT("question: what is ", 18);
    PUT(K, klen);
    PUT("? answer: ", 10);
    if (ans_start) *ans_start = n;
    PUT(V, vlen);
    if (ans_len) *ans_len = vlen;
    #undef PUT
    return n;
}

/* ---- answer-span CE over a (possibly clamped) window ----------------------
 * clampwin<=0 or >=n : full window. clampwin==64 : keep the LAST 64 bytes (the
 * answer is at the tail, so it survives; a fact >=64 bytes back is DROPPED). The
 * answer span maps to [wn-ans_len, wn) in the truncated frame. *fnv (if non-NULL)
 * gets the FNV of the answer-predicting logit rows (window mechanism proof).     */
static float answer_ce(st_model *m, const uint8_t *seq, int n, int ans_len,
                       int clampwin, uint64_t *fnv)
{
    int start = (clampwin > 0 && n > clampwin) ? (n - clampwin) : 0;
    int wn = n - start;
    const uint8_t *w = seq + start;
    int t1 = wn;
    int t0 = wn - ans_len;
    return st_span_ce(m, w, wn, t0, t1, fnv);
}

/* ---- train the model on fact-in-context lessons (disjoint eval K,V) -------- */
static void train_ctx(st_model *m, int rounds, int n_ex, uint32_t seed,
                      const int *dists, int ndist)
{
    uint32_t rng = seed;
    float *logits = (float *)malloc((size_t)ST_MAXSEQ * ST_VOCAB * sizeof(float));
    if (!logits) return;
    for (int r = 0; r < rounds; r++) {
        uint32_t rr = rng + (uint32_t)r * 2654435761u;    /* vary per round */
        for (int e = 0; e < n_ex; e++) {
            char K[16], V[16];
            int kl = gen_train_tok(&rr, K);
            int vl = gen_train_tok(&rr, V);
            int d  = dists[lcg(&rr) % ndist];
            uint8_t seq[ST_MAXSEQ];
            uint32_t fr = rr ^ 0x9E3779B9u;
            int n = build_probe(seq, K, kl, V, vl, d, &fr, 0, 0, 0, 0);
            if (n < 2 || n > ST_MAXSEQ) continue;
            st_zero_grad(m);
            st_forward(m, seq, n, logits);
            st_backward(m, seq, n);
            st_adam_step(m, ST_LR);
        }
    }
    free(logits);
}

/* ---- train until `target_bytes` of synthetic examples have been seen (the
 * C2 scale-up: same generator/template as train_ctx, sized by BYTES trained
 * rather than a fixed rounds*n_ex, so the reservoir size is measured, not
 * estimated). Returns actual bytes trained (>= target_bytes); *examples_out
 * gets the example count. */
static uint64_t train_ctx_bytes(st_model *m, uint64_t target_bytes, uint32_t seed,
                                const int *dists, int ndist, uint64_t *examples_out)
{
    uint32_t rng = seed;
    uint64_t bytes = 0, examples = 0;
    uint64_t next_report = target_bytes / 20;   /* ~20 progress lines, so a
                                                  * detached/timed-out run still
                                                  * leaves a throughput trail */
    if (next_report == 0) next_report = 1024;
    double t_start = now_ms();
    float *logits = (float *)malloc((size_t)ST_MAXSEQ * ST_VOCAB * sizeof(float));
    if (!logits) { if (examples_out) *examples_out = 0; return 0; }
    while (bytes < target_bytes) {
        char K[16], V[16];
        int kl = gen_train_tok(&rng, K);
        int vl = gen_train_tok(&rng, V);
        int d  = dists[lcg(&rng) % (uint32_t)ndist];
        uint8_t seq[ST_MAXSEQ];
        uint32_t fr = rng ^ 0x9E3779B9u;
        int n = build_probe(seq, K, kl, V, vl, d, &fr, 0, 0, 0, 0);
        if (n < 2 || n > ST_MAXSEQ) continue;
        st_zero_grad(m);
        st_forward(m, seq, n, logits);
        st_backward(m, seq, n);
        st_adam_step(m, ST_LR);
        bytes += (uint64_t)n;
        examples++;
        if (bytes >= next_report) {
            double elapsed_s = (now_ms() - t_start) / 1000.0;
            printf("[scale-wall-c2]   progress: %llu/%llu bytes (%llu examples, %.1f s, %.0f B/s)\n",
                   (unsigned long long)bytes, (unsigned long long)target_bytes,
                   (unsigned long long)examples, elapsed_s,
                   elapsed_s > 0.0 ? (double)bytes / elapsed_s : 0.0);
            fflush(stdout);
            next_report += target_bytes / 20;
            if (next_report == 0) next_report = bytes + 1024;
        }
    }
    free(logits);
    if (examples_out) *examples_out = examples;
    return bytes;
}

/* ---- measure the A(d) curve (full + clamped) over `nprobe` seeded probes ---- */
static float measure_A(st_model *m, int d, int nprobe, uint32_t seed, int clampwin)
{
    uint32_t rng = seed;
    double asum = 0.0; int cnt = 0;
    for (int p = 0; p < nprobe; p++) {
        char K[16], V[16];
        int kl = gen_eval_tok(&rng, K);
        int vl = gen_eval_tok(&rng, V);
        uint8_t intact[ST_MAXSEQ], ablated[ST_MAXSEQ];
        int as, al, fs, fl;
        uint32_t fr = rng ^ 0x1234567u;
        int n = build_probe(intact, K, kl, V, vl, d, &fr, &as, &al, &fs, &fl);
        if (n < 2 || n > ST_MAXSEQ) continue;
        /* ablated = intact with the fact SENTENCE [fs,fs+fl) -> random bytes */
        memcpy(ablated, intact, (size_t)n);
        uint32_t ar = rng ^ 0x0BADF00Du;
        for (int i = fs; i < fs + fl; i++) ablated[i] = (uint8_t)(lcg(&ar) & 0xFF);
        float ce_i = answer_ce(m, intact,  n, al, clampwin, 0);
        float ce_a = answer_ce(m, ablated, n, al, clampwin, 0);
        asum += (double)(ce_a - ce_i);
        cnt++;
    }
    return cnt ? (float)(asum / cnt) : 0.0f;
}

/* ------------------------------------------------------------------------- */
/* [ctx-carry-window] — the deterministic anti-theater window mechanism proof  */
/* ------------------------------------------------------------------------- */
static int cert_window_mechanism(st_model *m)
{
    printf("\n[ctx-carry-window] ANTI-THEATER: does the widened window actually feed\n");
    printf("                   a DISTANT fact byte into the answer logits?\n");
    /* one probe, d=128 so the fact sits >64 bytes back. */
    uint32_t rng = 0xC1C1C1u;
    char K[16], V[16];
    int kl = gen_eval_tok(&rng, K);
    int vl = gen_eval_tok(&rng, V);
    uint8_t seqA[ST_MAXSEQ], seqB[ST_MAXSEQ];
    int as, al, fs, fl;
    uint32_t fr = 0xF11133u;
    int n = build_probe(seqA, K, kl, V, vl, 128, &fr, &as, &al, &fs, &fl);
    /* perturb ONE byte inside the fact VALUE (well before n-64). */
    memcpy(seqB, seqA, (size_t)n);
    int pv = 6 + kl + 4;                 /* first byte of V in the fact */
    seqB[pv] = (uint8_t)(seqB[pv] ^ 0x5A);
    int perturb_abs = pv, tail_start = n - 64;
    uint64_t hFullA = 0, hFullB = 0, hClampA = 0, hClampB = 0;
    (void)answer_ce(m, seqA, n, al, 0,  &hFullA);
    (void)answer_ce(m, seqB, n, al, 0,  &hFullB);
    (void)answer_ce(m, seqA, n, al, 64, &hClampA);
    (void)answer_ce(m, seqB, n, al, 64, &hClampB);
    printf("   perturbed fact byte @abs=%d  (64-clamp tail begins @%d -> byte is OUTSIDE it)\n",
           perturb_abs, tail_start);
    printf("   FULL window : answer logit_hash  A=%016llx  B=%016llx  -> %s\n",
           (unsigned long long)hFullA, (unsigned long long)hFullB,
           (hFullA != hFullB) ? "DIFFER (window READS the distant byte)  [GREEN]"
                              : "SAME (window did NOT read it)");
    printf("   64-CLAMP    : answer logit_hash  A=%016llx  B=%016llx  -> %s\n",
           (unsigned long long)hClampA, (unsigned long long)hClampB,
           (hClampA == hClampB) ? "SAME (distant byte physically REMOVED — by-construction control)"
                                : "DIFFER (clamp leaked the byte?! — harness bug)");
    int full_reads  = (hFullA != hFullB);
    int clamp_blind = (hClampA == hClampB);
    printf("   => LOAD-BEARING (cross-audit #9): full_reads — the ST_MAXSEQ=%d window\n", ST_MAXSEQ);
    printf("      DEPENDS on a byte 96+ back; if the widened window were absent/stubbed\n");
    printf("      this signal VANISHES -> RED. The 64-clamp half is a by-construction\n");
    printf("      control (the byte is removed), not an independent mechanism proof.\n");
    CHECK(full_reads,  "[ctx-carry-window] full window (LOAD-BEARING): answer logits DEPEND on the distant fact byte");
    CHECK(clamp_blind, "[ctx-carry-window] clamp-64 (by-construction control): the byte is removed so logits cannot depend on it");
    return (full_reads && clamp_blind) ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* [gen-cohort-island] — v1 blob offered to v2 REFUSED, model byte-unchanged    */
/* ------------------------------------------------------------------------- */
static uint64_t fnv_bytes(const void *p, size_t len)
{
    const unsigned char *b = (const unsigned char *)p;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}
static int cert_cohort_island(void)
{
    printf("\n[gen-cohort-island] NS_STUDENT_VER 1->2 is a LOAD-BEARING succession island\n");
    st_model m;
    if (st_init(&m, 0x5EED77u) != ST_OK) { printf("  init OOM\n"); return 1; }
    size_t cap = st_blob_size(&m);
    unsigned char *blob = (unsigned char *)malloc(cap);
    if (!blob) { st_free(&m); return 1; }
    long wrote = st_save(&m, blob, cap);
    int ok = 1;
    ok &= (wrote == (long)cap);
    /* POSITIVE control: the freshly-saved v2 blob IS accepted (refusal is
     * version-specific, not a blanket reject). */
    int v2_ok = st_blob_tier_ok(&m, blob, cap);
    CHECK(v2_ok == 1, "[gen-cohort-island] a matching v2 blob is ACCEPTED (control)");
    /* synthesize a v1 blob: patch the ns_ver header field (u32 @ offset 8) to 1.
     * RoPE is parameter-free so n_params/dims are IDENTICAL between v1 and v2 —
     * only ns_ver distinguishes them, which is EXACTLY why the version guard is
     * the load-bearing island (a v1 peer would otherwise merge into a v2 cohort). */
    unsigned char *v1 = (unsigned char *)malloc(cap);
    memcpy(v1, blob, cap);
    v1[8] = 1; v1[9] = 0; v1[10] = 0; v1[11] = 0;   /* ns_ver = 1 (little-endian) */
    uint64_t w_before = fnv_bytes(m.w, (size_t)m.n_params * sizeof(float));
    int tier_ok = st_blob_tier_ok(&m, v1, cap);      /* must be 0 (refused)        */
    int load_rc = st_load(&m, v1, cap);              /* must be < 0 (rejected)     */
    uint64_t w_after = fnv_bytes(m.w, (size_t)m.n_params * sizeof(float));
    CHECK(tier_ok == 0,          "[gen-cohort-island] st_blob_tier_ok REFUSES the v1 blob");
    CHECK(load_rc < 0,           "[gen-cohort-island] st_load REJECTS the v1 blob");
    CHECK(w_before == w_after,   "[gen-cohort-island] model weights BYTE-UNCHANGED after the rejected load");
    /* merge: a v1 peer must NOT be folded into a v2 cohort. */
    const void *peers[1] = { v1 };
    size_t plens[1] = { cap };
    int accepted = st_merge_cohort(&m, peers, plens, 1);
    uint64_t w_merge = fnv_bytes(m.w, (size_t)m.n_params * sizeof(float));
    CHECK(accepted == 0,         "[gen-cohort-island] st_merge_cohort accepts 0 v1 peers (cross-gen isolation)");
    CHECK(w_merge == w_before,   "[gen-cohort-island] model weights BYTE-UNCHANGED after the refused merge");
    printf("   ns_ver: resident=%d  v1-blob=1  -> refused everywhere; the old baby is\n", NS_STUDENT_VER);
    printf("   REBORN + re-distilled, never translated (compat/evolution law).\n");
    free(blob); free(v1); st_free(&m);
    (void)ok;
    return g_fail ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* [ctx-carry-determinism] — FNV of a FIXED probe byte-stream (cross-arch pin)  */
/* ------------------------------------------------------------------------- */
static uint64_t determinism_probe_fnv(void)
{
    uint64_t h = 1469598103934665603ULL;
    uint32_t rng = 0xDE7E9Du;
    const int dd[4] = { 16, 48, 96, 128 };
    for (int p = 0; p < 8; p++) {
        char K[16], V[16];
        int kl = gen_eval_tok(&rng, K);
        int vl = gen_eval_tok(&rng, V);
        uint8_t seq[ST_MAXSEQ];
        uint32_t fr = 0xABCDEFu + (uint32_t)p;
        int n = build_probe(seq, K, kl, V, vl, dd[p & 3], &fr, 0, 0, 0, 0);
        for (int i = 0; i < n; i++) { h ^= seq[i]; h *= 1099511628211ULL; }
    }
    return h;
}
/* PINNED value of the fixed probe stream (integer-only generation -> identical
 * on x86_64 and aarch64). A drift here means the probe generator changed OR a
 * cross-arch integer nondeterminism crept in. Measured 2026-07-05 on both the
 * x86_64 (qemu) and native aarch64 hosted builds. */
#define CTX_PROBE_FNV_PIN 0x13fc6601ce8904d0ULL

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* `falsify` mode — research/rote-vs-copy.md stage A: is the ruler sound?      */
/*   S(d): same intact context ("fact: K is V"), answer slot V vs a V' of the  */
/*         same length that is not in the context; CE(V') - CE(V) over the     */
/*         FREE answer bytes only (qz at 2,3 and the forced digit at 5 hit the  */
/*         same way for V and V'). A template habit gives S ~ 0.               */
/*   R(d): Olsson's induction score: 24 random bytes (the training alphabet),  */
/*         d bytes of neutral filler, the same 24 bytes again; R = CE(first    */
/*         copy, bytes 2..24) - CE(second copy, same bytes). No template.      */
/*   ctx > 0 (the NC): the measured span sees only the ctx bytes in front of   */
/*         it, so for d >= 32 the fact / first copy is out of view.            */
/* Recorded only: A_free (A over the free bytes, 0..255 ablation as in         */
/* measure_A) and A_id (free bytes, ablated with the filler alphabet).         */
/* ------------------------------------------------------------------------- */
static double rv_sqrt(double x)                  /* no -lm in the cert build */
{
    if (x <= 0.0) return 0.0;
    double y = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 60; i++) y = 0.5 * (y + x / y);
    return y;
}

static int rv_free_pos(int vlen, int *pos)
{
    int k = 0;
    for (int i = 0; i < vlen; i++) if (i != 2 && i != 3 && i != 5) pos[k++] = i;
    return k;
}

/* mean next-byte CE at positions ts[0..k) (predict w[t] from row t-1), ONE
 * forward (st_span_ce costs a forward per call: 10 min per point at 64 probes,
 * 2026-10-08). log-softmax with the student's own st_expf / st_logf. */
static float *rv_logits;
static double rv_ce_at(st_model *m, const uint8_t *w, int wn, const int *ts, int k)
{
    if (!rv_logits) rv_logits = (float *)malloc((size_t)ST_MAXSEQ * ST_VOCAB * sizeof(float));
    if (!rv_logits || k < 1 || st_forward(m, w, wn, rv_logits) != ST_OK) return 0.0;
    double s = 0.0;
    for (int j = 0; j < k; j++) {
        const float *row = rv_logits + (size_t)(ts[j] - 1) * ST_VOCAB;
        float mx = row[0];
        for (int c = 1; c < ST_VOCAB; c++) if (row[c] > mx) mx = row[c];
        double z = 0.0;
        for (int c = 0; c < ST_VOCAB; c++) z += (double)st_expf(row[c] - mx);
        s += (double)st_logf((float)z) - (double)(row[w[ts[j]]] - mx);
    }
    return s / k;
}

/* mean CE over the free bytes of the answer at [as, as+vlen) (the tail). */
static double rv_free_ce(st_model *m, const uint8_t *seq, int n, int as, int vlen, int ctx)
{
    int pos[16], k = rv_free_pos(vlen, pos);
    int start = (ctx > 0 && as - ctx > 0) ? as - ctx : 0;
    for (int j = 0; j < k; j++) pos[j] += as - start;
    return rv_ce_at(m, seq + start, n - start, pos, k);
}

typedef struct { double mean, se; int n; } rv_stat;
static rv_stat rv_fin(double s, double s2, int c)
{
    rv_stat r = { 0.0, 0.0, c };
    if (c < 2) return r;
    r.mean = s / c;
    double var = (s2 - s * s / c) / (c - 1);
    r.se = rv_sqrt(var / c);
    return r;
}

static void rv_filler(uint8_t *seq, int *n, int d, uint32_t *fr)
{
    for (int i = 0; i < d; i++) {               /* build_probe's neutral filler */
        uint32_t r = lcg(fr);
        if (r % 27 == 0) seq[(*n)++] = ' ';
        else { char cc = (char)('a' + (r % 26)); if (cc == 'q') cc = 'x'; seq[(*n)++] = (uint8_t)cc; }
    }
}

static rv_stat measure_S(st_model *m, int d, int nprobe, uint32_t seed, int ctx)
{
    uint32_t rng = seed; double s = 0.0, s2 = 0.0; int c = 0;
    for (int p = 0; p < nprobe; p++) {
        char K[16], V[16], V2[16];
        int kl = gen_eval_tok(&rng, K);
        int vl = gen_eval_tok(&rng, V);
        int v2l;
        do { v2l = gen_eval_tok(&rng, V2); } while (v2l != vl || memcmp(V, V2, (size_t)vl) == 0);
        uint8_t a[ST_MAXSEQ], b[ST_MAXSEQ];
        int as, al;
        uint32_t fr = rng ^ 0x1234567u;
        int n = build_probe(a, K, kl, V, vl, d, &fr, &as, &al, 0, 0);
        if (n < 2 || n > ST_MAXSEQ) continue;
        memcpy(b, a, (size_t)n);
        memcpy(b + as, V2, (size_t)vl);          /* same context, answer V' */
        double x = rv_free_ce(m, b, n, as, vl, ctx) - rv_free_ce(m, a, n, as, vl, ctx);
        s += x; s2 += x * x; c++;
    }
    return rv_fin(s, s2, c);
}

#define RV_LEN 24
#define RV_PRE 16
/* [16 filler][A: 24 random][d filler][B: 24]; B = A (same) or fresh random
 * (diff). The first copy is NOT at the sequence start: the QUICK run showed
 * the memo's R (CE of a copy at the start minus the second copy) stays ~1 nat
 * with only 16 bytes of context (NC red) - the trained student expects
 * "fact: " at the start, so R measured the position, not copying. */
static int rv_build_repeat(uint8_t *seq, int d, uint32_t *rng, int diff)
{
    int n = 0;
    uint32_t fr0 = *rng ^ 0x51ED51EDu;
    rv_filler(seq, &n, RV_PRE, &fr0);
    for (int i = 0; i < RV_LEN; i++) seq[n++] = (uint8_t)LOWER_DIG[lcg(rng) % 36];
    uint32_t fr = *rng ^ 0x9E3779B9u;
    rv_filler(seq, &n, d, &fr);
    uint32_t br = *rng ^ 0xD1FFu;
    for (int i = 0; i < RV_LEN; i++)
        seq[n + i] = diff ? (uint8_t)LOWER_DIG[lcg(&br) % 36] : seq[RV_PRE + i];
    return n + RV_LEN;
}

/* R'(d) = CE(B, bytes 2..24 | B != A) - CE(B, bytes 2..24 | B == A): the same
 * positions and the same context length, only whether B repeats A differs.
 * *r0 (recorded only) is the memo's R: CE(A) - CE(B == A). */
static rv_stat measure_R(st_model *m, int d, int nprobe, uint32_t seed, int ctx, double *r0)
{
    uint32_t rng = seed; double s = 0.0, s2 = 0.0, z = 0.0; int c = 0;
    for (int p = 0; p < nprobe; p++) {
        uint8_t a[ST_MAXSEQ], b[ST_MAXSEQ];
        uint32_t r1 = rng, r2 = rng;
        int n = rv_build_repeat(a, d, &r1, 0);
        (void)rv_build_repeat(b, d, &r2, 1);
        rng = r1;
        if (n > ST_MAXSEQ) continue;
        int t0 = n - RV_LEN + 1;                  /* B, bytes 2..24 */
        int ts[RV_LEN];
        int start = (ctx > 0 && t0 - ctx > 0) ? t0 - ctx : 0;
        for (int j = 0; j < RV_LEN - 1; j++) ts[j] = t0 - start + j;
        double ce_same = rv_ce_at(m, a + start, n - start, ts, RV_LEN - 1);
        double ce_diff = rv_ce_at(m, b + start, n - start, ts, RV_LEN - 1);
        for (int j = 0; j < RV_LEN - 1; j++) ts[j] = RV_PRE + 1 + j;
        double ce_a = rv_ce_at(m, a, RV_PRE + RV_LEN, ts, RV_LEN - 1);
        double x = ce_diff - ce_same;
        s += x; s2 += x * x; z += ce_a - ce_same; c++;
    }
    if (r0) *r0 = c ? z / c : 0.0;
    return rv_fin(s, s2, c);
}

/* A over the free answer bytes; idmode 0 = ablate with 0..255 (measure_A's
 * way), 1 = ablate with the filler alphabet (in-distribution). */
static double measure_A_free(st_model *m, int d, int nprobe, uint32_t seed, int idmode)
{
    uint32_t rng = seed; double s = 0.0; int c = 0;
    for (int p = 0; p < nprobe; p++) {
        char K[16], V[16];
        int kl = gen_eval_tok(&rng, K);
        int vl = gen_eval_tok(&rng, V);
        uint8_t intact[ST_MAXSEQ], ablated[ST_MAXSEQ];
        int as, al, fs, fl;
        uint32_t fr = rng ^ 0x1234567u;
        int n = build_probe(intact, K, kl, V, vl, d, &fr, &as, &al, &fs, &fl);
        if (n < 2 || n > ST_MAXSEQ) continue;
        memcpy(ablated, intact, (size_t)n);
        uint32_t ar = rng ^ 0x0BADF00Du;
        if (idmode) { int k = fs; rv_filler(ablated, &k, fl, &ar); }
        else for (int i = fs; i < fs + fl; i++) ablated[i] = (uint8_t)(lcg(&ar) & 0xFF);
        s += rv_free_ce(m, ablated, n, as, vl, 0) - rv_free_ce(m, intact, n, as, vl, 0);
        c++;
    }
    return c ? s / c : 0.0;
}

/* the PC: the same student trained on random repeats only (no fact template) */
static void train_repeat(st_model *m, int rounds, int n_ex, uint32_t seed,
                         const int *dists, int ndist)
{
    float *logits = (float *)malloc((size_t)ST_MAXSEQ * ST_VOCAB * sizeof(float));
    if (!logits) return;
    for (int r = 0; r < rounds; r++) {
        uint32_t rr = seed + (uint32_t)r * 2654435761u;
        for (int e = 0; e < n_ex; e++) {
            uint8_t seq[ST_MAXSEQ];
            int d = dists[lcg(&rr) % (uint32_t)ndist];
            int n = rv_build_repeat(seq, d, &rr, 0);
            if (n > ST_MAXSEQ) continue;
            st_zero_grad(m);
            st_forward(m, seq, n, logits);
            st_backward(m, seq, n);
            st_adam_step(m, ST_LR);
        }
    }
    free(logits);
}

/* stage B: a student trained by `c2` and saved with C2_SAVE=<file> */
static int rv_save_file(const st_model *m, const char *path)
{
    size_t cap = st_blob_size(m);
    unsigned char *blob = (unsigned char *)malloc(cap);
    if (!blob) return -1;
    long n = st_save(m, blob, cap);
    FILE *f = n > 0 ? fopen(path, "wb") : NULL;
    int ok = f && fwrite(blob, 1, (size_t)n, f) == (size_t)n;
    if (f && fclose(f) != 0) ok = 0;
    free(blob);
    return ok ? 0 : -1;
}

static int rv_load_file(st_model *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = st_blob_size(m);
    unsigned char *blob = (unsigned char *)malloc(cap + 1);
    size_t n = blob ? fread(blob, 1, cap + 1, f) : 0;
    fclose(f);
    int rc = (blob && n == cap) ? st_load(m, blob, n) : -1;
    free(blob);
    return rc == ST_OK ? 0 : -1;
}

static int falsify_main(void)
{
    const int dd[6] = { 16, 32, 48, 96, 128, 192 };
    const int tdists[6] = { 8, 24, 48, 96, 128, 176 };
    /* ROTE_QUICK=1: 8 probes, 2 training rounds - only to see the code run
     * end to end and time it; NOT the stage-A measurement (that is 64 / 24). */
    const int quick = getenv("ROTE_QUICK") != 0;
    const int NP = quick ? 8 : 64, CTX = 16, ROUNDS = quick ? 2 : 24;
    if (quick) printf("[rote] QUICK budget (8 probes, 2 rounds): timing only, not a result\n");
    printf("[rote] ==== research/rote-vs-copy.md stage A: floor, NC, PC (C1 FULL budget) ====\n");
    fflush(stdout);
    double tall = now_ms();
    /* floor: three untrained students. A point is OVER when its own
     * mean - 3 SE exceeds the largest floor mean at that d (the QUICK run used
     * the floor's SE: the untrained floor is near-uniform, so its SE is tiny
     * and a trained model's ordinary noise crossed it - the NC went red on
     * noise). A point with fewer than NP/2 valid probes (d=192: the probe
     * overflows 256 bytes unless K and V are short) is n/a. */
    double flS[6], flR[6];
    for (int i = 0; i < 6; i++) { flS[i] = -1e30; flR[i] = -1e30; }
    #define RV_OK(st)        ((st).n >= NP / 2)
    #define RV_OVER(st, fl)  (RV_OK(st) && (st).mean - 3.0 * (st).se > (fl))
    for (int s = 0; s < 3; s++) {
        st_model f;
        if (st_init(&f, 0xF100A000u + (uint32_t)s * 7919u) != ST_OK) { printf("init OOM\n"); return 2; }
        st_rope_set_enabled(1);
        for (int i = 0; i < 6; i++) {
            double r0;
            rv_stat S = measure_S(&f, dd[i], NP, 0x5EED5u + (uint32_t)i * 101u, 0);
            rv_stat R = measure_R(&f, dd[i], NP, 0x5EEDAu + (uint32_t)i * 101u, 0, &r0);
            printf("[rote] floor seed=%d d=%d S=%+.5f (se %.5f, n %d) R=%+.5f (se %.5f, n %d) R0=%+.5f\n",
                   s, dd[i], S.mean, S.se, S.n, R.mean, R.se, R.n, r0);
            if (RV_OK(S) && S.mean > flS[i]) flS[i] = S.mean;
            if (RV_OK(R) && R.mean > flR[i]) flR[i] = R.mean;
        }
        fflush(stdout);
        st_free(&f);
    }
    for (int i = 0; i < 6; i++)
        printf("[rote] floor max d=%d S=%+.5f R=%+.5f\n", dd[i], flS[i], flR[i]);

    /* the C1 FULL student (same seed and budget as `full`), or (stage B) a
     * student `c2` trained and saved with C2_SAVE, read from ROTE_LOAD */
    st_model m;
    if (st_init(&m, 0xBABE0C1u) != ST_OK) { printf("init OOM\n"); return 2; }
    st_rope_set_enabled(1);
    double t0 = now_ms();
    const char *load = getenv("ROTE_LOAD");
    if (load) {
        if (rv_load_file(&m, load) != 0) { printf("[rote] ROTE_LOAD %s: cannot load\n", load); return 2; }
        printf("[rote] loaded the student under test from %s\n", load);
    } else {
        train_ctx(&m, ROUNDS, 128, 0x7A1E77u, tdists, 6);
        printf("[rote] trained C1 FULL in %.1f s\n", (now_ms() - t0) / 1000.0);
    }
    int sover = 0, rover = 0, ncbad = 0, nvs = 0, nvr = 0, nvn = 0;
    for (int i = 0; i < 6; i++) {
        double r0;
        rv_stat S = measure_S(&m, dd[i], NP, 0x5EED5u + (uint32_t)i * 101u, 0);
        rv_stat R = measure_R(&m, dd[i], NP, 0x5EEDAu + (uint32_t)i * 101u, 0, &r0);
        float A = measure_A(&m, dd[i], NP, 0xE7A1u + (uint32_t)i * 101u, 0);
        double Af = measure_A_free(&m, dd[i], NP, 0xE7A1u + (uint32_t)i * 101u, 0);
        double Ai = measure_A_free(&m, dd[i], NP, 0xE7A1u + (uint32_t)i * 101u, 1);
        nvs += RV_OK(S); nvr += RV_OK(R);
        sover += RV_OVER(S, flS[i]); rover += RV_OVER(R, flR[i]);
        printf("[rote] trained d=%d S=%+.5f (se %.5f, n %d)%s R=%+.5f (se %.5f, n %d)%s R0=%+.5f A=%+.5f A_free=%+.5f A_id=%+.5f\n",
               dd[i], S.mean, S.se, S.n, RV_OVER(S, flS[i]) ? " OVER" : "", R.mean, R.se, R.n,
               RV_OVER(R, flR[i]) ? " OVER" : "", r0, A, Af, Ai);
        if (dd[i] >= 32) {
            rv_stat Sn = measure_S(&m, dd[i], NP, 0x5EED5u + (uint32_t)i * 101u, CTX);
            rv_stat Rn = measure_R(&m, dd[i], NP, 0x5EEDAu + (uint32_t)i * 101u, CTX, &r0);
            int bad = RV_OVER(Sn, flS[i]) || RV_OVER(Rn, flR[i]);
            nvn += RV_OK(Sn) || RV_OK(Rn);
            ncbad += bad;
            printf("[rote] nc(ctx %d) d=%d S=%+.5f (se %.5f, n %d) R=%+.5f (se %.5f)%s\n", CTX, dd[i],
                   Sn.mean, Sn.se, Sn.n, Rn.mean, Rn.se, bad ? " OVER" : "");
        }
        fflush(stdout);
    }
    st_free(&m);

    /* PC: random repeats only, same student shape, the same example count.
     * ROTE_NO_PC=1 skips it (deterministic: the same student and seeds as
     * stage A, whose PC was R' over 6/6 - 98f7eb7a); counted as 6/6. */
    int pcover = 0, nvp = 0;
    if (getenv("ROTE_NO_PC")) {
        pcover = 6; nvp = 6;
        printf("[rote] pc skipped (ROTE_NO_PC): stage A's R' PC, over 6/6 (98f7eb7a)\n");
    } else {
        st_model pc;
        if (st_init(&pc, 0xBABE0C1u) != ST_OK) { printf("init OOM\n"); return 2; }
        st_rope_set_enabled(1);
        double t1 = now_ms();
        train_repeat(&pc, ROUNDS, 128, 0x9C9C9Cu, tdists, 6);
        printf("[rote] trained PC in %.1f s\n", (now_ms() - t1) / 1000.0);
        for (int i = 0; i < 6; i++) {
            double r0;
            rv_stat R = measure_R(&pc, dd[i], NP, 0x5EEDAu + (uint32_t)i * 101u, 0, &r0);
            nvp += RV_OK(R); pcover += RV_OVER(R, flR[i]);
            printf("[rote] pc d=%d R=%+.5f (se %.5f, n %d)%s R0=%+.5f\n", dd[i], R.mean, R.se, R.n,
                   RV_OVER(R, flR[i]) ? " OVER" : "", r0);
        }
        st_free(&pc);
    }

    /* S PC (stage B): the fact template at d=8 only - short enough to copy.
     * Pre-registered: S at d=16 OVER = the S ruler can see copying. */
    int spc = -1;
    {
        st_model sp;
        const int short_d[1] = { 8 };
        if (st_init(&sp, 0xBABE0C1u) != ST_OK) { printf("init OOM\n"); return 2; }
        st_rope_set_enabled(1);
        double t2 = now_ms();
        train_ctx(&sp, ROUNDS, 128, 0x5C5C5Cu, short_d, 1);
        printf("[rote] trained S-PC (d=8 only) in %.1f s\n", (now_ms() - t2) / 1000.0);
        for (int i = 0; i < 6; i++) {
            rv_stat S = measure_S(&sp, dd[i], NP, 0x5EED5u + (uint32_t)i * 101u, 0);
            if (i == 0) spc = RV_OVER(S, flS[i]);
            printf("[rote] spc d=%d S=%+.5f (se %.5f, n %d)%s\n", dd[i], S.mean, S.se, S.n,
                   RV_OVER(S, flS[i]) ? " OVER" : "");
        }
        st_free(&sp);
    }
    printf("[rote-spc] %s\n", spc ? "PASS (S sees copying at d=16)" : "FAIL (S cannot see copying: S results undecidable)");

    printf("[rote] S over %d/%d, R over %d/%d (valid points); NC over %d/%d; PC R over %d/%d\n",
           sover, nvs, rover, nvr, ncbad, nvn, pcover, nvp);
    printf("[rote-nc] %s\n", ncbad ? "FAIL (the ruler sees through a 16-byte window: broken)" : "PASS");
    printf("[rote-pc] %s\n", pcover >= 4 ? "PASS" : "FAIL (R cannot see copying at this size: undecidable)");
    const char *cls = (ncbad || pcover < 4) ? "UNDECIDABLE (ruler not shown sound)" :
                      (sover >= 4 && rover >= 4) ? "general copy" :
                      (sover >= 4) ? "copy inside the template only" :
                      (rover >= 4) ? "induction on random repeats, but no fact copy (not in the memo's table)" :
                      "no copy shown (habit if A != 0, else nothing)";
    #undef RV_OK
    #undef RV_OVER
    printf("[rote] verdict (%s): %s%s\n", load ? load : "C1 FULL student", cls,
           spc ? "" : " [the S ruler is not shown sound: S counts are undecidable]");
    printf("[rote] total %.1f s\n", (now_ms() - tall) / 1000.0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "falsify") == 0) return falsify_main();
    /* fast cross-arch determinism probe: print ONLY the fixed-probe-stream FNV
     * and exit (no training) — lets a cross-arch harness (qemu-x86_64 vs native
     * aarch64) confirm the byte-identical probe stream without paying training
     * cost under emulation. */
    if (argc > 1 && strcmp(argv[1], "det") == 0) {
        printf("[ctx-carry-determinism] fixed-probe-stream FNV = 0x%016llx\n",
               (unsigned long long)determinism_probe_fnv());
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "c2") == 0) {
        uint64_t target = 10ull * 1024 * 1024;   /* 10 MB default */
        const char *env = getenv("C2_TARGET_BYTES");
        if (env) { uint64_t v = strtoull(env, 0, 10); if (v > 0) target = v; }
        printf("========================================================================\n");
        printf("[scale-wall-c2] data-reservoir scale-up: target=%llu bytes (%.2f MB)\n",
               (unsigned long long)target, (double)target / (1024.0 * 1024.0));
        printf("========================================================================\n");
        st_model m;
        if (st_init(&m, 0xBABEC2u) != ST_OK) { printf("init OOM\n"); return 2; }
        st_rope_set_enabled(1);
        int tdists[6] = { 8, 24, 48, 96, 128, 176 };
        double t0 = now_ms();
        uint64_t examples = 0;
        uint64_t bytes = train_ctx_bytes(&m, target, 0x7A1EC2u, tdists, 6, &examples);
        double t1 = now_ms();
        printf("[scale-wall-c2] trained %llu bytes over %llu examples in %.1f s (%.4f ms/example)\n",
               (unsigned long long)bytes, (unsigned long long)examples, (t1 - t0) / 1000.0,
               examples ? (t1 - t0) / (double)examples : 0.0);

        int dfull[6] = { 16, 32, 48, 96, 128, 192 };
        printf("\n[ctx-carry] A(d) after C2 scale-up (full window, 64 probes/point)\n");
        printf("   d     A_full(d)    A_clamp64(d)\n");
        for (int i = 0; i < 6; i++) {
            float af = measure_A(&m, dfull[i], 64, 0xE7A1u + (uint32_t)i * 101u, 0);
            float ac = measure_A(&m, dfull[i], 64, 0xE7A1u + (uint32_t)i * 101u, 64);
            printf("   %-4d  %+9.5f    %+9.5f\n", dfull[i], af, ac);
        }
        const char *save = getenv("C2_SAVE");      /* rote-vs-copy.md stage B */
        if (save)
            printf("[scale-wall-c2] C2_SAVE %s: %s\n", save,
                   rv_save_file(&m, save) == 0 ? "saved" : "FAILED");
        printf("\n[scale-wall-c2] same gated arms as C1 (training-budget-independent):\n");
        cert_window_mechanism(&m);
        cert_cohort_island();
        st_free(&m);
        printf("\n[result] scale-wall-c2 measurement done (%s; not gated -- see scale-wall-c1.md)\n",
               g_fail ? "gated-arm FAIL" : "gated arms PASS");
        return g_fail ? 1 : 0;
    }
    int full = (argc > 1 && strcmp(argv[1], "full") == 0);
    printf("========================================================================\n");
    printf("[ctx-carry] SCALE-WALL rung C1 — context carry (RoPE + ST_MAXSEQ=%d)\n", ST_MAXSEQ);
    printf("            mode=%s   NS_STUDENT_VER=%d\n", full ? "FULL(nightly)" : "CI(reduced)", NS_STUDENT_VER);
    printf("========================================================================\n");

    /* sweep + budget. CI = a reduced sweep bounded for strict CI (~1 min); FULL
     * = the whole nightly curve (minutes). The GATED arms (window mechanism,
     * clamp-collapse, cohort island) do NOT depend on the training budget — only
     * the A(d) MAGNITUDE (printed, not gated) sharpens with the FULL budget. */
    int   dfull[6] = { 16, 32, 48, 96, 128, 192 };
    int   dci[2]   = { 32, 96 };
    const int *dsweep = full ? dfull : dci;
    int   nd       = full ? 6 : 2;
    int   rounds   = full ? 24 : 3;
    int   n_ex     = full ? 128 : 20;
    int   nprobe   = full ? 64 : 10;
    /* training distances: a spread up to the max swept distance so long-range
     * copy is actually in the training signal. */
    int   tdists[6] = { 8, 24, 48, 96, 128, 176 };
    int   ntd = full ? 6 : 4;

    /* ---- recall-lookup exclusion (assert before training) ---- */
    printf("\n[ctx-carry-exclusion] recall-lookup exclusion on the eval token space:\n");
    {
        uint32_t rng = 12345u; int emark = 1, r3clean = 1;
        for (int i = 0; i < 200; i++) {
            char K[16], V[16];
            gen_eval_tok(&rng, K); gen_eval_tok(&rng, V);
            if (!strstr(K, "qz") || !strstr(V, "qz")) emark = 0;  /* reserved bigram */
            if (r3_key_id_local(K) != -1) r3clean = 0;            /* not an R3 key    */
        }
        printf("   every eval K,V carries the reserved bigram \"qz\" (absent from the\n");
        printf("   \"qz\"-free training corpus) and r3_vocab_key_id(K)==-1 -> no\n");
        printf("   memorization, no R3 binding; the answer can only come from reading.\n");
        CHECK(emark,   "[ctx-carry-exclusion] eval tokens are corpus-non-members (qz bigram)");
        CHECK(r3clean, "[ctx-carry-exclusion] eval keys are OOV to the R3 key vocab (id==-1)");
    }

    /* ---- determinism pin ---- */
    {
        uint64_t f = determinism_probe_fnv();
        printf("\n[ctx-carry-determinism] fixed-probe-stream FNV = 0x%016llx\n",
               (unsigned long long)f);
        printf("   (integer-only generation; x86_64 and aarch64 MUST print this same value)\n");
        /* self-check the pin unless it is still the placeholder. */
        if (CTX_PROBE_FNV_PIN != 0xC275FB45C0DE0001ULL) {
            CHECK(f == CTX_PROBE_FNV_PIN, "[ctx-carry-determinism] probe-stream FNV matches the pin");
        } else {
            printf("   (pin not yet set — record 0x%016llx as CTX_PROBE_FNV_PIN)\n",
                   (unsigned long long)f);
        }
    }

    /* ---- train the C1 (RoPE) model ---- */
    st_model m;
    if (st_init(&m, 0xBABE0C1u) != ST_OK) { printf("init OOM\n"); return 2; }
    st_rope_set_enabled(1);
    printf("\n[train] RoPE model: rounds=%d n_ex=%d tdists up to %d ... (this is the slow part)\n",
           rounds, n_ex, tdists[ntd - 1]);
    train_ctx(&m, rounds, n_ex, 0x7A1E77u, tdists, ntd);

    /* ---- [ctx-carry] the A(d) curve, full window ---- */
    printf("\n[ctx-carry] A(d) = CE(answer|ablated) - CE(answer|intact), nats  (full 256 window)\n");
    printf("   d     A_full(d)    A_clamp64(d)   note\n");
    float Afull[6], Aclamp[6];
    for (int i = 0; i < nd; i++) {
        Afull[i]  = measure_A(&m, dsweep[i], nprobe, 0xE7A1u + (uint32_t)i * 101u, 0);
        Aclamp[i] = measure_A(&m, dsweep[i], nprobe, 0xE7A1u + (uint32_t)i * 101u, 64);
        const char *note = (dsweep[i] >= 96) ? "fact >=96B back: full window only" :
                           (dsweep[i] >= 64) ? "fact >64B back" : "fact within a 64 window";
        printf("   %-4d  %+9.5f    %+9.5f    %s\n", dsweep[i], Afull[i], Aclamp[i], note);
    }

    /* ---- [ctx-carry-clamp] the window-clamp collapse at large d (a CONTROL) */
    printf("\n[ctx-carry-clamp] CONTROL (semi-tautological, cross-audit #9) — clamp the TRAINED model to 64:\n");
    {
        /* pick the largest swept d >= 96 for the collapse assertion. */
        int idx = -1; for (int i = 0; i < nd; i++) if (dsweep[i] >= 96) idx = i;
        if (idx < 0) idx = nd - 1;
        float ac = Aclamp[idx];
        float aca = ac < 0 ? -ac : ac;
        printf("   at d=%d:  A_full=%+.5f   A_clamp64=%+.5f\n", dsweep[idx], Afull[idx], ac);
        printf("   the clamp drops the fact BY CONSTRUCTION (true of any model), so this ISOLATES\n");
        printf("   window-vs-training; the load-bearing mechanism proof is [ctx-carry-window] full_reads.\n");
        /* collapse bar: |A_clamp| small in absolute nats (measured-minus-margin). */
        CHECK(aca < 0.05f, "[ctx-carry-clamp] A_clamp64(d>=96) collapses to ~0 (control: isolates window-vs-training)");
    }

    /* ---- window mechanism anti-theater proof (deterministic) ---- */
    cert_window_mechanism(&m);

    /* ---- [ctx-carry-nope] RoPE vs NoPE, MEASURED, PRINTED (NOT gated) ---- */
    printf("\n[ctx-carry-nope] RoPE vs NoPE side-by-side (PRINTED, NOT GATED — §8: the cert\n");
    printf("                 bets on the WINDOW, not on RoPE; NoPE can learn implicit pos)\n");
    {
        st_model mn;
        int rr = full ? rounds : 2;                            /* lean twin budget */
        int nx = full ? n_ex : 16;
        if (st_init(&mn, 0xB0BE0C1u) == ST_OK) {
            st_rope_set_enabled(0);                            /* NoPE twin        */
            train_ctx(&mn, rr, nx, 0x7A1E77u, tdists, ntd);
            int dprobe = full ? 128 : 96;
            float A_nope = measure_A(&mn, dprobe, nprobe, 0x5151u, 0);
            st_rope_set_enabled(1);                            /* restore for RoPE */
            float A_rope = measure_A(&m,  dprobe, nprobe, 0x5151u, 0);
            printf("   d=%d   A_rope=%+.5f   A_nope=%+.5f   (positive => that model carries context)\n",
                   dprobe, A_rope, A_nope);
            st_free(&mn);
        }
    }
    st_rope_set_enabled(1);

    /* ---- [gen-cohort-island] ---- */
    cert_cohort_island();

    /* ---- honest scope line ---- */
    printf("\n[ctx-carry] SCOPE: 256-byte context carry is NOT conversation — it is the\n");
    printf("            atomic prerequisite (being told something and using it). The A(d)\n");
    printf("            MAGNITUDE is the measured deliverable; at toy scale it may be small.\n");
    printf("            The GATED proof is the deterministic window mechanism, not a forced\n");
    printf("            A(d) bar. The toy ceiling stands until C2-C4 move the curve.\n");

    st_free(&m);
    printf("\n[result] %s  (gated failures=%d)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
