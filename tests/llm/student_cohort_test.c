/*
 *  student_cohort_test.c — host cert for SS-3 "same-tier merge cohorts"
 *  (arch/common/llm/student.c, special-structure-mind.md §3.2 / §8 item 4).
 *
 *  SS-3 gives the byte student its OWN, isolated, student-only weight merge
 *  (st_merge_cohort) so SAME-TIER students can average their weights — WITHOUT
 *  ever calling R3's gl_merge / touching rw[] (the [baby-merge-isolation]
 *  tripwire). The merge cohort is the TIER: S averages with S, M with M; a
 *  cross-tier blob is REFUSED by construction (an honest island).
 *
 *  Certs (all self-contained, IN-PROCESS — the core claim needs no network):
 *    [ss3-cohort-converge]  TWO M-tier students seeded identically, trained on
 *                           a SHARED objective with different data orderings,
 *                           then merged same-tier:
 *                             (a) merged held-out loss <= the WORSE parent
 *                                 (convergence for a compatible objective);
 *                             (b) merge is PEER-SYMMETRIC: merge(A,{B}) bytes
 *                                 == merge(B,{A}) bytes (canonical reduction).
 *    [ss3-cohort-island]    an S-tier blob offered to an M-tier merge is
 *                           REFUSED (accepted==0) and the M model's weights are
 *                           left BYTE-UNCHANGED (islands by construction).
 *    [ss3-merge-falsifiable]a SABOTAGED merge (a 1e-3 perturbation applied to
 *                           one peer before merging, like the KV cert) BREAKS
 *                           peer-symmetry — proving the cert can FAIL.
 *
 *  HONESTY (Path-W, memory moment_2026_06_12_wave41_one_mind): naive weight-
 *  averaging is the MECHANISM and CONVERGES two minds toward a SHARED objective.
 *  It is LOSSY for two minds that learned DIFFERENT facts (one survives, one
 *  decays toward chance). This cert proves the MECHANISM (deterministic,
 *  symmetric, same-tier average) + convergence for a compatible objective. It
 *  does NOT claim two divergent facts are both preserved — that is FALSE for
 *  averaging; union-replay / Fisher recovery is Path-W² and OUT OF SCOPE here.
 *
 *  Build (wave-49): -O1 -ffp-contract=off (one math everywhere).
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

/* a fixed, structured byte corpus — the SHARED objective both students learn.
 * Same family for both: averaging two models of the SAME objective converges. */
static const uint8_t CORPUS[] =
    "the cat sat on the mat. the dog ran in the sun. she said the sea is blue "
    "and the sky is blue too. a bird sang on the old oak tree at dawn. the cat "
    "and the dog sat by the sea and saw the sun set red. the old man ran to the "
    "sea to see the red sun set on the blue sea at the end of the day.";

#define SEQLEN   24

/* mean held-out next-byte loss over `count` windows starting at byte `from`. */
static float heldout_loss(st_model *m, int from, int count)
{
    float tot = 0.0f; int got = 0;
    for (int w = 0; w < count; w++) {
        int off = from + w * SEQLEN;
        if (off + SEQLEN + 1 > (int)sizeof(CORPUS) - 1) break;
        uint8_t buf[SEQLEN];
        for (int i = 0; i < SEQLEN; i++) buf[i] = CORPUS[off + i];
        int np = 0;
        float l = st_eval_loss(m, buf, SEQLEN, &np);
        tot += l; got++;
    }
    return got ? tot / (float)got : 0.0f;
}

/* byte offset of the w[] payload inside an st_save() blob == the fixed header
 * size: blob = HDR + 3*np float (w,mu,vu) + int32 adam_t. Derived (not a magic
 * 40) so the perturbation in [ss3-merge-falsifiable] hits w[0] exactly. */
static size_t blob_w_off(const st_model *m)
{
    return st_blob_size(m)
         - ((size_t)m->n_params * 3u * sizeof(float))
         - sizeof(int32_t);
}

int main(void)
{
    printf("=== SS-3 same-tier merge cohort cert ===\n");
    printf("(special-structure-mind.md §3.2 / §8 item 4)\n\n");

    int total_windows = ((int)sizeof(CORPUS) - 1) / SEQLEN;
    int train_windows = total_windows * 3 / 4;
    if (train_windows < 2) train_windows = 2;
    int held_from = train_windows * SEQLEN;
    int held_windows = total_windows - train_windows;
    if (held_windows < 1) held_windows = 1;

    /* shared logit scratch (the forward writes n*VOCAB logits). */
    float *logits = (float *)malloc((size_t)SEQLEN * ST_VOCAB * sizeof(float));
    if (!logits) { printf("OOM\n"); return 2; }

    /* ----------------------------------------------------------------- */
    /* [ss3-cohort-converge] — two M-tier minds, shared objective, merge */
    /* ----------------------------------------------------------------- */
    printf("[ss3-cohort-converge] two M-tier students, shared objective\n");

    st_model A, B;
    /* SAME seed -> SAME starting point (linear mode connectivity, the merge
     * precondition — two clones of one origin average well; see g22). */
    st_init_tier(&A, 0xC0FFEE, ST_TIER_M);
    st_init_tier(&B, 0xC0FFEE, ST_TIER_M);

    /* train both on the SAME corpus but in a DIFFERENT visit order, so they
     * diverge along different SGD paths toward a compatible minimum. */
    const int ROUNDS = 60; const float LR = 0.02f;
    for (int r = 0; r < ROUNDS; r++) {
        for (int j = 0; j < train_windows; j++) {
            /* A visits 0,1,2,..; B visits a rotated order (shift = j*7 % W). */
            int wa = j;
            int wb = (j * 7 + 3) % train_windows;
            uint8_t ba[SEQLEN], bb[SEQLEN];
            for (int i = 0; i < SEQLEN; i++) { ba[i] = CORPUS[wa*SEQLEN+i]; bb[i] = CORPUS[wb*SEQLEN+i]; }
            st_zero_grad(&A); st_forward(&A, ba, SEQLEN, logits); st_backward(&A, ba, SEQLEN); st_adam_step(&A, LR);
            st_zero_grad(&B); st_forward(&B, bb, SEQLEN, logits); st_backward(&B, bb, SEQLEN); st_adam_step(&B, LR);
        }
    }
    float lossA = heldout_loss(&A, held_from, held_windows);
    float lossB = heldout_loss(&B, held_from, held_windows);
    float worse = (lossA > lossB) ? lossA : lossB;

    /* serialize B -> a peer blob, then merge it into A (canonical: A first). */
    size_t cap = st_blob_size(&A);
    unsigned char *blobB = (unsigned char *)malloc(cap);
    long lenB = st_save(&B, blobB, cap);

    /* snapshot A's pre-merge weights so we can also compute merge(B,{A}). */
    int np = A.n_params;
    float *Aw = (float *)malloc((size_t)np * sizeof(float));
    memcpy(Aw, A.w, (size_t)np * sizeof(float));
    unsigned char *blobA = (unsigned char *)malloc(cap);
    long lenA = st_save(&A, blobA, cap);   /* A's pre-merge blob, for merge(B,{A}) */

    /* merge(A,{B}) -> Amerged */
    const void *peersB[1] = { blobB };
    size_t lensB[1] = { (size_t)lenB };
    int accA = st_merge_cohort(&A, peersB, lensB, 1);
    float lossM = heldout_loss(&A, held_from, held_windows);

    printf("  parent losses: A=%.4f  B=%.4f  (worse=%.4f)\n",
           (double)lossA, (double)lossB, (double)worse);
    printf("  merged loss  : %.4f   accepted_peers=%d\n", (double)lossM, accA);

    CHECK(accA == 1, "[ss3-cohort-converge] same-tier peer ACCEPTED (1)");
    CHECK(lossM <= worse + 1e-4f,
          "[ss3-cohort-converge] merged held-out loss <= the worse parent");

    /* (b) peer-symmetry: merge(B,{A}) must be BYTE-IDENTICAL to merge(A,{B}).
     * Rebuild a fresh B' == B (same seed+training), load A's pre-merge blob as
     * the peer, and merge into B'. */
    st_model Bm;
    st_init_tier(&Bm, 0xC0FFEE, ST_TIER_M);
    /* re-train Bm identically to B */
    for (int r = 0; r < ROUNDS; r++) {
        for (int j = 0; j < train_windows; j++) {
            int wb = (j * 7 + 3) % train_windows;
            uint8_t bb[SEQLEN];
            for (int i = 0; i < SEQLEN; i++) bb[i] = CORPUS[wb*SEQLEN+i];
            st_zero_grad(&Bm); st_forward(&Bm, bb, SEQLEN, logits); st_backward(&Bm, bb, SEQLEN); st_adam_step(&Bm, LR);
        }
    }
    const void *peersA[1] = { blobA };
    size_t lensA[1] = { (size_t)lenA };
    int accB = st_merge_cohort(&Bm, peersA, lensA, 1);

    int sym = (accB == 1) &&
              (memcmp(A.w, Bm.w, (size_t)np * sizeof(float)) == 0);
    CHECK(sym, "[ss3-cohort-converge] peer-symmetric: merge(A,{B}) bytes == merge(B,{A}) bytes");

    /* ----------------------------------------------------------------- */
    /* [ss3-merge-falsifiable] — a sabotaged peer breaks symmetry        */
    /* ----------------------------------------------------------------- */
    printf("\n[ss3-merge-falsifiable] a perturbed peer breaks the symmetry\n");
    {
        /* perturb ONE weight of A's peer blob by 1e-3 (the KV-cert idea), then
         * merge(B,{A'}) and compare to merge(A,{B}). They MUST now differ — if
         * they were still equal, the merge would be ignoring the peer (broken).*/
        unsigned char *blobA2 = (unsigned char *)malloc(cap);
        memcpy(blobA2, blobA, cap);
        /* bump w[0] in the peer blob by 1e-3 (the KV-cert perturbation). */
        size_t woff = blob_w_off(&A);
        float w0; memcpy(&w0, blobA2 + woff, sizeof w0);
        w0 += 1e-3f;
        memcpy(blobA2 + woff, &w0, sizeof w0);

        st_model Bf;
        st_init_tier(&Bf, 0xC0FFEE, ST_TIER_M);
        for (int r = 0; r < ROUNDS; r++)
            for (int j = 0; j < train_windows; j++) {
                int wb = (j * 7 + 3) % train_windows;
                uint8_t bb[SEQLEN];
                for (int i = 0; i < SEQLEN; i++) bb[i] = CORPUS[wb*SEQLEN+i];
                st_zero_grad(&Bf); st_forward(&Bf, bb, SEQLEN, logits); st_backward(&Bf, bb, SEQLEN); st_adam_step(&Bf, LR);
            }
        const void *peersA2[1] = { blobA2 };
        size_t lensA2[1] = { (size_t)lenA };
        st_merge_cohort(&Bf, peersA2, lensA2, 1);
        int differs = (memcmp(A.w, Bf.w, (size_t)np * sizeof(float)) != 0);
        CHECK(differs, "[ss3-merge-falsifiable] perturbed peer changes the merge (cert can FAIL)");
        st_free(&Bf); free(blobA2);
    }

    /* ----------------------------------------------------------------- */
    /* [ss3-cohort-island] — an S-tier blob into an M merge is REFUSED   */
    /* ----------------------------------------------------------------- */
    printf("\n[ss3-cohort-island] cross-tier (S into M) REFUSED, M unchanged\n");
    {
        st_model S, M2;
        st_init_tier(&S,  0x5EED, ST_TIER_S);
        st_init_tier(&M2, 0xBEEF, ST_TIER_M);

        /* save the S model -> a cross-tier peer blob. */
        size_t scap = st_blob_size(&S);
        unsigned char *blobS = (unsigned char *)malloc(scap);
        long lenS = st_save(&S, blobS, scap);

        /* snapshot M2's weights; a refused merge must leave them byte-for-byte. */
        int np2 = M2.n_params;
        float *pre = (float *)malloc((size_t)np2 * sizeof(float));
        memcpy(pre, M2.w, (size_t)np2 * sizeof(float));

        const void *peerS[1] = { blobS };
        size_t lensS[1] = { (size_t)lenS };
        int acc = st_merge_cohort(&M2, peerS, lensS, 1);
        int untouched = (memcmp(pre, M2.w, (size_t)np2 * sizeof(float)) == 0);

        printf("  st_merge_cohort(M, {S-blob}) accepted=%d (want 0)\n", acc);
        CHECK(acc == 0, "[ss3-cohort-island] cross-tier S peer REFUSED (accepted==0)");
        CHECK(untouched, "[ss3-cohort-island] refused merge left M weights BYTE-UNCHANGED");

        free(pre); free(blobS); st_free(&S); st_free(&M2);
    }

    /* ----------------------------------------------------------------- */
    /* Path-W honesty note (printed, not just doc'd).                    */
    /* ----------------------------------------------------------------- */
    printf("\n[honesty] averaging is the MECHANISM: it converges minds toward a\n");
    printf("          SHARED objective (proven above). It is LOSSY for DIVERGENT\n");
    printf("          facts (one survives, one decays) — union-replay / Fisher\n");
    printf("          recovery is Path-W^2, OUT OF SCOPE here (memory wave-41).\n");

    free(logits); free(blobB); free(blobA); free(Aw);
    st_free(&A); st_free(&B); st_free(&Bm);

    printf("\nSUMMARY: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
