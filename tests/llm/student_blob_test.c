/*
 *  student_blob_test.c — in-process cert for the SS-3 student-blob transport
 *  (docs/architecture/student-blob-transport.md §3, STEP 1, the gate).
 *
 *  Proves the content-addressed manifest transport (gl_student_publish /
 *  gl_student_fetch, the _TK_HOSTED_LIBC_ bodies) round-trips a REAL S-tier
 *  student blob BIT-EXACTLY in ONE process with ZERO network (SOLO mode,
 *  drpc_my_node == 0xFF), then hands the recovered blob to the existing crown
 *  math (st_merge_cohort) UNCHANGED.
 *
 *  Assertions:
 *    [ss3-blob-roundtrip]          publish(blobA) -> fetch -> memcmp==0 over
 *                                  all chunks + 2-level index (depth 2).
 *    [ss3-blob-merge]              the recovered blob is a well-formed S model
 *                                  (st_blob_tier_ok), merges (accepted==1), the
 *                                  merged loss <= the worse parent, the merge
 *                                  is peer-symmetric, AND merging the TRANSPORTED
 *                                  blob is BYTE-IDENTICAL to merging the DIRECT
 *                                  blob (transport fidelity at the merge level).
 *    [ss3-blob-roundtrip-falsify]  a dropped chunk -> fetch returns <0 and
 *                                  leaves `out` UNCONSUMED (fail closed).
 *    [ss3-blob-refuse-toobig]      a blob whose chunk count exceeds the cap is
 *                                  REFUSED (-1), never truncated.
 *
 *  KEY CODEBASE FACT (student-blob-transport.md §0, verified here): the P0
 *  block store is PFS_MAX_BLOCKS=64, but the S blob is 482 chunks. A memory-
 *  only store therefore CANNOT hold a whole student blob — pfs_put returns
 *  PFS_E_FULL at the 65th distinct block. So this cert mounts the eviction-
 *  capable durable ARK backend (PKERNEL_PFS_BACKEND=ark): RAM is a 64-slot
 *  cache, every block is also in the ARK log, and a pfs_get cache-miss falls
 *  through to ARK. The transport bytes are unchanged (content-addressed), so
 *  recovery is still bit-exact. (See the runner's [finding] note.)
 *
 *  Build: -O1 -ffp-contract=off (one math everywhere).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "student.h"        /* arch/common/llm/student.h (plain libc world) */

/* ---- transport + durable hooks (plain-C externs; NO kernel headers here,
 *      so this TU stays in the libc world like the other student certs).
 *      UB=unsigned char, UW=unsigned int, INT=int (include/typedef.h). ---- */
extern int  gl_student_publish(unsigned char node, const void *blob, unsigned int len);
extern int  gl_student_fetch(unsigned char node, void *out, unsigned int cap);
extern void gl_student_test_drop_chunk(int idx);
extern int  pfs_durable_restore(void (*emit)(const char *));
extern int  pfs_durable_active(void);

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

static void emit(const char *s) { fputs(s, stdout); }

/* shared SS-3 corpus (same family as student_cohort_test.c). */
static const uint8_t CORPUS[] =
    "the cat sat on the mat. the dog ran in the sun. she said the sea is blue "
    "and the sky is blue too. a bird sang on the old oak tree at dawn. the cat "
    "and the dog sat by the sea and saw the sun set red. the old man ran to the "
    "sea to see the red sun set on the blue sea at the end of the day.";

#define SEQLEN 24

static int   g_train_windows;
static int   g_held_from;
static int   g_held_windows;
static float *g_logits;

/* train an S-tier model on the corpus with a per-step visit-order shift. */
static void train_s(st_model *m, uint32_t seed, int shift, int rounds, float lr)
{
    st_init_tier(m, seed, ST_TIER_S);
    for (int r = 0; r < rounds; r++) {
        for (int j = 0; j < g_train_windows; j++) {
            int w = (j * shift + 3) % g_train_windows;
            uint8_t b[SEQLEN];
            for (int i = 0; i < SEQLEN; i++) b[i] = CORPUS[w * SEQLEN + i];
            st_zero_grad(m);
            st_forward(m, b, SEQLEN, g_logits);
            st_backward(m, b, SEQLEN);
            st_adam_step(m, lr);
        }
    }
}

static float heldout(st_model *m)
{
    float tot = 0.0f; int got = 0;
    for (int w = 0; w < g_held_windows; w++) {
        int off = g_held_from + w * SEQLEN;
        if (off + SEQLEN + 1 > (int)sizeof(CORPUS) - 1) break;
        uint8_t b[SEQLEN];
        for (int i = 0; i < SEQLEN; i++) b[i] = CORPUS[off + i];
        int np = 0;
        tot += st_eval_loss(m, b, SEQLEN, &np);
        got++;
    }
    return got ? tot / (float)got : 0.0f;
}

int main(void)
{
    printf("=== SS-3 student-blob transport cert (student-blob-transport.md §3) ===\n\n");

    /* ---- mount the eviction-capable durable backend (see header note) ---- */
    int dur = pfs_durable_restore(emit);
    if (!pfs_durable_active()) {
        printf("[setup] FAIL durable backend not active — a 482-chunk blob cannot\n");
        printf("        fit the 64-slot P0 store without it (set PKERNEL_PFS_BACKEND=ark).\n");
        return 2;
    }
    printf("[setup] durable ARK backend active (restored %d block(s))\n\n", dur);

    int total_windows = ((int)sizeof(CORPUS) - 1) / SEQLEN;
    g_train_windows = total_windows * 3 / 4; if (g_train_windows < 2) g_train_windows = 2;
    g_held_from = g_train_windows * SEQLEN;
    g_held_windows = total_windows - g_train_windows; if (g_held_windows < 1) g_held_windows = 1;
    g_logits = (float *)malloc((size_t)SEQLEN * ST_VOCAB * sizeof(float));
    if (!g_logits) { printf("OOM\n"); return 2; }

    const int ROUNDS = 40; const float LR = 0.02f;

    /* model A (seed C0FFEE, order 1) — the published blob. */
    st_model A; train_s(&A, 0xC0FFEE, 1, ROUNDS, LR);
    float lossA = heldout(&A);
    size_t cap = st_blob_size(&A);
    unsigned char *blobA = (unsigned char *)malloc(cap);
    long lenA = st_save(&A, blobA, cap);
    int nchunk = (int)((lenA + 4096 - 1) / 4096);
    printf("[info] S blob: %ld bytes, %d chunks (depth %d), st_blob_size=%zu\n",
           lenA, nchunk, nchunk > 127 ? 2 : 1, cap);

    /* ================================================================= */
    /* [ss3-blob-roundtrip-falsify] FIRST, on a model whose dropped chunk */
    /* is NOT yet anywhere in the store (content-addressing would dedup    */
    /* it away if the full blob were published first).                    */
    /* ================================================================= */
    printf("\n[ss3-blob-roundtrip-falsify] a dropped chunk -> fetch fails closed\n");
    {
        st_model C; train_s(&C, 0x0FA15E, 5, ROUNDS, LR);
        unsigned char *blobC = (unsigned char *)malloc(cap);
        long lenC = st_save(&C, blobC, cap);

        gl_student_test_drop_chunk(5);                 /* omit chunk 5 (w region) */
        int npub = gl_student_publish(9, blobC, (unsigned int)lenC);
        printf("  publish(node9, drop chunk 5) -> %d chunks\n", npub);

        unsigned char *outF = (unsigned char *)malloc(cap);
        memset(outF, 0xAB, cap);                       /* sentinel for unconsumed */
        int rf = gl_student_fetch(9, outF, (unsigned int)cap);
        printf("  fetch(node9) -> %d (want <0)\n", rf);

        int unconsumed = 1;
        for (size_t i = 0; i < cap; i++) if (outF[i] != 0xAB) { unconsumed = 0; break; }

        CHECK(npub > 0, "[ss3-blob-roundtrip-falsify] publish-with-drop still returns a chunk count");
        CHECK(rf < 0,   "[ss3-blob-roundtrip-falsify] missing chunk -> fetch returns <0 (fail closed)");
        CHECK(unconsumed, "[ss3-blob-roundtrip-falsify] out buffer left UNCONSUMED (no partial model)");

        free(blobC); free(outF); st_free(&C);
    }

    /* ================================================================= */
    /* [ss3-blob-roundtrip] publish A -> fetch -> byte-identical recovery  */
    /* ================================================================= */
    printf("\n[ss3-blob-roundtrip] publish + fetch the full S blob (depth 2)\n");
    unsigned char *outA = (unsigned char *)malloc(cap);
    memset(outA, 0x00, cap);
    int npubA = gl_student_publish(7, blobA, (unsigned int)lenA);
    int rA = gl_student_fetch(7, outA, (unsigned int)cap);
    printf("  publish(node7) -> %d chunks ; fetch -> %d bytes (want %ld)\n",
           npubA, rA, lenA);
    CHECK(npubA == nchunk, "[ss3-blob-roundtrip] publish returns the full chunk count");
    CHECK(rA == (int)lenA, "[ss3-blob-roundtrip] fetch returns the full blob length");
    CHECK(rA == (int)lenA && memcmp(blobA, outA, (size_t)lenA) == 0,
          "[ss3-blob-roundtrip] recovered blob is BYTE-IDENTICAL (all chunks + index)");

    /* ================================================================= */
    /* [ss3-blob-merge] the recovered blob feeds st_merge_cohort UNCHANGED */
    /* ================================================================= */
    printf("\n[ss3-blob-merge] recovered blob merges like the direct blob\n");
    {
        st_model B; train_s(&B, 0xC0FFEE, 7, ROUNDS, LR);   /* same seed, order 2 */
        float lossB = heldout(&B);
        unsigned char *blobB = (unsigned char *)malloc(cap);
        long lenB = st_save(&B, blobB, cap);
        float worse = (lossA > lossB) ? lossA : lossB;

        int tier_ok = st_blob_tier_ok(&B, outA, (size_t)lenA);
        CHECK(tier_ok == 1, "[ss3-blob-merge] recovered blob is a well-formed S model (st_blob_tier_ok)");

        /* merge the TRANSPORTED blob (outA) and the DIRECT blob (blobA) into
         * two fresh clones of B; the results must be byte-identical. */
        st_model Bt, Bd;
        train_s(&Bt, 0xC0FFEE, 7, ROUNDS, LR);
        train_s(&Bd, 0xC0FFEE, 7, ROUNDS, LR);
        const void *pT[1] = { outA };   size_t lT[1] = { (size_t)lenA };
        const void *pD[1] = { blobA };  size_t lD[1] = { (size_t)lenA };
        int accT = st_merge_cohort(&Bt, pT, lT, 1);
        int accD = st_merge_cohort(&Bd, pD, lD, 1);
        float lossMt = heldout(&Bt);

        printf("  parents: A=%.4f B=%.4f (worse=%.4f) ; merged(transported)=%.4f\n",
               (double)lossA, (double)lossB, (double)worse, (double)lossMt);
        CHECK(accT == 1 && accD == 1, "[ss3-blob-merge] same-tier peer ACCEPTED (transported & direct)");
        CHECK(memcmp(Bt.w, Bd.w, (size_t)Bt.n_params * sizeof(float)) == 0,
              "[ss3-blob-merge] merge(transported) bytes == merge(direct) bytes (transport fidelity)");
        CHECK(lossMt <= worse + 1e-4f,
              "[ss3-blob-merge] merged held-out loss <= the worse parent (convergence)");

        /* SS-3 peer-symmetry: merge(A,{B}) bytes == merge(B,{A}) bytes. */
        st_model As, Bs;
        train_s(&As, 0xC0FFEE, 1, ROUNDS, LR);   /* == A */
        train_s(&Bs, 0xC0FFEE, 7, ROUNDS, LR);   /* == B */
        const void *pB[1] = { blobB }; size_t lB[1] = { (size_t)lenB };
        const void *pA[1] = { blobA }; size_t lA[1] = { (size_t)lenA };
        int accAB = st_merge_cohort(&As, pB, lB, 1);
        int accBA = st_merge_cohort(&Bs, pA, lA, 1);
        CHECK(accAB == 1 && accBA == 1 &&
              memcmp(As.w, Bs.w, (size_t)As.n_params * sizeof(float)) == 0,
              "[ss3-blob-merge] peer-symmetric: merge(A,{B}) bytes == merge(B,{A}) bytes");

        free(blobB); st_free(&B); st_free(&Bt); st_free(&Bd); st_free(&As); st_free(&Bs);
    }

    /* ================================================================= */
    /* [ss3-blob-refuse-toobig] a blob exceeding the chunk cap is REFUSED  */
    /* (returns -1) — never truncated. The cap check fires BEFORE any byte */
    /* of the (here deliberately tiny) buffer is read.                    */
    /* ================================================================= */
    printf("\n[ss3-blob-refuse-toobig] over-cap blob is refused, never truncated\n");
    {
        unsigned char tiny = 0;
        /* (GL_ST_MAXCHUNK=8192)+1 chunks worth of claimed length. */
        unsigned int huge_len = (8192u + 1u) * 4096u;
        int rr = gl_student_publish(11, &tiny, huge_len);
        CHECK(rr == -1, "[ss3-blob-refuse-toobig] publish refuses an over-cap blob (-1)");
    }

    free(g_logits); free(blobA); free(outA); st_free(&A);

    printf("\nSUMMARY: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
