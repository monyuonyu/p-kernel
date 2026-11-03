/*
 * b1_held_test.c — [b1-held] (ROADMAP B1, 0-12; design: held-independence.md "B1")
 *
 * Bits per byte of the student on an independent held text
 * (tests/llm/b1_held.txt, 10 KB written for this and never trained on),
 * after the training the `student` verb does (8 rounds, lr ST_LR, seqlen 32,
 * the fixture's train windows). Expectations fixed before the first run:
 *
 *   B1  the held text is >= 8192 bytes of printable ASCII and newlines
 *   B2  it shares no 16-byte run with anything a node is taught: the whole
 *       TEACHER_FIXTURE and the whole canonical cradle lesson
 *   B3  it shares no 16-byte run with the bytes this test trained on
 *       (-DB1_LEAK also trains on the first half of the held text: B3 RED)
 *   B4  bpb before and after is a number in (0, 16) over >= 256 held windows,
 *       and every window is predicted (no window skipped)
 *   The weights are never chosen with the held text: this test evaluates it
 *   only after the last training step, and run_b1_held.sh checks that no
 *   file under arch/ or boot/ names it (the kernel cannot read it).
 *
 * The value is the result, not a gate (B1 is a ruler): "bpb after" may be
 * worse than chance (8.0) - the fixture's own held loss went above chance
 * once its held tail stopped repeating the train filler (0-12, e492a0b4).
 *
 *   b1_held_test <held.txt> [tier 0|1|2] [rounds]    exit 0 = B1..B4 PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../../arch/common/llm/student.h"
#include "../../arch/common/llm/held_indep.h"
#include "../../arch/common/llm/teacher_fixture.h"

#define SEQ 32
#define LN2 0.69314718055994530942

static uint8_t held[65536];
static int fails;

static void item(int ok, const char *id, const char *what)
{
    printf("[b1-held] %s %s %s\n", ok ? "PASS" : "FAIL", id, what);
    if (!ok) fails++;
}

/* all 16-byte runs of held[0..n) against seen[0..sn): one window of n bytes */
static int shared_runs(const uint8_t *seen, int sn, int n)
{
    st_held_report r;
    int rc = st_held_check_seen(held, n, n, 0, 1, seen, sn, ST_HELD_K, &r);
    return rc < 0 ? -1 : r.shared;
}

/* mean bits per byte over the non-overlapping SEQ windows of held[0..n) */
static double bpb(st_model *m, int n, int *wins, int *skipped)
{
    double s = 0.0; int got = 0, skip = 0;
    for (int w = 0; (w + 1) * SEQ <= n; w++) {
        int np = 0;
        float l = st_eval_loss(m, held + w * SEQ, SEQ, &np);
        if (np) { s += l; got++; } else skip++;
    }
    *wins = got; *skipped = skip;
    return got ? s / got / LN2 : 0.0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <held.txt> [tier] [rounds]\n", argv[0]); return 2; }
    int tier = argc > 2 ? atoi(argv[2]) : ST_TIER_S;
    int rounds = argc > 3 ? atoi(argv[3]) : 8;
    float lr = ST_LR;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    int n = (int)fread(held, 1, sizeof held, f);
    fclose(f);

    int ascii = 1;
    for (int i = 0; i < n; i++)
        if (!(held[i] == '\n' || (held[i] >= 32 && held[i] < 127))) ascii = 0;
    printf("[b1-held] held text %d B, %d windows of %d\n", n, n / SEQ, SEQ);
    item(n >= 8192 && ascii, "B1", "held text is >= 8192 B of printable ASCII");

    const uint8_t *fx = (const uint8_t *)TEACHER_FIXTURE;
    int fxn = (int)sizeof(TEACHER_FIXTURE) - 1;
    static uint8_t canon[8192];
    int cn = cradle_compose_canon(canon, (int)sizeof canon, NULL);
    int s_fx = shared_runs(fx, fxn, n), s_cn = shared_runs(canon, cn, n);
    printf("[b1-held] shared 16-byte runs: fixture %d B -> %d, cradle lesson %d B -> %d\n",
           fxn, s_fx, cn, s_cn);
    item(s_fx == 0 && s_cn == 0, "B2", "no 16-byte run in common with the fixture or the cradle lesson");

    /* train as the `student` verb does: the fixture's train windows */
    st_model m;
    if (st_init_tier(&m, 1u, tier) != 0) { printf("[b1-held] FAIL init tier %d\n", tier); return 1; }
    int wb = 0, kb = 0;
    double before = bpb(&m, n, &wb, &kb);
    int fx_end = st_held_split(fxn, SEQ).train_end;
#ifdef B1_LEAK
    int leak_end = (n / 2 / SEQ) * SEQ;     /* the first half of the held text */
#else
    int leak_end = 0;
#endif
    float *logits = (float *)malloc((size_t)SEQ * ST_VOCAB * sizeof(float));
    if (!logits) return 2;
    for (int r = 0; r < rounds; r++) {
        for (int o = 0; o + SEQ <= fx_end; o += SEQ) ST_TRAIN_STEP(&m, fx + o, SEQ, logits, lr);
        for (int o = 0; o + SEQ <= leak_end; o += SEQ) ST_TRAIN_STEP(&m, held + o, SEQ, logits, lr);
    }
    free(logits);
    int s_tr = shared_runs(fx, fx_end, n);
    int s_lk = leak_end ? shared_runs(held, leak_end, n) : 0;
    printf("[b1-held] trained: fixture 0..%d%s, %d rounds, lr %.0e -> shared %d + %d\n",
           fx_end, leak_end ? " + held 0..half (B1_LEAK)" : "", rounds, (double)lr, s_tr, s_lk);
    item(s_tr == 0 && s_lk == 0, "B3", "no 16-byte run in common with the bytes trained");

    int wa = 0, ka = 0;
    double after = bpb(&m, n, &wa, &ka);
    /* the fixture's own held windows, for context (the number 0-12 printed) */
    double fh = 0.0; int fw = 0;
    for (int o = fx_end; o + SEQ <= fxn; o += SEQ) {
        int np = 0; float l = st_eval_loss(&m, fx + o, SEQ, &np);
        if (np) { fh += l; fw++; }
    }
    fh = fw ? fh / fw / LN2 : 0.0;
    printf("[b1-held] tier %d bpb before=%.4f after=%.4f (chance 8.0000) windows=%d skipped=%d; fixture held after=%.4f (%d windows)\n",
           tier, before, after, wa, ka, fh, fw);
    /* comparators on the same predicted bytes (not gates): byte unigram and
     * bigram counted from the trained fixture bytes, add-one smoothed */
    {
        static double c1[256], c2[256][256];
        double t1 = 0;
        for (int i = 0; i < fx_end; i++) { c1[fx[i]] += 1; t1 += 1; }
        for (int i = 0; i + 1 < fx_end; i++) c2[fx[i]][fx[i + 1]] += 1;
        double u = 0, b = 0; int np = 0;
        for (int w = 0; (w + 1) * SEQ <= n; w++)
            for (int t = 1; t < SEQ; t++) {
                uint8_t p = held[w * SEQ + t - 1], c = held[w * SEQ + t];
                u += -log((c1[c] + 1) / (t1 + 256));
                b += -log((c2[p][c] + 1) / (c1[p] + 256));
                np++;
            }
        printf("[b1-held] comparators on the same bytes: unigram=%.4f bigram=%.4f bpb (from the trained fixture bytes, add-one)\n",
               u / np / LN2, b / np / LN2);
    }
    item(wb >= 256 && wa >= 256 && kb == 0 && ka == 0 &&
         isfinite(before) && isfinite(after) && before > 0 && before < 16 && after > 0 && after < 16,
         "B4", "bpb before and after measured over every held window");
    st_free(&m);
    printf("[b1-held] %s\n", fails ? "RED" : "ALL PASS");
    return fails ? 1 : 0;
}
