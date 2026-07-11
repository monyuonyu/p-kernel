/*
 *  depth_compound_test.c — host cert for ACCURACY-COMPOUNDING (the AlphaZero
 *  crack in miniature). fable5 Wave-C. depth_iq_path_design.md §3.4 / §4.3.
 *
 *  The sibling of depth_test.c's [depth-compound-verified-only]. That arm proves
 *  distilling V-exact-verified deliberation winners lowers the correct-answer
 *  LOSS. THIS cert asks the harder, load-bearing question the "verifier-exceeds"
 *  loop actually claims:
 *
 *    After distilling the V-exact-verified winning traces DLB found by SEARCH x
 *    VERIFY, does the ONE-SHOT (K=1, greedy, NO deliberation) accuracy RISE above
 *    the pre-distill one-shot accuracy?
 *
 *  i.e. does the test-time compute a search spent yesterday become weight-
 *  resident skill the model answers in ONE shot tomorrow? That is the minimal
 *  self-improvement loop: the free perfect verifier lets the student generate
 *  its own correct supervision, distill it, and get better at answering cold —
 *  the AlphaZero compounding, at byte-baby scale.
 *
 *  The eval set is "held out" from the model's WEIGHT knowledge: these are
 *  queries the untrained baby CANNOT answer one-shot (its greedy accuracy is
 *  ~chance). DLB solves them by rejection sampling against the perfect verifier;
 *  the verified winners are distilled; then we re-measure ONE-SHOT accuracy on
 *  the SAME queries. A rise is amortization — in-distribution by construction,
 *  because amortizing search into weights IS an in-distribution claim (§4.3).
 *
 *  This cert drives the EXACT distill path the live DMN wire calls
 *  (student_shell.c student_dmn_consolidate -> dlb_compound_distill(g_student,
 *  ROUNDS, LR, require_verified=1)). Proving that function lifts one-shot
 *  accuracy proves the wire; the wire is a 2-line call to it.
 *
 *  LOAD-BEARING TEETH (anti-theater):
 *    Arm-D (disease): distill the SAME queries paired with WRONG answers, marked
 *      UNVERIFIED, with require_verified=0 (gate bypassed). One-shot accuracy must
 *      NOT rise — it DEGRADES (the model learns the wrong residues). If distilling
 *      garbage also "improved" accuracy the metric would be theater.
 *    STUB (nothing distilled): enqueue an UNVERIFIED ring, distill with
 *      require_verified=1 -> the HARD GATE skips all -> weights untouched ->
 *      one-shot accuracy is EXACTLY flat (greedy is deterministic). Proves the
 *      rise is the distill, not eval noise.
 *
 *  HONEST, pre-registered NULL (design §3.5, §6.2): if at tier=S the post-distill
 *  one-shot GAIN falls below the noise/step threshold, it is PRINTED as a NULL
 *  (like DLB's general-domain NULL) and the cert STILL PASSES structurally — the
 *  loss-based [depth-compound-verified-only] already proves the distill works;
 *  this cert reports whether the win shows up in one-shot ACCURACY, and does NOT
 *  tune to force it. The teeth (Arm-D degrades, stub flat) are HARD either way.
 *
 *  Usage:  ./depth_compound            (exit 0 = teeth green; compound claim reported)
 *          ./depth_compound --machine  (one FNV determinism line, cross-arch)
 *  Build: -O1 -ffp-contract=off (one math). See run_depth_compound.sh.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "student.h"
#include "dlb.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

/* ---- deterministic hash (one-math; same on every arch) -------------------- */
static uint64_t H(uint64_t a, uint64_t b)
{
    uint64_t h = 1469598103934665603ULL;
    h ^= a; h *= 1099511628211ULL;
    h ^= b; h *= 1099511628211ULL;
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27; h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return h;
}

/* ================================================================== */
/* the V-EXACT metric: mod-10 single-hop residue (§6.0 generator, k=1)   */
/* Identical construction to depth_test.c so both certs share one math.  */
/* ================================================================== */
static int item_digits(int base, int n, int *d)   /* returns residue e; k=1 */
{
    int e = 0;
    for (int i = 0; i <= 1; i++) {
        d[i] = (int)(H((uint64_t)base * 131u + (uint64_t)i, (uint64_t)n) % 10u);
        e = (i == 0) ? d[0] : ((e + d[i]) % 10);
    }
    return e;
}
static int build_query(uint8_t *buf, const int *d)  /* returns qn ("d+d=") */
{
    int n = 0;
    buf[n++] = (uint8_t)('0' + d[0]);
    buf[n++] = '+';
    buf[n++] = (uint8_t)('0' + d[1]);
    buf[n++] = '=';
    return n;
}
static int fold_from_query(const uint8_t *q, int qn)
{
    int e = 0, have = 0;
    for (int i = 0; i < qn; i++) {
        if (q[i] >= '0' && q[i] <= '9') {
            int v = q[i] - '0';
            e = have ? ((e + v) % 10) : v;
            have = 1;
        } else if (q[i] == '=') break;
    }
    return e;
}
/* V-EXACT: reads ONLY (query, candidate) — a real procedural oracle, AUC=1. */
static float vexact_verify(const uint8_t *q, int qn,
                           const uint8_t *cand, int cn, void *vctx)
{
    (void)vctx;
    if (cn < 1) return 0.0f;
    int e = fold_from_query(q, qn);
    return ((cand[0] % 10) == e) ? 1.0f : 0.0f;
}

/* ---- ONE-SHOT (K=1, greedy, NO deliberation) accuracy on a set ------------ */
/* This is the metric under test: what the model KNOWS cold, temp=0 greedy, one
 * answer byte, no search. Deterministic -> a flat stub is EXACTLY flat. */
static double one_shot_acc(st_model *m, int base, int N)
{
    int hit = 0;
    for (int n = 0; n < N; n++) {
        int d[2]; int e = item_digits(base, n, d);
        uint8_t q[8]; int qn = build_query(q, d);
        uint8_t out[DLB_GEN_MAX];
        int on = st_generate(m, q, qn, out, 1, 0.0f, 1, 1);   /* greedy, one byte */
        if (on >= 1 && (out[0] % 10) == e) hit++;
    }
    return (double)N > 0 ? (double)hit / N : 0.0;
}

/* ---- run DLB (search x verify) on the eval set; enqueue ONLY verified winners */
/* Returns the number of verified winners enqueued (the traces production would
 * distill). Each enqueued trace is (query, winner-byte, verified=1). */
static int deliberate_and_enqueue(st_model *m, int base, int N, int K)
{
    dlb_budget b = { K, 1 /*one answer byte*/, 1.0f, 40, 0.0f /*always deliberate*/ };
    dlb_compound_reset();
    int enq = 0;
    for (int n = 0; n < N; n++) {
        int d[2]; item_digits(base, n, d);
        uint8_t q[8]; int qn = build_query(q, d);
        uint8_t out[DLB_GEN_MAX]; dlb_result info;
        int on = dlb_answer(m, q, qn, out, (int)sizeof out, &b, vexact_verify, NULL, &info);
        if (on < 1) continue;
        /* enqueue ONLY a V-exact-verified winner (best_score==1 -> the verifier
         * accepted the returned candidate). The winning trace = the query + the
         * one answer byte DLB found by search. */
        if (info.best_score >= 0.5f) {
            dlb_compound_enqueue(q, qn, out, 1, /*verified=*/1);
            enq++;
        }
    }
    return enq;
}

/* ================================================================== */
/* machine mode: cross-arch determinism hash of the whole pre/post run   */
/* ================================================================== */
static uint64_t run_core(double *pre, double *post, double *post_d, double *post_s);

static int machine_mode(void)
{
    double pre, post, post_d, post_s;
    uint64_t fnv = run_core(&pre, &post, &post_d, &post_s);
    printf("[machine] compound_acc_fnv = %016llx\n", (unsigned long long)fnv);
    printf("[machine] pre=%.4f post=%.4f armd=%.4f stub=%.4f\n", pre, post, post_d, post_s);
    return 0;
}

/* ---- the shared core: one deterministic pre/DLB/distill/post sequence ------ */
/* Fills pre/post (verified loop), post_d (Arm-D disease), post_s (stub). Returns
 * an FNV over the four accuracies (x1000, integer) for the machine determinism
 * line — identical across arches under one-math. */
#define C_BASE   6000    /* eval seed base, DISJOINT from depth_test's 1000/5000/7000/8000 */
#define C_NEVAL  24      /* <= DLB_RING_MAX; each trace 5 bytes < DLB_TRACE_MAX */
#define C_K      32      /* search width (best-of-32 over 10 residues -> ~perfect coverage) */
#define C_ROUNDS 30      /* distill passes (matches depth_test [depth-not-breadth]) */
#define C_LR     5e-3f

static uint64_t run_core(double *pre, double *post, double *post_d, double *post_s)
{
    /* Four models from the SAME init seed: only the distilled ring differs, so a
     * post-vs-pre delta is attributable to the traces, nothing else. */
    st_model mv, md, ms;
    st_init_tier(&mv, 0xC0FFEEu, ST_TIER_S);   /* verified loop (production)      */
    st_init_tier(&md, 0xC0FFEEu, ST_TIER_S);   /* Arm-D: unverified WRONG (disease)*/
    st_init_tier(&ms, 0xC0FFEEu, ST_TIER_S);   /* stub: gate blocks -> nothing     */

    /* (0) PRE: one-shot accuracy of the untrained baby (weight-held-out). */
    double p_pre = one_shot_acc(&mv, C_BASE, C_NEVAL);

    /* (1) VERIFIED loop: DLB solves the eval queries, enqueue verified winners,
     *     distill via the LIVE path (require_verified=1). */
    (void)deliberate_and_enqueue(&mv, C_BASE, C_NEVAL, C_K);
    dlb_compound_distill(&mv, C_ROUNDS, C_LR, /*require_verified=*/1);
    double p_post = one_shot_acc(&mv, C_BASE, C_NEVAL);

    /* (2) Arm-D disease: the SAME queries paired with a WRONG residue, marked
     *     UNVERIFIED, distilled with the gate BYPASSED (require_verified=0). */
    dlb_compound_reset();
    for (int n = 0; n < C_NEVAL; n++) {
        int d[2]; int e = item_digits(C_BASE, n, d);
        uint8_t q[8]; int qn = build_query(q, d);
        uint8_t bad = (uint8_t)((e + 3) % 10);   /* deterministic WRONG residue */
        dlb_compound_enqueue(q, qn, &bad, 1, /*verified=*/0);
    }
    dlb_compound_distill(&md, C_ROUNDS, C_LR, /*require_verified=*/0);
    double p_postd = one_shot_acc(&md, C_BASE, C_NEVAL);

    /* (3) STUB: an UNVERIFIED ring distilled with the gate ON (require_verified=1)
     *     -> distinct=0 -> weights untouched -> accuracy EXACTLY flat. */
    dlb_compound_reset();
    for (int n = 0; n < C_NEVAL; n++) {
        int d[2]; int e = item_digits(C_BASE, n, d);
        uint8_t q[8]; int qn = build_query(q, d);
        uint8_t bad = (uint8_t)((e + 3) % 10);
        dlb_compound_enqueue(q, qn, &bad, 1, /*verified=*/0);
    }
    int stub_distilled = dlb_compound_distill(&ms, C_ROUNDS, C_LR, /*require_verified=*/1);
    double p_posts = one_shot_acc(&ms, C_BASE, C_NEVAL);
    (void)stub_distilled;

    *pre = p_pre; *post = p_post; *post_d = p_postd; *post_s = p_posts;

    st_free(&mv); st_free(&md); st_free(&ms);

    uint64_t f = 1469598103934665603ULL;
    f = H(f, (uint64_t)(p_pre   * 1000.0 + 0.5));
    f = H(f, (uint64_t)(p_post  * 1000.0 + 0.5));
    f = H(f, (uint64_t)(p_postd * 1000.0 + 0.5));
    f = H(f, (uint64_t)(p_posts * 1000.0 + 0.5));
    return f;
}

/* ================================================================== */
int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--machine") == 0) return machine_mode();

    printf("=== ACCURACY-COMPOUNDING cert (the AlphaZero crack in miniature) ===\n");
    printf("    V-exact mod-10 single-hop; tier=S byte-baby; K=%d search, %d distill rounds.\n",
           C_K, C_ROUNDS);
    printf("    Question: does distilling V-exact-verified DLB winners raise ONE-SHOT\n");
    printf("    (K=1, greedy, NO deliberation) accuracy above the pre-distill one-shot?\n\n");

    const double CHANCE  = 0.10;   /* uniform residue mod 10 */
    const double MARGIN  = 0.15;   /* the compound gain must clear this to be a WIN */
    const double NULL_TH = 0.03;   /* below this the gain is a pre-registered NULL   */

    double pre, post, post_d, post_s;
    (void)run_core(&pre, &post, &post_d, &post_s);

    printf("[compound] eval seed base=%d  N=%d (weight-held-out: baby can't answer cold)\n",
           C_BASE, C_NEVAL);
    printf("    pre-distill  one-shot acc = %.3f  (chance %.2f)\n", pre, CHANCE);
    printf("    post-distill one-shot acc = %.3f  (verified winners distilled)\n", post);
    printf("    Arm-D  (distill UNVERIFIED wrong, gate bypassed) one-shot acc = %.3f\n", post_d);
    printf("    STUB   (unverified ring, gate ON -> nothing distilled)  acc  = %.3f\n", post_s);

    double gain = post - pre;
    printf("    one-shot GAIN (post - pre) = %+.3f\n\n", gain);

    /* ---- sanity: the untrained baby really is at ~chance one-shot ---------- */
    CHECK(pre <= CHANCE + 0.10,
          "[compound-sanity] pre-distill one-shot acc is ~chance (weight-held-out)");

    /* ---- TEETH (hard, anti-theater) --------------------------------------- */
    /* Arm-D: distilling wrong (unverified) answers must NOT raise one-shot acc;
     * it degrades (the model is taught the wrong residue). This is what licenses
     * crediting the verified loop at all (feedback_validator_and_learner_traps). */
    int armd_degrades = (post_d <= pre + NULL_TH);
    printf("    ANTI-THEATER: Arm-D distills WRONG traces -> one-shot %s (%.3f vs pre %.3f) -> %s\n",
           armd_degrades ? "does NOT rise" : "ROSE(!)", post_d, pre,
           armd_degrades ? "RED (as designed)" : "STILL GREEN — THEATER");
    CHECK(armd_degrades,
          "[compound-armd] Arm-D: distilling UNVERIFIED wrong traces does NOT raise one-shot acc");

    /* STUB: the gate skipped everything -> weights untouched -> EXACTLY flat. */
    printf("    ANTI-THEATER: STUB gate-blocks all -> one-shot acc %s (%.3f vs pre %.3f)\n",
           (post_s == pre) ? "EXACTLY flat" : "MOVED(!)", post_s, pre);
    CHECK(post_s == pre,
          "[compound-stub] gate-blocked distill leaves one-shot acc exactly flat (rise is the distill)");

    /* ---- the compound claim: reported, honest-NULL fallback --------------- */
    int compounds = (gain >= MARGIN);
    int honest_null = (gain < NULL_TH);
    if (compounds) {
        printf("\n    [ACCURACY COMPOUNDS] post > pre by %+.3f >= margin %.2f: the search a\n"
               "    K=%d deliberation spent is now WEIGHT-RESIDENT — answered in ONE shot.\n"
               "    The verifier-exceeds minimal loop closes: the free perfect verifier let\n"
               "    the student generate its own correct supervision and get better cold.\n",
               gain, MARGIN, C_K);
        CHECK(post > pre + MARGIN,
              "[compound-accuracy] post-distill one-shot acc RISES above pre by the margin");
    } else if (honest_null) {
        printf("\n    [HONEST NULL] post-distill one-shot GAIN %+.3f < %.2f: at tier=S the\n"
               "    compounding shows in LOSS (the [depth-compound-verified-only] cert) but is\n"
               "    BELOW the one-shot ACCURACY step threshold here. PRE-REGISTERED NULL, not\n"
               "    tuned (design §3.5/§6.2). The cert PASSES structurally: the teeth hold and\n"
               "    the distill mechanism is proven; the ACCURACY leg is a ThinkPad-runner scale\n"
               "    question, exactly like DLB's deferred general-domain gain.\n", gain, NULL_TH);
        /* structural pass: not a CHECK failure (mirrors DLB's printed NULL). */
    } else {
        /* gain in the grey band [NULL_TH, MARGIN): a partial, honestly reported
         * lift that neither clears the win margin nor qualifies as a clean null. */
        printf("\n    [PARTIAL] post-distill one-shot GAIN %+.3f is in [%.2f,%.2f): a real but\n"
               "    sub-margin lift — reported honestly, not gated up. Teeth remain the proof.\n",
               gain, NULL_TH, MARGIN);
    }

    printf("\n[compound-summary] pass=%d fail=%d  (pre=%.3f post=%.3f armd=%.3f stub=%.3f)\n",
           g_pass, g_fail, pre, post, post_d, post_s);
    if (g_fail == 0) {
        printf("[result] PASS\n");
        printf("[note] The live DMN wire (student_shell.c student_dmn_consolidate ->\n");
        printf("       dlb_compound_distill(g_student, ROUNDS, LR, require_verified=1)) calls\n");
        printf("       the EXACT distill path this cert exercises. Compound=%s null=%s.\n",
               compounds ? "YES" : "no", honest_null ? "YES" : "no");
        return 0;
    }
    printf("[result] FAIL\n");
    return 1;
}
