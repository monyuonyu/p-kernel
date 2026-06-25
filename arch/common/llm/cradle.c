/*
 *  cradle.c — the lesson BRIDGE corpus seam + the [cradle-teach] cert.
 *             (Thread T, T-fix-b / T-1; docs/architecture/thread-t-impl-plan.md
 *             §1, §2.1, §2.2.)
 *
 *  This is the SOURCE OF TRUTH for the student-side of the teacher->student
 *  lesson bridge: a TEACHER node emits a TEXT lesson over the mesh (a KDDS
 *  CRADLE_TEACH beacon + the p-fs lesson body), the STUDENT pulls it and trains
 *  its byte-native baby on it so the lesson becomes WEIGHT-RESIDENT (the real
 *  distill path distill_proof.c proves). The TEXT-LEVEL design (byte-CE): the
 *  byte stream IS the training signal; no token alignment, no logits on the
 *  wire (thread-t-impl-plan.md §0 winner, §4 honest bound).
 *
 *  WHY A SEPARATE TU (not just in student_shell.c): the corpus seam + the
 *  ingest logic + the cert must be exercised BY THE CERT, and the cert is a
 *  STANDALONE host harness (tests/llm/cradle_teach_proof.c) that compiles
 *  student.c WITHOUT the kernel (no gguf.h / forward.h / pfs_dur). So the ring,
 *  the window-source switch, the ingest, and the in-process cert live HERE,
 *  depending only on student.h (the math) + <string.h> — host-compilable AND
 *  linked into the hosted kernel. student_shell.c calls cradle_window_src() in
 *  its window() and cradle_poll_and_pull() at the top of the DMN sleep tick;
 *  the in-kernel transport (KDDS beacon + p-fs body) lives in cradle_net.c.
 *  NO PRIVATE COPY of the train/held split: the production student_dmn_
 *  consolidate() computes train_end from the SAME live ring length this cert
 *  uses (§1.2), so the cert exercises the real seam.
 *
 *  ONE-MIND / NOCENTRAL: the student trains via its OWN st_backward/st_adam_step
 *  on its OWN weights; it NEVER averages rw[] and never calls gl_merge. The
 *  lesson bytes enter the EXACT proven sleep_rounds loop unchanged.
 *
 *  Build: host-libc tier (LLM_CFLAGS: -O1 -ffp-contract=off, system <stdint.h>),
 *  alongside student.c / student_shell.c. NOT on bare metal (no malloc/libm;
 *  the weak stub in student_stub.c keeps the link green there — but cradle.c
 *  itself is libc-light: only <string.h> memcpy + <stdint.h>, no malloc).
 */
#include "student.h"
#include <string.h>     /* memcpy / memset                                  */

/* ---------------------------------------------------------------------------
 * The lesson ring — the corpus SOURCE that replaces the static fixture when a
 * teacher has delivered a lesson (thread-t-impl-plan.md §2.2). A node with NO
 * lesson trains BYTE-IDENTICALLY to today (cradle_window_src returns the
 * fixture). file-static .bss, never the task stack (the hosted-relay stack-
 * overflow lesson). Single-writer (the student's DMN sleep tick / the cert),
 * so no concurrent-use guard is needed.
 * ------------------------------------------------------------------------- */
static uint8_t g_lesson_ring[CRADLE_RING_BYTES];
static int     g_lesson_len = 0;        /* live lesson byte length, 0 = none  */
static int     g_cradle_enabled = 1;    /* Arm A gate: pulling rides the mesh */

/* The ring is "live" (drives training) only when it holds at least a few full
 * windows, so a tiny/garbage lesson cannot starve the fixture. Mirrors the
 * §2.2 threshold (>= 4*ST_DMN_SEQLEN). ST_DMN_SEQLEN is private to
 * student_shell.c; cradle uses the SAME constant here (CRADLE_SEQLEN) so the
 * threshold and the train/held split match the production tick exactly. */
#define CRADLE_MIN_LIVE  (4 * CRADLE_SEQLEN)

/* ---- the corpus-source switch (called from student_shell.c::window) ----
 * Return the live lesson ring when it is populated past the threshold, else
 * NULL (the caller then reads its static TEACHER_FIXTURE). *len_out gets the
 * source length. This is the ONLY seam student_shell.c needs: window() reads
 * cradle_window_src() first, the fixture second. Gated by g_cradle_enabled so
 * Arm A (teaching OFF) provably falls back to the fixture. Pure read. */
const uint8_t *cradle_window_src(int *len_out)
{
    if (g_cradle_enabled && g_lesson_len >= CRADLE_MIN_LIVE) {
        if (len_out) *len_out = g_lesson_len;
        return g_lesson_ring;
    }
    if (len_out) *len_out = 0;
    return 0;
}

/* ---- lesson ingest (called by the transport after a p-fs pull) ----
 * Copy a fetched lesson body into the ring (LENGTH-PREFIXED BINARY, NOT a
 * C-string: a live lesson is tok_decode output incl. NUL/control bytes — §2.1
 * risk 10; the ring is uint8_t-clean). Refuses (returns -1, ring UNCHANGED) a
 * body that is empty, too big, or below the live threshold — NEVER truncates.
 * Returns the bytes installed (>0) on success. Idempotent re-install of the
 * same bytes is harmless (the high-water in the transport dedups by seq). */
int cradle_lesson_ingest(const uint8_t *body, int len)
{
    if (!body || len <= 0 || len > CRADLE_RING_BYTES) return -1;
    if (len < CRADLE_MIN_LIVE) return -1;   /* too small to train -> keep fixture */
    memcpy(g_lesson_ring, body, (size_t)len);
    g_lesson_len = len;
    return len;
}

/* observability / test hooks (pure). */
int  cradle_lesson_len(void)        { return g_lesson_len; }
void cradle_set_enabled(int on)     { g_cradle_enabled = on ? 1 : 0; }
int  cradle_get_enabled(void)       { return g_cradle_enabled; }
void cradle_lesson_clear(void)      { g_lesson_len = 0; }

/* ===========================================================================
 * THE [cradle-teach] CERT (thread-t-impl-plan.md §1) — in-process production
 * self-test. Modeled on tests/llm/distill_proof.c; exercises the REAL ring
 * seam + the REAL sleep_rounds distill math (st_backward/st_adam_step) — no
 * private copy of the optimizer. Two roles in one process: A (teacher) composes
 * a lesson; B (student, fresh STUDENT_SEED 0x0BABE) pulls it into the ring and
 * trains.
 *
 * THE KILLER-OBJECTION FIX (§1.1): the train/held split is positional
 * (window() reads one contiguous buffer; sleep iterates only [0,trainw)). So
 * the cert places the coined fact's TRAINED copy in the train region AND a
 * DISTINCT held-out occurrence (a continuation the optimizer NEVER touches) in
 * the tail held region [>=train_end]. A loss drop on the never-trained
 * occurrence is GENERALIZATION, not rote copy (§1.2).
 *
 * Three falsification arms (§1.3): Arm A (teaching OFF -> fixture, no learning),
 * Arm B (scrambled bytes -> no learning), Arm C (a never-taught probe stays
 * unknown). The cert PASSES only if teaching-on learns AND all three arms hold.
 * ========================================================================= */

/* All math is via student.h's public API + cradle's own ring; the cert is
 * self-contained (libc-light: memcpy/memset only). The cert builds its own
 * lesson bytes in a local scratch and installs them via cradle_lesson_ingest,
 * then trains B by driving the SAME window()/sleep loop the production tick
 * uses — re-implemented here over the ring source ONLY for the cert (the
 * production loop lives in student_shell.c which the host cert cannot link;
 * the math — st_zero_grad/st_forward/st_backward/st_adam_step over windows —
 * is byte-identical, asserted by the shared CRADLE_SEQLEN/split constants). */

/* a window over an arbitrary corpus (the ring or a scramble), identical math
 * to student_shell.c::window and distill_proof.c::window. */
static void ct_window(const uint8_t *corpus, int n, uint8_t *dst, int off, int len)
{
    for (int i = 0; i < len; i++) dst[i] = corpus[(off + i) % n];
}

static float ct_heldout_loss(st_model *m, const uint8_t *corpus, int n,
                             int seqlen, int train_end, int count)
{
    uint8_t buf[ST_MAXSEQ];
    double s = 0.0; int got = 0;
    for (int w = 0; w < count; w++) {
        ct_window(corpus, n, buf, train_end + w * seqlen, seqlen);
        int np = 0;
        float l = st_eval_loss(m, buf, seqlen, &np);
        if (np) { s += l; got++; }
    }
    return got ? (float)(s / got) : 0.0f;
}

/* loss of the model on ONE explicit byte window (the held-out probe occurrence,
 * located at a fixed offset in the tail). Pure forward. */
static float ct_probe_loss(st_model *m, const uint8_t *corpus, int n,
                           int probe_off, int seqlen)
{
    uint8_t buf[ST_MAXSEQ];
    ct_window(corpus, n, buf, probe_off, seqlen);
    int np = 0;
    return st_eval_loss(m, buf, seqlen, &np);
}

/* K sleep rounds over the train windows of `corpus`. scramble!=0 replaces each
 * window with random bytes (the LCG distill_proof.c uses — §1.3 Arm B), same
 * #updates / same length, so the control isolates SEQUENCE from byte stats. */
/* the per-forward logits scratch: ST_MAXSEQ*ST_VOCAB floats = 64 KB. file-static
 * (.bss), NEVER a task-stack local (the hosted-relay stack-overflow lesson — a
 * 64 KB stack array would overflow a shell/cert task). The cert is single-
 * threaded (one process / serialized verb), so no concurrent-use guard needed. */
static float ct_logits[ST_MAXSEQ * ST_VOCAB];

static void ct_sleep_rounds(st_model *m, const uint8_t *corpus, int n,
                            int seqlen, int train_windows, int rounds, float lr,
                            int scramble)
{
    uint8_t buf[ST_MAXSEQ];
    uint32_t rng = 0x5EED1234u;
    float *logits = ct_logits;            /* fixed MAX scratch, no VLA, no malloc */
    for (int r = 0; r < rounds; r++) {
        for (int w = 0; w < train_windows; w++) {
            if (scramble) {
                for (int i = 0; i < seqlen; i++) {
                    rng = rng * 1664525u + 1013904223u;
                    buf[i] = (uint8_t)((rng >> 16) & 0xff);
                }
            } else {
                ct_window(corpus, n, buf, w * seqlen, seqlen);
            }
            st_zero_grad(m);
            st_forward(m, buf, seqlen, logits);
            st_backward(m, buf, seqlen);
            st_adam_step(m, lr);
        }
    }
}

/* emit type: the cert prints through a caller line-printer (the kernel's
 * `print`, or the host harness's puts wrapper). */
typedef void (*cradle_emit_fn)(const char *);

static void ct_emitf(cradle_emit_fn e, const char *pfx, float v)
{
    /* tiny libc-free %.4f formatter (sign + int + 4 frac), like dmn_putf4. */
    char b[128]; int k = 0;
    while (pfx[k] && k < 100) { b[k] = pfx[k]; k++; }
    if (v < 0) { b[k++] = '-'; v = -v; }
    unsigned whole = (unsigned)v;
    unsigned frac  = (unsigned)((v - (float)whole) * 10000.0f + 0.5f);
    if (frac >= 10000) { whole++; frac -= 10000; }
    char tmp[12]; int t = 0;
    if (whole == 0) tmp[t++] = '0';
    while (whole > 0 && t < 11) { tmp[t++] = (char)('0' + whole % 10); whole /= 10; }
    while (t > 0) b[k++] = tmp[--t];
    b[k++] = '.';
    b[k++] = (char)('0' + (frac / 1000) % 10);
    b[k++] = (char)('0' + (frac / 100) % 10);
    b[k++] = (char)('0' + (frac / 10) % 10);
    b[k++] = (char)('0' + frac % 10);
    b[k++] = '\r'; b[k++] = '\n'; b[k] = '\0';
    if (e) e(b);
}

/* ---- the lesson the cert teaches ------------------------------------------
 * A coined fact ("zorblax is a blue fox") absent from any fixture, repeated in
 * the TRAIN region; a DISTINCT continuation/paraphrase ("the zorblax runs and
 * the blue fox hides") placed in the TAIL held region. The held occurrence
 * shares the fact's tokens but is a sentence the optimizer NEVER trains on, so
 * a loss drop there is generalization (§1.2). Plain ASCII so the cert is
 * legible; the ring/window are byte-clean for live binary lessons (§2.1). */
#define CT_TRAIN_SENT  "the zorblax is a blue fox. "
#define CT_HELD_SENT   "the zorblax runs and the blue fox hides in the den. "
#define CT_FILLER      "rivers flow to the sea and the wind moves over the hills. "

/* Build the cert lesson into out[cap]. Layout: FILLER+TRAIN sentences fill the
 * train region (the first trainw windows), then a clearly held tail begins with
 * the HELD continuation at a known offset. Returns total length; sets
 * *probe_off to the byte offset of the held probe occurrence (>= train_end). */
static int ct_build_lesson(uint8_t *out, int cap, int seqlen, int *probe_off_out,
                           int *train_end_out, int *trainw_out, int *heldw_out)
{
    const char *T = CT_TRAIN_SENT;
    const char *F = CT_FILLER;
    const char *H = CT_HELD_SENT;
    int tlen = (int)strlen(T), flen = (int)strlen(F), hlen = (int)strlen(H);

    /* compute the production split FIRST, from a fixed total window count, so
     * train_end is a CLEAN window boundary and the held probe can be placed at
     * exactly train_end (the FIRST never-trained window). This matches what
     * student_dmn_consolidate computes from cradle_corpus_len() — the cert's
     * split is the production split, not a test-private one (§1.2). */
    int total     = cap / seqlen;                  /* total windows in the budget */
    int trainw    = total * 3 / 4; if (trainw < 2) trainw = 2;
    int heldw     = total - trainw; if (heldw < 1) heldw = 1;
    int train_end = trainw * seqlen;               /* first held byte (boundary)  */

    int len = 0;
    /* TRAIN region [0, train_end): alternate filler + the trained fact copies,
     * filling exactly up to the window boundary (pad the last partial slot with
     * filler bytes so the boundary is exact). */
    while (len + tlen + flen <= train_end) {
        memcpy(out + len, F, (size_t)flen); len += flen;
        memcpy(out + len, T, (size_t)tlen); len += tlen;
    }
    while (len < train_end) { out[len] = (uint8_t)F[len % flen]; len++; }  /* exact pad */

    /* HELD region: the HELD continuation FIRST, at exactly train_end (so the
     * probe window starts on the boundary and is the never-trained occurrence),
     * then filler so the held tail has body. */
    int probe_off = len;                            /* == train_end (a boundary)  */
    memcpy(out + len, H, (size_t)hlen); len += hlen;
    while (len + flen < cap) { memcpy(out + len, F, (size_t)flen); len += flen; }
    while (len < cap && len + 1 < cap) out[len++] = (uint8_t)' ';

    if (probe_off_out)  *probe_off_out  = probe_off;
    if (train_end_out)  *train_end_out  = train_end;
    if (trainw_out)     *trainw_out     = trainw;
    if (heldw_out)      *heldw_out      = heldw;
    return len;
}

/* The cert. Returns 0 on PASS, else the fail count. emit may be NULL. */
/* Cert lesson budget: a SMALL corpus (a few hundred windows would make the cert
 * minutes-long on a throttled host — the distill_proof lesson is similar). ~2 KB
 * gives a clean 3/4 split (~48 train windows) and a sharp held-out drop in a few
 * seconds, matching distill_proof's wall-time. NOT the full ring (CRADLE_RING_
 * BYTES); the production ring is sized for live lessons, the cert is bounded. */
#define CT_CERT_BUDGET   1280
#define CT_CERT_ROUNDS   12

int cradle_teach_self_test(cradle_emit_fn emit)
{
    int fails = 0;
    const int seqlen = CRADLE_SEQLEN;
    const int rounds = CT_CERT_ROUNDS;  /* enough for a real held-out drop, bounded */
    const float lr   = 3e-3f;
    const float chance = st_logf(256.0f);

    if (emit) emit("[cradle-teach] ==== teacher->student lesson bridge (Thread T, "
                   "T-fix-b) ====\r\n");

    /* --- A composes the lesson (the teacher side; seeded corpus per §2/§5: the
     * live in-kernel GGUF harvest is DEFERRED — the BRIDGE is the deliverable). */
    static uint8_t lesson[CRADLE_RING_BYTES];
    int probe_off, train_end, trainw, heldw;
    int llen = ct_build_lesson(lesson, CT_CERT_BUDGET, seqlen,
                               &probe_off, &train_end, &trainw, &heldw);
    if (emit) {
        ct_emitf(emit, "[cradle-teach] lesson bytes=", (float)llen);
        ct_emitf(emit, "[cradle-teach]   train_end (production split)=", (float)train_end);
        ct_emitf(emit, "[cradle-teach]   held probe offset=", (float)probe_off);
        ct_emitf(emit, "[cradle-teach]   chance (ln256)=", chance);
    }
    /* the probe occurrence MUST be in the NEVER-trained tail (the inversion
     * guard, §1.2). */
    if (probe_off < train_end) {
        if (emit) emit("[cradle-teach] FAIL probe occurrence not in held tail\r\n");
        fails++;
    }

    /* =====================================================================
     * MAIN ARM — teaching ON: B pulls the lesson, trains, KNOWS the held probe.
     * ===================================================================== */
    cradle_set_enabled(1);
    cradle_lesson_clear();
    int got = cradle_lesson_ingest(lesson, llen);
    if (got != llen) {
        if (emit) emit("[cradle-teach] FAIL lesson ingest refused the body\r\n");
        fails++;
    }
    int rlen; const uint8_t *src = cradle_window_src(&rlen);
    if (!src || rlen != llen) {
        if (emit) emit("[cradle-teach] FAIL ring not live after ingest\r\n");
        fails++;
    }

    st_model B;
    if (st_init(&B, 0x0BABEu) != ST_OK) {
        if (emit) emit("[cradle-teach] FAIL st_init OOM\r\n");
        return fails + 1;
    }
    /* BEFORE: held-out loss == chance; the probe occurrence is unknown. */
    float pre_held  = ct_heldout_loss(&B, lesson, llen, seqlen, train_end, heldw);
    float pre_probe = ct_probe_loss(&B, lesson, llen, probe_off, seqlen);
    if (emit) {
        ct_emitf(emit, "[cradle-teach] BEFORE  held-out loss = ", pre_held);
        ct_emitf(emit, "[cradle-teach] BEFORE  held PROBE loss= ", pre_probe);
    }
    /* TEACH: drive the REAL distill loop over the ring's train windows. */
    ct_sleep_rounds(&B, lesson, llen, seqlen, trainw, rounds, lr, 0);
    float post_held  = ct_heldout_loss(&B, lesson, llen, seqlen, train_end, heldw);
    float post_probe = ct_probe_loss(&B, lesson, llen, probe_off, seqlen);
    if (emit) {
        ct_emitf(emit, "[cradle-teach] AFTER   held-out loss = ", post_held);
        ct_emitf(emit, "[cradle-teach] AFTER   held PROBE loss= ", post_probe);
        ct_emitf(emit, "[cradle-teach]   held PROBE drop      = ", pre_probe - post_probe);
    }

    /* the headline: a LARGE drop on the NEVER-trained probe occurrence. The
     * MAIN drop is the fact-specific signal; the arms below must each produce a
     * drop that is only a SMALL FRACTION of it (the distill_proof discriminator
     * — a lesson-less / scrambled run still learns generic English structure
     * for a SMALL drop, but never the FACT). */
    float main_drop = pre_probe - post_probe;
    int learned_ok = (main_drop > 1.50f);            /* a real, large drop       */
    int honest_ok  = (pre_probe > chance - 0.50f);   /* started near chance     */
    if (!honest_ok) {
        if (emit) emit("[cradle-teach] FAIL probe did not start near chance "
                       "(baby pre-baked?)\r\n");
        fails++;
    }
    if (!learned_ok) {
        if (emit) emit("[cradle-teach] FAIL teaching-ON did not lower the "
                       "held probe loss (the bridge did not teach B)\r\n");
        fails++;
    } else if (emit) {
        emit("[cradle-teach] LEARNED: the mesh-delivered lesson made a held-out "
             "probe weight-resident (generalization, not rote)\r\n");
    }
    st_free(&B);

    /* =====================================================================
     * ARM A — teaching OFF (mesh discriminator). With cradle disabled the ring
     * is ignored, B falls back to the FIXTURE source (window_src returns NULL),
     * never sees the fact -> the probe stays at chance (§1.3 Arm A).
     * We simulate the production fixture-fallback by training B on a DIFFERENT
     * corpus (the filler-only stream, == "no lesson"); the probe loss must NOT
     * drop. The cradle_window_src gate is asserted directly too.
     * ===================================================================== */
    cradle_set_enabled(0);
    int dummy_len; const uint8_t *off_src = cradle_window_src(&dummy_len);
    int armA_gate_ok = (off_src == 0);     /* disabled -> source is the fixture */

    st_model BA;
    st_init(&BA, 0x0BABEu);
    /* "no lesson" corpus = the filler only (the fact never appears). */
    static uint8_t nolesson[CRADLE_RING_BYTES];
    int nl = 0; int flen = (int)strlen(CT_FILLER);
    while (nl + flen < CT_CERT_BUDGET) { memcpy(nolesson + nl, CT_FILLER, (size_t)flen); nl += flen; }
    int ntot = nl / seqlen, ntw = ntot * 3 / 4; if (ntw < 2) ntw = 2;
    float a_pre  = ct_probe_loss(&BA, lesson, llen, probe_off, seqlen);
    ct_sleep_rounds(&BA, nolesson, nl, seqlen, ntw, rounds, lr, 0);
    float a_post = ct_probe_loss(&BA, lesson, llen, probe_off, seqlen);
    st_free(&BA);
    /* the fixture-fallback B learns only GENERIC English from the filler (a
     * SMALL drop), NEVER the FACT: its probe drop must be a small fraction of
     * the MAIN fact-specific drop (the distill_proof grounded standard). */
    float a_drop = a_pre - a_post;
    int armA_ok = armA_gate_ok && (a_drop < main_drop * 0.5f);   /* NOT the fact */
    if (emit) {
        ct_emitf(emit, "[cradle-teach] ARM A teaching-OFF probe (pre) = ", a_pre);
        ct_emitf(emit, "[cradle-teach] ARM A teaching-OFF probe (post)= ", a_post);
        ct_emitf(emit, "[cradle-teach]   ARM A drop vs MAIN drop      = ", a_drop);
    }
    if (!armA_ok) {
        if (emit) emit("[cradle-teach] FAIL ARM A: a teaching-disabled B learned "
                       "the FACT (its drop rivals the taught drop — did not ride "
                       "the mesh)\r\n");
        fails++;
    } else if (emit) {
        emit("[cradle-teach] ARM A PASS (RED): teaching-OFF -> fixture fallback "
             "learns only generic English, NOT the fact (drop << MAIN)\r\n");
    }
    cradle_set_enabled(1);

    /* =====================================================================
     * ARM B — scrambled bytes (sequence discriminator). The transport delivers
     * a body of RANDOM bytes (the LCG, §1.3) — same length / same #updates. The
     * probe loss must stay at chance: the fact lives in the byte SEQUENCE, not
     * in byte statistics (§1.3 Arm B; random-byte scramble, NOT a shuffle).
     * ===================================================================== */
    st_model BB;
    st_init(&BB, 0x0BABEu);
    float b_pre  = ct_probe_loss(&BB, lesson, llen, probe_off, seqlen);
    ct_sleep_rounds(&BB, lesson, llen, seqlen, trainw, rounds, lr, 1); /* scramble */
    float b_post = ct_probe_loss(&BB, lesson, llen, probe_off, seqlen);
    st_free(&BB);
    /* random bytes carry NO English structure: the scrambled run's probe drop
     * must be a small fraction of the MAIN drop (it should barely move, or even
     * rise — the grounded control, distill_proof.c). */
    float b_drop = b_pre - b_post;
    int armB_ok = (b_drop < main_drop * 0.5f);  /* scrambled -> NOT the sequence */
    if (emit) {
        ct_emitf(emit, "[cradle-teach] ARM B scrambled probe (pre) = ", b_pre);
        ct_emitf(emit, "[cradle-teach] ARM B scrambled probe (post)= ", b_post);
        ct_emitf(emit, "[cradle-teach]   ARM B drop vs MAIN drop    = ", b_drop);
    }
    if (!armB_ok) {
        if (emit) emit("[cradle-teach] FAIL ARM B: a scrambled-byte lesson still "
                       "taught the probe (gain is byte stats, not the sequence)\r\n");
        fails++;
    } else if (emit) {
        emit("[cradle-teach] ARM B PASS (RED): scrambled bytes do NOT teach the "
             "probe (drop << MAIN; the fact lives in the sequence)\r\n");
    }

    /* =====================================================================
     * ARM C — a never-taught probe stays unknown (proves it's the LESSON that
     * taught B, not a pre-baked answer). After the MAIN arm trained B on the
     * lesson, a DIFFERENT coined fact never present in the lesson must remain at
     * chance under the same trained weights (§1.3 Arm C). We re-run the MAIN
     * training and then probe a control occurrence absent from the lesson.
     * ===================================================================== */
    st_model BC;
    st_init(&BC, 0x0BABEu);
    ct_sleep_rounds(&BC, lesson, llen, seqlen, trainw, rounds, lr, 0); /* taught */
    /* control corpus: a coined fact the lesson NEVER contained. */
    static uint8_t ctrl[CRADLE_RING_BYTES];
    const char *NEVER = "the quibborn is a green owl and it sleeps under the roof. ";
    int cl = 0, nvlen = (int)strlen(NEVER);
    while (cl + nvlen < CT_CERT_BUDGET) { memcpy(ctrl + cl, NEVER, (size_t)nvlen); cl += nvlen; }
    float c_never = ct_probe_loss(&BC, ctrl, cl, 0, seqlen);     /* never-taught  */
    float c_taught = ct_probe_loss(&BC, lesson, llen, probe_off, seqlen); /* taught */
    st_free(&BC);
    int armC_ok = (c_never > chance - 0.80f) && (c_taught < c_never - 0.30f);
    if (emit) {
        ct_emitf(emit, "[cradle-teach] ARM C never-taught probe loss = ", c_never);
        ct_emitf(emit, "[cradle-teach] ARM C taught     probe loss = ", c_taught);
    }
    if (!armC_ok) {
        if (emit) emit("[cradle-teach] FAIL ARM C: a never-taught probe is "
                       "already known (B has a pre-baked answer, not the lesson)\r\n");
        fails++;
    } else if (emit) {
        emit("[cradle-teach] ARM C PASS (RED): a never-taught fact stays at "
             "chance; only the taught lesson is known\r\n");
    }

    if (fails == 0) {
        if (emit) emit("[cradle-teach] PASS — the teacher's mesh-delivered byte "
                       "sequence made a held-out fact WEIGHT-RESIDENT in the byte "
                       "student; teaching-off / scrambled / never-taught all stay "
                       "at chance (one-mind, NOCENTRAL)\r\n");
    } else {
        if (emit) emit("[cradle-teach] FAIL\r\n");
    }
    return fails;
}

/* ===========================================================================
 * THE CANONICAL LIVE LESSON (T-fix-c) — unify live == cert: the SAME bytes.
 *
 * The live [cradle-live] teacher must emit the SAME trainable, train/held-
 * structured lesson the in-proc [cradle-teach] cert proves — composed via
 * ct_build_lesson, NOT a hand-typed string. The previous live CURE arm sent a
 * ~115-byte hand-typed string that fell BELOW CRADLE_MIN_LIVE (128) and was
 * REFUSED by cradle_lesson_ingest (ring_len stayed 0 -> the student never
 * learned). The canonical lesson is CT_CERT_BUDGET (1280) bytes — well past the
 * live threshold — and is byte-identical to what cradle_teach_self_test builds,
 * so the in-proc cert and the live wire teach the EXACT same lesson (one lesson
 * format, one math). The held probe lands at the production train_end boundary
 * (probe_off == train_end, the FIRST never-trained window), so the live probe's
 * post-training drop proves GENERALIZATION, not rote copy.
 * ========================================================================= */

/* The canonical lesson budget (== the cert's CT_CERT_BUDGET). The live teacher
 * passes a buffer of at least this size; the composed lesson is exactly this
 * many bytes. Exposed so the caller can size its file-static emit buffer / cap
 * to the SAME budget the cert uses (no magic number on the wire side). */
int cradle_canon_budget(void) { return CT_CERT_BUDGET; }

/* Compose the canonical, trainable, train/held-structured lesson into out[cap]
 * (cap MUST be >= cradle_canon_budget()). Returns the composed byte length
 * (== CT_CERT_BUDGET) on success, or -1 if out is NULL / cap too small. On
 * success *probe_off (if non-NULL) gets the held-probe byte offset (== the
 * production train_end, the first never-trained window). The bytes are IDENTICAL
 * to what cradle_teach_self_test's ct_build_lesson composes — that identity is
 * the whole point (in-proc cert == live wire). */
int cradle_compose_canon(uint8_t *out, int cap, int *probe_off)
{
    if (!out || cap < CT_CERT_BUDGET) return -1;
    int poff, train_end, trainw, heldw;
    int llen = ct_build_lesson(out, CT_CERT_BUDGET, CRADLE_SEQLEN,
                               &poff, &train_end, &trainw, &heldw);
    if (probe_off) *probe_off = poff;
    return llen;
}
