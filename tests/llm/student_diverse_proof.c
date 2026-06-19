/*
 *  student_diverse_proof.c — PROVE the diverse teacher corpus makes a less
 *  repetitive baby (step ⑤, "make the baby's babble richer").
 *
 *  Distills two babies from the SAME fresh seed with the SAME geometry: one on
 *  the OLD repetitive fixture, one on the NEW diverse harvest. For each, after
 *  K sleep rounds, it reports:
 *    (a) held-out next-byte loss (nats)        — quality / generalization
 *    (b) distinct-byte-trigram ratio of a generation sample (higher = richer)
 *    (c) longest repeated byte run in that sample (lower = less degenerate)
 *  and prints a generation SAMPLE from each so the difference is visible.
 *
 *  PASS gate (honest, tiny byte baby):
 *    - new distinct-trigram ratio strictly GREATER than old, AND
 *    - new held-out loss not meaningfully worse (<= old + 0.5 nats).
 *
 *  The OLD repetitive corpus is embedded here (the exact pre-⑤ fixture). The
 *  NEW diverse corpus is the bytes now embedded as TEACHER_FIXTURE in
 *  student_shell.c; run_diverse_proof.sh EXTRACTS that literal at run time (the
 *  harvested *.bytes blobs are .gitignored, so the committed C string is the
 *  single source of truth) and passes it here as <new.bytes>.
 *
 *  Build (wave-49): -O1 -ffp-contract=off, links arch/common/llm/student.c.
 *  Usage: student_diverse_proof <new.bytes>   (OLD corpus is built in)
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../arch/common/llm/student.h"

#define SEQ        32
#define ROUNDS     8
#define LR         3e-3f
#define GEN_LEN    96       /* st_generate internal cap is ~96 */
#define SEED       0x0BABEu

/* The exact pre-⑤ repetitive fixture (was TEACHER_FIXTURE in student_shell.c),
 * embedded so this cert is self-contained even though *.bytes are gitignored. */
static const char OLD_CORPUS[] =
    "the cat sat on the mat. the dog ran in the sun. "
    "she said the sea is blue and the sky is blue too. "
    "the cat and the dog ran to the sea and sat on the sand. "
    "the sun set and the sky was red. the cat slept on the mat again. "
    "the dog ran on the sand and the cat sat in the sun by the sea. ";

static uint8_t *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)sz, f); fclose(f);
    b[got] = 0; *n = (long)got; return b;
}

static void window(uint8_t *dst, const uint8_t *corpus, long n, int off, int len)
{
    for (int i = 0; i < len; i++) dst[i] = corpus[((long)off + i) % n];
}

static float heldout(st_model *m, const uint8_t *corpus, long n, int train_end, int count)
{
    uint8_t buf[ST_MAXSEQ]; double s = 0; int got = 0;
    for (int w = 0; w < count; w++) {
        window(buf, corpus, n, train_end + w * SEQ, SEQ);
        int np = 0; float l = st_eval_loss(m, buf, SEQ, &np);
        if (np) { s += l; got++; }
    }
    return got ? (float)(s / got) : 0.0f;
}

static void train(st_model *m, const uint8_t *corpus, long n, int train_windows)
{
    uint8_t buf[ST_MAXSEQ];
    float *logits = (float *)malloc((size_t)SEQ * ST_VOCAB * sizeof(float));
    if (!logits) return;
    for (int r = 0; r < ROUNDS; r++)
        for (int w = 0; w < train_windows; w++) {
            window(buf, corpus, n, w * SEQ, SEQ);
            st_zero_grad(m);
            st_forward(m, buf, SEQ, logits);
            st_backward(m, buf, SEQ);
            st_adam_step(m, LR);
        }
    free(logits);
}

/* distinct byte-trigram ratio: |distinct trigrams| / |trigrams| in [0,1].
 * 1.0 = every trigram unique (max variety); low = repetitive. */
static double trigram_ratio(const uint8_t *s, int n)
{
    if (n < 3) return 0.0;
    int total = n - 2;
    /* hash trigrams into a set via a small open-addressed table */
    int cap = 8192; int *keys = (int *)calloc(cap, sizeof(int));
    char *used = (char *)calloc(cap, 1);
    int distinct = 0;
    for (int i = 0; i + 2 < n; i++) {
        int key = (s[i] << 16) | (s[i+1] << 8) | s[i+2];
        unsigned h = ((unsigned)key * 2654435761u) % cap;
        while (used[h] && keys[h] != key) h = (h + 1) % cap;
        if (!used[h]) { used[h] = 1; keys[h] = key; distinct++; }
    }
    free(keys); free(used);
    return (double)distinct / (double)total;
}

/* longest run of a repeated single byte (e.g. "    " or "the the" collapses to
 * char-level; we measure single-byte runs as the crudest degeneracy signal) */
static int longest_run(const uint8_t *s, int n)
{
    int best = 0, cur = 1;
    for (int i = 1; i < n; i++) {
        if (s[i] == s[i-1]) { cur++; if (cur > best) best = cur; }
        else cur = 1;
    }
    return n ? (best ? best : 1) : 0;
}

/* longest repeated SUBSTRING run found by scanning for the most repeated
 * 4-gram and counting its occurrences (cheap proxy for phrase looping). */
static int max_4gram_repeat(const uint8_t *s, int n)
{
    if (n < 4) return 0;
    int best = 1;
    for (int i = 0; i + 3 < n; i++) {
        int cnt = 1;
        for (int j = i + 1; j + 3 < n; j++)
            if (memcmp(s + i, s + j, 4) == 0) cnt++;
        if (cnt > best) best = cnt;
    }
    return best;
}

static void print_sample(const char *tag, const uint8_t *s, int n)
{
    printf("  %s sample: \"", tag);
    for (int i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c < 0x20 || c > 0x7e) c = '.';
        putchar(c);
    }
    printf("\"\n");
}

struct result { float loss; double trig; int run; int rep4; uint8_t gen[GEN_LEN]; int gn; };

static void distill_and_eval(const char *tag, const uint8_t *corpus, long n,
                             struct result *out)
{
    st_model m;
    if (st_init(&m, SEED) != ST_OK) { fprintf(stderr, "init OOM\n"); exit(1); }
    int total = (int)(n / SEQ);
    if (total < 4) total = 4;
    int train_windows = total * 3 / 4; if (train_windows < 1) train_windows = 1;
    int test_windows  = total - train_windows; if (test_windows < 1) test_windows = 1;

    train(&m, corpus, n, train_windows);
    out->loss = heldout(&m, corpus, n, train_windows * SEQ, test_windows);

    /* generate a sample from a short prompt drawn from the corpus head */
    uint8_t prompt[8];
    for (int i = 0; i < 8; i++) prompt[i] = corpus[i % n];
    out->gn = st_generate(&m, prompt, 8, out->gen, GEN_LEN, 0.8f, 40, 0xC0FFEEu);
    if (out->gn < 0) out->gn = 0;
    out->trig = trigram_ratio(out->gen, out->gn);
    out->run  = longest_run(out->gen, out->gn);
    out->rep4 = max_4gram_repeat(out->gen, out->gn);

    printf("[%s] corpus=%ld bytes  train_win=%d test_win=%d\n",
           tag, n, train_windows, test_windows);
    printf("  held-out loss      = %.4f nats\n", out->loss);
    printf("  distinct-trigram   = %.4f  (higher = richer)\n", out->trig);
    printf("  longest byte run   = %d    (lower = less degenerate)\n", out->run);
    printf("  max 4-gram repeats = %d    (lower = less phrase-looping)\n", out->rep4);
    print_sample(tag, out->gen, out->gn);

    st_free(&m);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <new.bytes>  (OLD corpus is built in)\n", argv[0]); return 2; }
    long no = (long)sizeof(OLD_CORPUS) - 1, nn = 0;
    uint8_t *old = (uint8_t *)OLD_CORPUS;
    uint8_t *neu = slurp(argv[1], &nn);
    if (!neu) return 1;

    printf("=== diverse-baby proof (seed=0x%X, %d rounds, seqlen=%d) ===\n",
           SEED, ROUNDS, SEQ);
    struct result ro, rn;
    distill_and_eval("OLD", old, no, &ro);
    printf("\n");
    distill_and_eval("NEW", neu, nn, &rn);

    printf("\n=== verdict ===\n");
    printf("  trigram   OLD %.4f -> NEW %.4f  (delta %+.4f)\n", ro.trig, rn.trig, rn.trig - ro.trig);
    printf("  loss      OLD %.4f -> NEW %.4f  (delta %+.4f)\n", ro.loss, rn.loss, rn.loss - ro.loss);
    printf("  byte-run  OLD %d -> NEW %d\n", ro.run, rn.run);
    printf("  4g-repeat OLD %d -> NEW %d\n", ro.rep4, rn.rep4);

    int richer    = rn.trig > ro.trig;
    int not_worse = rn.loss <= ro.loss + 0.5f;
    int pass = richer && not_worse;
    printf("\n[diverse-baby] %s  (richer=%d, loss-not-worse=%d)\n",
           pass ? "PASS" : "FAIL", richer, not_worse);

    free(neu);
    return pass ? 0 : 1;
}
