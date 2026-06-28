/*
 *  student_yield_test.c — host cert for the COOPERATIVE-YIELD DMN fix
 *  (docs/architecture/cooperative-yield-plan.md; arch/common/llm/student_shell.c).
 *
 *  The bug: the DMN sleep-consolidation batch (ST_DMN_ROUNDS * train_windows
 *  complete forward/backward/adam triples) used to run ENTIRELY inside one
 *  student_dmn_consolidate() call. On the single-threaded cooperative Linux port
 *  that call never re-enters the kernel, so it is NON-PREEMPTIBLE and starves the
 *  node's shell/SWIM/net/mind-ask/pfs-serve while it dreams (the 27/41/42/32
 *  live-3node failures).
 *
 *  The fix slices the batch into <=ST_DMN_PASS_BUDGET complete triples per call,
 *  checkpoints a flat cursor, and returns so dmn_task's tk_dly_tsk loop yields.
 *
 *  This cert links the REAL production student_shell.c (via #include, so its
 *  static sleep_rounds / sleep_rounds_resume / window / heldout_loss and the
 *  file-static cursor are all exercised) plus the real student.c math, and stubs
 *  only the kernel-side seams student_shell.c references (pfs durable, cradle
 *  transport, device sizing, gguf/teacher probe, ss6 transport) — none of which
 *  is invoked on this path.
 *
 *  Two falsifiable certs + a falsifier:
 *
 *   [yield-byte-identical]  sliced (with a pure-read inference interleaved
 *                           between every slice) produces a final st_save blob
 *                           BIT-IDENTICAL to the all-at-once run. The
 *                           determinism crown: slicing changes only WHERE we
 *                           pause between list elements, never the order/operands.
 *
 *   [yield-responsive]      driving the PRODUCTION student_dmn_consolidate()
 *                           state machine: (a) every call advances <= K passes
 *                           (bounded compute -> the node yields each pulse);
 *                           (b) cumulative passes == ST_DMN_ROUNDS*train_windows
 *                           (thinking was NOT skipped); (c) the final blob ==
 *                           the all-at-once reference (correct, not a cheat that
 *                           dropped rounds).
 *
 *   FALSIFIER (-DYIELD_DISABLE): student_dmn_consolidate runs the WHOLE batch in
 *   the first call. Then [yield-responsive] (a) FAILS RED (one call advances all
 *   total passes, >> K) — modelling the stall — while (b)/(c) and the
 *   byte-identity still hold (same math, just not yielded). Proves the cert has
 *   teeth: it goes RED exactly when the node would starve.
 *
 *  Build (one math everywhere): -O1 -ffp-contract=off.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Pull in the REAL production unit under test (and, transitively, student.h /
 * dev_capacity.h / gguf.h / forward.h). Its static internals become visible to
 * this TU so the cert drives the exact shipped code, not a re-implementation. */
#include "../../arch/common/llm/student_shell.c"

/* ---------- kernel-seam stubs (never invoked on the consolidation path) ----
 * These resolve the symbols student_shell.c references outside student.c. The
 * cert never calls the teacher probe or the ss6 transport; the durable + cradle
 * + device seams are driven into their benign "no-op / fixture / M-tier" arms. */

/* durable backend: pretend persistence is ON (so the consolidate persist arm is
 * exercised) but make the writes no-ops and report "no saved baby". */
int pfs_dur_active(void) { return 1; }
int pfs_dur_write(const char *f, const void *d, unsigned n) { (void)f;(void)d;(void)n; return 0; }
int pfs_dur_read (const char *f, void *b, unsigned m) { (void)f;(void)b;(void)m; return -1; }

/* cradle transport: the REAL cradle.c is linked (it provides cradle_window_src /
 * cradle_lesson_ingest / cradle_lesson_freeze / cradle_lesson_len / cradle_set_
 * enabled / cradle_lesson_clear), so Cert C exercises the genuine freeze gate
 * rather than a NULL stub. cradle_poll_and_pull stays the weak no-op defined in
 * student_shell.c (cradle_net.c is NOT linked), so the start-of-batch pull is
 * inert and the ring holds exactly what the cert installs. With no lesson
 * installed (Cert A/B) the ring is empty -> cradle_window_src returns NULL ->
 * window() reads TEACHER_FIXTURE, unchanged. */

/* device sizing: deterministic M-tier init from the same seed the production
 * student_ensure uses (STUDENT_SEED), so g_student matches a reference clone. */
int st_init_device(st_model *m, uint32_t seed) { return st_init(m, seed); }

/* teacher GGUF probe seam — referenced only by teacher_gguf_loaded(), never
 * called here; just needs to resolve. */
int  gguf_open (gguf_file *gf, const char *p) { (void)gf;(void)p; return -1; }
void gguf_close(gguf_file *gf)                { (void)gf; }
int  lm_load   (lm_model *m, const gguf_file *gf) { (void)m;(void)gf; return -1; }
void lm_free   (lm_model *m)                  { (void)m; }

/* ss6-live transport seam — referenced only by the ss6live verb, never here. */
void     ss6_live_install(void *m)   { (void)m; }
void     ss6_live_uninstall(void)    { }
void     ss6_live_set_enabled(int o) { (void)o; }
unsigned ss6_live_req_sent(void)     { return 0; }
unsigned ss6_live_req_served(void)   { return 0; }

/* ---------- harness ---------- */
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

static double now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* serialize a model into a freshly malloc'd buffer; *len gets the byte count. */
static unsigned char *dump(st_model *m, long *len)
{
    size_t cap = st_blob_size(m);
    unsigned char *b = (unsigned char *)malloc(cap);
    if (!b) { *len = -1; return NULL; }
    *len = st_save(m, b, cap);
    return b;
}

int main(void)
{
    /* The production fixture plan (cradle_window_src stubbed NULL -> fixture). */
    int corpus_n  = (int)sizeof(TEACHER_FIXTURE) - 1;
    int total_win = corpus_n / ST_DMN_SEQLEN;
    int trainw    = total_win * 3 / 4; if (trainw < 2) trainw = 2;
    int heldw     = total_win - trainw; if (heldw < 1) heldw = 1;
    int train_end = trainw * ST_DMN_SEQLEN;
    int total     = ST_DMN_ROUNDS * trainw;   /* the update-list length */
    int K         = ST_DMN_PASS_BUDGET;

    printf("[yield] plan: corpus=%dB seqlen=%d trainw=%d rounds=%d total_passes=%d K=%d\n",
           corpus_n, ST_DMN_SEQLEN, trainw, ST_DMN_ROUNDS, total, K);
#ifdef YIELD_DISABLE
    printf("[yield] *** built with -DYIELD_DISABLE (falsifier: whole batch per call) ***\n");
#endif

    /* ===================================================================
     * Cert A — [yield-byte-identical]: sliced (w/ interleaved pure-read
     * inference) == all-at-once, bit-for-bit.
     * =================================================================== */
    st_model ref, sl;
    if (st_init(&ref, STUDENT_SEED) != ST_OK || st_init(&sl, STUDENT_SEED) != ST_OK) {
        printf("[yield] st_init OOM\n"); return 2;
    }

    /* all-at-once + measure one triple's wall cost (for K justification). */
    double t0 = now_ms();
    sleep_rounds(&ref, ST_DMN_SEQLEN, trainw, ST_DMN_ROUNDS, ST_DMN_LR);
    double t1 = now_ms();
    double per_triple_ms = (total > 0) ? (t1 - t0) / total : 0.0;

    /* sliced, interleaving a pure-read held-out inference between every slice. */
    int idx = 0, slice_calls = 0, max_slice = 0;
    while (idx < total) {
        int before = idx;
        idx = sleep_rounds_resume(&sl, ST_DMN_SEQLEN, trainw, ST_DMN_ROUNDS,
                                  ST_DMN_LR, idx, K);
        int delta = idx - before;
        if (delta > max_slice) max_slice = delta;
        slice_calls++;
        /* interleaved READER — a `mind ask` between slices must not corrupt the
         * batch (re-entrancy arg, plan §2.2): it runs its own full st_forward. */
        volatile float junk = heldout_loss(&sl, ST_DMN_SEQLEN, train_end, heldw);
        (void)junk;
        if (slice_calls > total + 4) { printf("[yield] slice loop runaway\n"); break; }
    }

    long lr_ref, lr_sl;
    unsigned char *b_ref = dump(&ref, &lr_ref);
    unsigned char *b_sl  = dump(&sl,  &lr_sl);
    printf("[yield] sliced in %d calls, max slice=%d passes (interleaved %d reads)\n",
           slice_calls, max_slice, slice_calls);
    CHECK(b_ref && b_sl && lr_ref == lr_sl && lr_ref > 0,
          "[yield-byte-identical] blob sizes equal & non-empty");
    CHECK(b_ref && b_sl && lr_ref == lr_sl && memcmp(b_ref, b_sl, (size_t)lr_ref) == 0,
          "[yield-byte-identical] sliced+interleaved == all-at-once (FULL blob memcmp==0)");
    free(b_ref); free(b_sl); st_free(&ref); st_free(&sl);

    /* ===================================================================
     * Cert B — [yield-responsive]: drive the PRODUCTION state machine.
     * =================================================================== */
    /* a clean all-at-once reference from the same seed/plan. */
    st_model refB;
    if (st_init(&refB, STUDENT_SEED) != ST_OK) { printf("[yield] refB OOM\n"); return 2; }
    sleep_rounds(&refB, ST_DMN_SEQLEN, trainw, ST_DMN_ROUNDS, ST_DMN_LR);
    float ref_loss = heldout_loss(&refB, ST_DMN_SEQLEN, train_end, heldw);

    /* bring up the resident baby exactly as production does (st_init_device stub
     * -> st_init(STUDENT_SEED)), then reset the cursor and drive the tick loop. */
    g_have_student = 0; g_consol_active = 0; g_consol_idx = 0;
    if (student_ensure(0) != 0) { printf("[yield] student_ensure OOM\n"); return 2; }

    int calls = 0, cumulative_prev = 0, max_call = 0, cumulative = 0, rc;
    double max_call_ms = 0.0;             /* WALL time of the slowest single call  */
    do {
        int before = g_consol_idx;        /* snapshot resets to 0 on call 1 */
        double c0 = now_ms();
        rc = student_dmn_consolidate();
        double c1 = now_ms();
        if (c1 - c0 > max_call_ms) max_call_ms = c1 - c0;
        int after = g_consol_idx;         /* left at `total` after completion */
        int delta = after - before;
        if (delta > max_call) max_call = delta;
        cumulative = after;
        calls++;
        (void)cumulative_prev;
        if (calls > total + 8) { printf("[yield] consolidate loop runaway\n"); break; }
    } while (rc == 0);

    float got_loss = student_dmn_heldout_loss();
    long lr_b, lr_g;
    unsigned char *b_g = dump(&g_student, &lr_g);
    unsigned char *b_b = dump(&refB,      &lr_b);

    printf("[yield] consolidate: calls=%d max_passes/call=%d cumulative=%d (want %d)  "
           "final_loss=%.6f ref_loss=%.6f\n",
           calls, max_call, cumulative, total, (double)got_loss, (double)ref_loss);

    /* WALL-TIME responsiveness (HARDWARE-RELATIVE, flake-proof). The count bound
     * (max_call <= K) alone cannot catch an OVERSIZED K: if K >= total the
     * "slicing" is a no-op (one call does the whole batch) yet max_call <= K
     * trivially holds — the exact non-yielding stall we are killing. So also
     * assert the slowest single call is well under the cost of the WHOLE batch:
     * a real K-slice is ~K triples, the monolith is ~total triples, so the
     * natural separator is "one call < half the all-at-once cost" — true for any
     * genuine slice on ANY hardware, RED for an unsliced/oversized-K call. We do
     * NOT use a fixed ms ceiling: a triple is ~97ms on the dev host but ~244ms
     * under the emulated sandbox, so an absolute bound would flake. The absolute
     * ms + % of a 1000ms pulse are PRINTED for the human (honest starvation
     * signal), but the ASSERT is the hardware-independent ratio. */
    double monolith_ms = per_triple_ms * total;       /* cost of the whole batch at once */
    double wall_ceiling = 0.5 * monolith_ms;          /* one call must be < half of it     */
    printf("[yield] single-triple ~%.3f ms  K=%d  slowest consolidate() call = %.1f ms "
           "(~%.0f%% of a 1000ms pulse; monolith would be ~%.0f ms; ceiling %.0f ms)\n",
           per_triple_ms, K, max_call_ms, 100.0 * max_call_ms / 1000.0,
           monolith_ms, wall_ceiling);

    CHECK(max_call <= K,
          "[yield-responsive] every consolidate() call advances <= K passes (node yields)");
    CHECK(max_call_ms < wall_ceiling,
          "[yield-responsive] slowest call wall-time < half the all-at-once cost (oversized-K starvation guard)");
    CHECK(cumulative == total,
          "[yield-responsive] cumulative passes == rounds*train_windows (not skipped)");
    CHECK(rc == 1,
          "[yield-responsive] final call returns 1 (one sleep-line per consolidation)");
    CHECK(b_g && b_b && lr_g == lr_b && lr_g > 0 &&
          memcmp(b_g, b_b, (size_t)lr_g) == 0,
          "[yield-responsive] production final blob == all-at-once reference (correct)");
    CHECK(got_loss == ref_loss,
          "[yield-responsive] final held-out loss == all-at-once reference loss");

    free(b_g); free(b_b); st_free(&refB);

    /* ===================================================================
     * Cert C — [yield-corpus-frozen]: a lesson arriving MID-BATCH (the
     * cradle_net_task ingest path, which can now interleave because the DMN
     * yields) must be DEFERRED, not mixed into the running batch — so
     * byte-identity to the all-at-once run holds even on a LIVE corpus. This
     * closes the cradle_window_src=NULL hole: it drives the REAL cradle ring.
     * =================================================================== */
    static uint8_t L1[700], L2[512];
    for (int i = 0; i < (int)sizeof L1; i++) L1[i] = (uint8_t)(31 * i + 7);
    for (int i = 0; i < (int)sizeof L2; i++) L2[i] = (uint8_t)(101 * i + 200);

    cradle_set_enabled(1);
    cradle_lesson_freeze(0);
    cradle_lesson_clear();
    int ig1 = cradle_lesson_ingest(L1, (int)sizeof L1);   /* ring := L1 (live corpus) */
    CHECK(ig1 == (int)sizeof L1,
          "[yield-corpus-frozen] live lesson L1 ingested (ring populated)");
    int wl = 0; const uint8_t *ws = cradle_window_src(&wl);
    CHECK(ws && wl == (int)sizeof L1,
          "[yield-corpus-frozen] window source is the live ring (not the fixture)");

    int cnC  = cradle_corpus_len();                 /* == sizeof L1 */
    int twC  = (cnC / ST_DMN_SEQLEN) * 3 / 4; if (twC < 2) twC = 2;
    int totC = ST_DMN_ROUNDS * twC;

    /* all-at-once reference trained on the FROZEN L1 corpus. */
    st_model refC; if (st_init(&refC, STUDENT_SEED) != ST_OK) { printf("[yield] refC OOM\n"); return 2; }
    sleep_rounds(&refC, ST_DMN_SEQLEN, twC, ST_DMN_ROUNDS, ST_DMN_LR);

    /* a FRESH resident baby for the sliced run (reset the singleton). */
    st_free(&g_student); g_have_student = 0; g_loaded_from_disk = 0;
    g_consol_active = 0; g_consol_idx = 0; cradle_lesson_freeze(0);
    if (student_ensure(0) != 0) { printf("[yield] ensure C OOM\n"); return 2; }

    int rcC, callsC = 0, deferred_rc = -999, corpus_held = 1, busy_seen = 0;
    do {
        rcC = student_dmn_consolidate();
        if (g_consol_active) {                  /* mid-batch (busy): a lesson arrives now */
            busy_seen = 1;
            deferred_rc = cradle_lesson_ingest(L2, (int)sizeof L2);  /* must DEFER */
            int wl2 = 0; const uint8_t *ws2 = cradle_window_src(&wl2);
            if (!(ws2 && wl2 == (int)sizeof L1 && memcmp(ws2, L1, sizeof L1) == 0))
                corpus_held = 0;                 /* ring must still be L1 */
        }
        callsC++;
        if (callsC > totC + 8) { printf("[yield] Cert C loop runaway\n"); break; }
    } while (rcC == 0);

    long lc_g, lc_r;
    unsigned char *bC_g = dump(&g_student, &lc_g);
    unsigned char *bC_r = dump(&refC,      &lc_r);

    printf("[yield] Cert C: live L1=%dB twC=%d totC=%d callsC=%d busy_seen=%d deferred_rc=%d\n",
           (int)sizeof L1, twC, totC, callsC, busy_seen, deferred_rc);

    CHECK(busy_seen,
          "[yield-corpus-frozen] batch actually sliced (busy observed mid-batch)");
    CHECK(deferred_rc == 0,
          "[yield-corpus-frozen] mid-batch ingest DEFERRED (return 0, not installed)");
    CHECK(corpus_held,
          "[yield-corpus-frozen] ring stayed L1 for the whole batch (corpus frozen)");
    CHECK(bC_g && bC_r && lc_g == lc_r && lc_g > 0 &&
          memcmp(bC_g, bC_r, (size_t)lc_g) == 0,
          "[yield-corpus-frozen] sliced-on-live-corpus blob == all-at-once-on-L1 (byte-identical)");

    /* the deferred lesson is NOT dropped: the batch unfroze, so the next poll
     * ingests L2 (re-pollable — the transport never advanced its high-water). */
    int ig2 = cradle_lesson_ingest(L2, (int)sizeof L2);
    CHECK(ig2 == (int)sizeof L2 && cradle_lesson_len() == (int)sizeof L2,
          "[yield-corpus-frozen] deferred lesson ingests after batch completes (not dropped)");

    free(bC_g); free(bC_r); st_free(&refC);
    cradle_lesson_clear(); cradle_lesson_freeze(0);

    printf("\n[yield] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
