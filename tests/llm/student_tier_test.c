/*
 *  student_tier_test.c — host cert for SS-2 "tier-selectable model dims"
 *  (arch/common/llm/student.c, special-structure-mind.md §3.2).
 *
 *  SS-2 makes the byte student's four model dims (D=d_model, DFF, L=layers,
 *  E=experts) TIER-SELECTABLE at runtime (S/M/L) instead of compile-time fixed,
 *  WITHOUT changing today's behaviour: the DEFAULT tier is M and M is BYTE-
 *  IDENTICAL to the pre-SS-2 baby.  Every stack scratch array is bound to the
 *  fixed ST_*_MAX (the L tier), never the runtime dim — the [no-vla] gate.
 *
 *  This ONE source file is compiled into TWO binaries by run_ss2.sh:
 *    - the NEW student.c  (tier-aware: ST_TIER_M is defined)
 *    - the BASE student.c (pre-SS-2 snapshot: ST_TIER_M is NOT defined)
 *  Both print a deterministic FNV-1a hash of the M-tier forward logits + a
 *  fixed-input held-out loss.  run_ss2.sh diffs the two hashes/losses — equal
 *  proves [m-identical] + [no-loss-regression] against the real predecessor.
 *
 *  Certs:
 *    [m-identical]        M-tier forward-logit FNV hash + held-out loss are
 *                         BYTE-IDENTICAL between the new and base student.c.
 *    [no-loss-regression] M-tier held-out loss == the base loss (same number).
 *    [tier-load]          (new build only) save M -> load M ok; save S ->
 *                         load S ok; save M into an S-tier model REFUSED
 *                         (fail-closed, the S model keeps its own weights).
 *    [tier-distinct]      (new build only) S/M/L have DISTINCT n_params and
 *                         dims; S < M < L; M == the legacy n_params.
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

/* a fixed, structured byte fixture (independent of any teacher file) so the
 * hash is reproducible across builds. */
static const uint8_t FIX[] =
    "the cat sat on the mat. the dog ran in the sun. she said the sea is "
    "blue and the sky is blue too.";

/* FNV-1a over a float buffer's raw bytes (deterministic; -ffp-contract=off so
 * the floats themselves are bit-stable across targets). */
static uint64_t fnv1a(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

/* Forward the M-tier (default) model on FIX and hash the full logit matrix +
 * compute the held-out next-byte loss. Identical code path on both builds. */
static int m_identical_probe(uint64_t *hash_out, float *loss_out)
{
    st_model m;
    if (st_init(&m, 0xC0FFEEu) != ST_OK) return -1;   /* default == M tier */
    int n = (int)sizeof(FIX) - 1;
    if (n > ST_MAXSEQ) n = ST_MAXSEQ;
    float *logits = (float *)malloc((size_t)n * ST_VOCAB * sizeof(float));
    if (!logits) { st_free(&m); return -1; }

    int rc = st_forward(&m, FIX, n, logits);
    if (rc != ST_OK) { free(logits); st_free(&m); return -1; }
    *hash_out = fnv1a(logits, (size_t)n * ST_VOCAB * sizeof(float));

    int np = 0;
    *loss_out = st_eval_loss(&m, FIX, n, &np);

    free(logits);
    st_free(&m);
    return 0;
}

int main(int argc, char **argv)
{
    int want_machine = (argc > 1 && strcmp(argv[1], "--machine") == 0);

    uint64_t h = 0; float loss = 0.0f;
    if (m_identical_probe(&h, &loss) != 0) {
        printf("PROBE_FAIL\n");
        return 2;
    }

    /* machine-readable lines run_ss2.sh scrapes from BOTH builds for the diff. */
    printf("M_HASH 0x%016llx\n", (unsigned long long)h);
    printf("M_LOSS %.6f\n", (double)loss);
    if (want_machine) {
        /* nothing more on the machine path — the script does the compare. */
        return 0;
    }

    printf("=== SS-2 tier cert ===\n");
    printf("  M-tier forward-logit FNV hash = 0x%016llx\n", (unsigned long long)h);
    printf("  M-tier held-out loss          = %.6f nats\n", (double)loss);

#ifdef ST_TIER_M
    /* ---- tier infrastructure exists only in the NEW build ---- */
    printf("\n[tier-distinct] S/M/L are distinct shapes; M == legacy\n");
    {
        st_model s, mm, l;
        int rs = st_init_tier(&s,  1, ST_TIER_S);
        int rm = st_init_tier(&mm, 1, ST_TIER_M);
        int rl = st_init_tier(&l,  1, ST_TIER_L);
        int ok = (rs == ST_OK && rm == ST_OK && rl == ST_OK);
        if (ok) {
            printf("  S: d=%d dff=%d L=%d E=%d  n_params=%d\n",
                   s.d, s.dff, s.nlayer, s.nexpert, s.n_params);
            printf("  M: d=%d dff=%d L=%d E=%d  n_params=%d\n",
                   mm.d, mm.dff, mm.nlayer, mm.nexpert, mm.n_params);
            printf("  L: d=%d dff=%d L=%d E=%d  n_params=%d\n",
                   l.d, l.dff, l.nlayer, l.nexpert, l.n_params);
            /* M must equal the legacy compile-time dims exactly. */
            int m_is_legacy = (mm.d == ST_DMODEL && mm.dff == ST_DFF &&
                               mm.nlayer == ST_NLAYER && mm.nexpert == ST_NEXPERT);
            CHECK(m_is_legacy, "[tier-distinct] M-tier dims == the legacy NS-1 dims");
            CHECK(s.n_params < mm.n_params && mm.n_params < l.n_params,
                  "[tier-distinct] n_params strictly grows S < M < L");
            /* every tier's dims must be <= the MAX scratch ceiling (no-vla). */
            CHECK(l.d <= ST_D_MAX && l.dff <= ST_DFF_MAX &&
                  l.nlayer <= ST_L_MAX && l.nexpert <= ST_E_MAX,
                  "[tier-distinct] L-tier dims fit under ST_*_MAX (no-vla bound)");
        } else {
            CHECK(0, "[tier-distinct] st_init_tier S/M/L all init");
        }
        st_free(&s); st_free(&mm); st_free(&l);
    }

    /* ---- the tiers must actually RUN (forward succeeds on each shape) ---- */
    printf("\n[tier-run] each tier forwards without overflow\n");
    {
        const int tiers[3] = { ST_TIER_S, ST_TIER_M, ST_TIER_L };
        const char *nm[3]  = { "S", "M", "L" };
        int all = 1;
        for (int i = 0; i < 3; i++) {
            st_model t;
            if (st_init_tier(&t, 7, tiers[i]) != ST_OK) { all = 0; continue; }
            int n = (int)sizeof(FIX) - 1; if (n > ST_MAXSEQ) n = ST_MAXSEQ;
            float *lg = (float *)malloc((size_t)n * ST_VOCAB * sizeof(float));
            int rc = lg ? st_forward(&t, FIX, n, lg) : ST_E_OOM;
            int w = st_last_fire_width();
            printf("  tier %s: forward rc=%d  final-token width=%d (<= E=%d)\n",
                   nm[i], rc, w, t.nexpert);
            if (rc != ST_OK || w < ST_TOPK || w > t.nexpert) all = 0;
            free(lg); st_free(&t);
        }
        CHECK(all, "[tier-run] S/M/L all forward; width within [K_min, E]");
    }

    /* ---- [tier-load] save/load round-trip + cross-tier refusal ---- */
    printf("\n[tier-load] round-trip per tier + cross-tier load REFUSED\n");
    {
        /* save M -> load M : OK */
        st_model ma, mb;
        st_init_tier(&ma, 11, ST_TIER_M);
        st_init_tier(&mb, 99, ST_TIER_M);     /* different seed -> different w */
        size_t cap = st_blob_size(&ma);
        unsigned char *blobM = (unsigned char *)malloc(cap);
        long wlen = st_save(&ma, blobM, cap);
        int rmm = st_load(&mb, blobM, (size_t)wlen);
        /* after load, mb.w must equal ma.w byte-for-byte */
        int same = (rmm == ST_OK) &&
                   (memcmp(ma.w, mb.w, (size_t)ma.n_params * sizeof(float)) == 0);
        CHECK(same, "[tier-load] save M -> load M ok (weights restored)");

        /* save S -> load S : OK */
        st_model sa, sb;
        st_init_tier(&sa, 21, ST_TIER_S);
        st_init_tier(&sb, 88, ST_TIER_S);
        size_t scap = st_blob_size(&sa);
        unsigned char *blobS = (unsigned char *)malloc(scap);
        long slen = st_save(&sa, blobS, scap);
        int rss = st_load(&sb, blobS, (size_t)slen);
        int ssame = (rss == ST_OK) &&
                    (memcmp(sa.w, sb.w, (size_t)sa.n_params * sizeof(float)) == 0);
        CHECK(ssame, "[tier-load] save S -> load S ok (weights restored)");

        /* save M -> load into an S-tier model : REFUSED (fail-closed) */
        st_model st;
        st_init_tier(&st, 33, ST_TIER_S);
        /* snapshot the S model's weights; a refused load must leave them intact */
        float *pre = (float *)malloc((size_t)st.n_params * sizeof(float));
        memcpy(pre, st.w, (size_t)st.n_params * sizeof(float));
        int rmis = st_load(&st, blobM, (size_t)wlen);   /* M blob into S model */
        int untouched = (memcmp(pre, st.w, (size_t)st.n_params * sizeof(float)) == 0);
        printf("  cross-tier st_load(M-blob -> S-model) returned %d (want <0)\n", rmis);
        CHECK(rmis < 0, "[tier-load] save M -> load S REFUSED (mismatch fail-closed)");
        CHECK(untouched, "[tier-load] refused load left the S model's weights intact");

        free(pre); free(blobM); free(blobS);
        st_free(&ma); st_free(&mb); st_free(&sa); st_free(&sb); st_free(&st);
    }
#else
    printf("\n(base build: tier infrastructure absent — only the M hash/loss "
           "above are compared by run_ss2.sh)\n");
#endif

    printf("\nSUMMARY: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
