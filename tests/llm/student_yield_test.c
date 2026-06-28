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

/* cradle transport: no live lesson -> window() reads TEACHER_FIXTURE (the no-op
 * contract; the byte-identity argument relies on the corpus being stable). */
const uint8_t *cradle_window_src(int *len_out) { if (len_out) *len_out = 0; return NULL; }
int            cradle_lesson_len(void)         { return 0; }

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
    do {
        int before = g_consol_idx;        /* snapshot resets to 0 on call 1 */
        rc = student_dmn_consolidate();
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
    printf("[yield] single-triple ~%.3f ms -> K=%d call ~%.2f ms (budget ~50ms vs 1000ms pulse)\n",
           per_triple_ms, K, per_triple_ms * K);

    CHECK(max_call <= K,
          "[yield-responsive] every consolidate() call advances <= K passes (node yields)");
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

    printf("\n[yield] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
