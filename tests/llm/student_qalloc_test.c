/*
 * student_qalloc_test.c — [st-qalloc] (R-1 stage 5; design:
 * docs/architecture/30-module/research/brain-quant-alloc.md §3, gates fixed
 * 2026-10-24 before any E3O3 / A3 / AUTO / gate run).
 *
 * The student is trained as student_qmix_test trains it (8 rounds, lr 3e-3,
 * seqlen 32, the fixture's train windows). Calibration (GPTQ H and the
 * Fisher) on the EVEN train windows, the copy-drift gate on the ODD ones, B1
 * held only reported. Arms, all 2-bit seq plus exactly 4096 B:
 *   E4    st_quant_gptq_bits(2, embed 4, attn 2, out 2)   (stage 4's E4)
 *   E3O3  st_quant_gptq_bits(2, embed 3, attn 2, out 3)
 *   A3    st_quant_gptq_bits(2, embed 2, attn 3, out 2)
 *   AUTO  the arm of the smallest st_quant_omega (Fisher of the even windows)
 *
 *   I0  bits(2,4,2,2) == mix(2,4,2) and bits(2,2,2,2) == gptq(2), byte-identical
 *   P0  bytes_q of E4, E3O3, A3 == bytes_q(seq) + 4096, exactly
 *   A0  AUTO built from the pick is byte-identical to the picked arm, and every
 *       omega is > 0
 *   P1  B1 bpb damage of AUTO <= min over E4, E3O3, A3 + 0.02
 *   G4  every quantized value of every arm on its row's grid at its bits, and
 *       the calibration shares no 16-byte run with the held text
 *   K0  st_quant_copy_kl (own exp/log, no libm) agrees with this test's libm
 *       KL on the gate windows within 1e-3 relative, for seq and E4
 *   D1  gate (odd windows, KL <= ST_QGATE_KL): E4 accepted, seq refused, a
 *       copy of the model itself KL 0 and accepted
 * Values (the runner counts them over the seeds): "value P2 1|0" (the arm of
 * the largest omega has a larger damage than AUTO; the gate is 6 of 8 seeds),
 * "value D2 k/4" (held KL <= 0.05 agrees with the gate, for seq E4 E3O3 A3).
 *
 * Negative controls (run_st_qalloc.sh): -DST_QALLOC_INVERT P1 red;
 * -DST_QUANT_BADSCALE D1 red (E4 refused); -DST_QALLOC_FLATFISHER values only.
 *
 * Stage 6 (research/brain-quant-alloc-el.md, run_st_qalloc_el.sh, seeds 9-16):
 * -DST_QALLOC_EL picks by st_quant_omega_el (sum_i F_i (RTN(W)_i - W_i)^2)
 * instead; every item, P2 and the controls then refer to omega_el, and stage
 * 5's omega is printed as a value ("value omega_mat").
 *
 * Stage 7 (research/brain-quant-alloc-eq.md, seeds 17-24): "value E" lines,
 * paired bootstrap 95% intervals of held damage differences over the held
 * windows (values only, never a pick); -DST_QALLOC_BOOT_ONE gives width 0.
 *
 * Stage 8 (research/brain-quant-alloc-cal.md, run_st_qalloc_cal.sh, seeds
 * 25-32): -DST_QALLOC_CAL picks the arm whose built copy loses least bpb on
 * <b1_cal.txt> (the third argument; held is never used), prints "value C1"
 * (clear misses of the CAL and the omega_el pick) and adds b1_cal vs held to
 * G4. -DST_QALLOC_CAL_INVERT picks the largest; -DST_QALLOC_CAL_SHORT keeps
 * b1_cal's first 48 windows.
 *
 * Stage 9 (research/brain-quant-alloc-src.md, run_st_qalloc_src.sh, seeds
 * 33-48): -DST_QALLOC_SRC (with CAL) prints "value SRC S33|TAIL|GATE", the
 * damage on other texts and the clear miss of the arm each would pick (the
 * pick stays CAL), and adds TAIL vs held and b1_cal vs the fixture to G4.
 * -DST_QALLOC_SRC_SWAP feeds S33's windows to the TAIL line.
 *
 *   student_qalloc_test <b1_held.txt> <seed> [b1_cal.txt]   exit 0 = ALL PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../../arch/common/llm/student.h"
#include "../../arch/common/llm/held_indep.h"
#include "../../arch/common/llm/teacher_fixture.h"

#if defined(ST_QALLOC_SRC) && !defined(ST_QALLOC_CAL)
#error "ST_QALLOC_SRC needs ST_QALLOC_CAL"
#endif

#define SEQ 32
#define LN2 0.69314718055994530942
#define NARM 3

#if defined(ST_QALLOC_EL)
#define OMEGA st_quant_omega_el
#define OMEGA_NAME "omega_el"
#else
#define OMEGA st_quant_omega
#define OMEGA_NAME "omega"
#endif

static const char *arm_name[NARM] = { "E4", "E3O3", "A3" };
static const int arm_bits[NARM][3] = { { 4, 2, 2 }, { 3, 2, 3 }, { 2, 3, 2 } };  /* embed attn out */

static uint8_t held[65536];
static int hn;
static int fails;
static float *lg_a, *lg_b;

static void item(int ok, const char *id, const char *what)
{
    printf("[st-qalloc] %s %s %s\n", ok ? "PASS" : "FAIL", id, what);
    if (!ok) fails++;
}

/* bpb over the windows of buf; lw (if not NULL) gets each counted window's
 * loss in nats (stage 7). nwin is the count of the last call. */
static int nwin;
static double bpb_buf(st_model *m, const uint8_t *buf, int n, double *lw)
{
    double s = 0.0; int got = 0;
    for (int w = 0; (w + 1) * SEQ <= n; w++) {
        int np = 0;
        float l = st_eval_loss(m, buf + w * SEQ, SEQ, &np);
        if (np) { s += l; if (lw) lw[got] = l; got++; }
    }
    nwin = got;
    return got ? s / got / LN2 : 0.0;
}

static double bpb_w(st_model *m, double *lw) { return bpb_buf(m, held, hn, lw); }

static double bpb(st_model *m) { return bpb_w(m, NULL); }

/*
 * Stage 7 (research/brain-quant-alloc-eq.md §2): paired bootstrap of the
 * held damage difference of two arms. The pairs are the held windows
 * (d_w = (loss_a - loss_b) / ln 2, the fp32 loss cancels); BOOT_B resamples
 * of nwin windows with replacement (xorshift64*, a fixed seed), and the
 * percentile 95% interval (the 50th and the 1950th of the 2000 sorted means).
 * Values only: never used to pick. Neighbouring windows are one text's
 * continuation, not independent, so the interval is narrower than it should
 * be. -DST_QALLOC_BOOT_ONE draws window 0 every time (width 0).
 */
#define BOOT_B 2000
#define MAXWIN (65536 / SEQ)
static uint64_t xs;
static uint64_t xorshift64s(void)
{
    xs ^= xs >> 12; xs ^= xs << 25; xs ^= xs >> 27;
    return xs * 0x2545F4914F6CDD1DULL;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void boot_ci(const double *la, const double *lb, int n, uint32_t seed,
                    double *mean, double *lo, double *hi)
{
    static double d[MAXWIN], bm[BOOT_B];
    double s = 0.0;
    for (int i = 0; i < n; i++) { d[i] = (la[i] - lb[i]) / LN2; s += d[i]; }
    *mean = n ? s / n : 0.0;
    xs = 0x9E3779B97F4A7C15ULL ^ (uint64_t)seed;
    for (int b = 0; b < BOOT_B; b++) {
        double t = 0.0;
        for (int i = 0; i < n; i++) {
            int k = (int)(xorshift64s() % (uint64_t)n);
#if defined(ST_QALLOC_BOOT_ONE)
            k = 0;
#endif
            t += d[k];
        }
        bm[b] = n ? t / n : 0.0;
    }
    qsort(bm, BOOT_B, sizeof bm[0], cmp_d);
    *lo = bm[BOOT_B / 40 - 1];
    *hi = bm[BOOT_B - BOOT_B / 40 - 1];
}

static void softmax(const float *l, double *p)
{
    double mx = l[0], s = 0.0;
    for (int o = 1; o < ST_VOCAB; o++) if (l[o] > mx) mx = l[o];
    for (int o = 0; o < ST_VOCAB; o++) { p[o] = exp((double)l[o] - mx); s += p[o]; }
    for (int o = 0; o < ST_VOCAB; o++) p[o] /= s;
}

/* mean KL(a || b) per position over the windows of buf (libm, double) */
static double kl_mean(st_model *a, st_model *b, const uint8_t *buf, int n)
{
    static double pa[ST_VOCAB], pb[ST_VOCAB];
    double s = 0.0; long np = 0;
    for (int w = 0; (w + 1) * SEQ <= n; w++) {
        st_forward(a, buf + w * SEQ, SEQ, lg_a);
        st_forward(b, buf + w * SEQ, SEQ, lg_b);
        for (int t = 0; t < SEQ; t++) {
            softmax(lg_a + (size_t)t * ST_VOCAB, pa);
            softmax(lg_b + (size_t)t * ST_VOCAB, pb);
            double k = 0.0;
            for (int o = 0; o < ST_VOCAB; o++)
                if (pa[o] > 0.0) k += pa[o] * (log(pa[o]) - log(pb[o] > 1e-300 ? pb[o] : 1e-300));
            s += k; np++;
        }
    }
    return np ? s / (double)np : 0.0;
}

/* G4: the rows of q are on src's per-row grid at bits */
static long off_grid_mat(const st_model *src, const st_model *q, int off, int rows, int cols, int bits)
{
    int qmax = (1 << (bits - 1)) - 1;
    long bad = 0;
    for (int r = 0; r < rows; r++) {
        const float *a = src->w + off + (size_t)r * cols, *b = q->w + off + (size_t)r * cols;
        float amax = 0.0f;
        for (int c = 0; c < cols; c++) { float v = a[c] < 0 ? -a[c] : a[c]; if (v > amax) amax = v; }
        if (amax == 0.0f) { for (int c = 0; c < cols; c++) if (b[c] != 0.0f) bad++; continue; }
        float s = amax / (float)qmax;
        for (int c = 0; c < cols; c++) {
            double k = (double)b[c] / (double)s;
            double rk = floor(k + 0.5);
            if (fabs(k - rk) > 1e-3 || rk > qmax || rk < -qmax - 1) bad++;
        }
    }
    return bad;
}

static long off_grid(const st_model *src, const st_model *q, int bits, int be, int ba, int bo)
{
    const int D = src->d, DFF = src->dff, L = src->nlayer, E = src->nexpert;
    long bad = off_grid_mat(src, q, src->o_embed, ST_VOCAB, D, be);
    for (int l = 0; l < L; l++) {
        bad += off_grid_mat(src, q, src->o_wq + l * D * D, D, D, ba);
        bad += off_grid_mat(src, q, src->o_wk + l * D * D, D, D, ba);
        bad += off_grid_mat(src, q, src->o_wv + l * D * D, D, D, ba);
        bad += off_grid_mat(src, q, src->o_wo + l * D * D, D, D, ba);
    }
    for (int x = 0; x < L * E; x++) {
        bad += off_grid_mat(src, q, src->o_w1 + x * DFF * D, DFF, D, bits);
        bad += off_grid_mat(src, q, src->o_w3 + x * DFF * D, DFF, D, bits);
        bad += off_grid_mat(src, q, src->o_w2 + x * D * DFF, D, DFF, bits);
    }
    bad += off_grid_mat(src, q, src->o_out, ST_VOCAB, D, bo);
    return bad;
}

static int same_w(const st_model *a, const st_model *b)
{
    return a->n_params == b->n_params &&
           memcmp(a->w, b->w, (size_t)a->n_params * sizeof(float)) == 0;
}

static void red_all(void)
{
    item(0, "I0", "bits(2,4,2,2) == mix(2,4,2), bits(2,2,2,2) == gptq(2)");
    item(0, "P0", "bytes E4 == E3O3 == A3 == seq + 4096");
    item(0, "A0", "AUTO is the picked arm; every omega > 0");
    item(0, "P1", "bpb damage AUTO <= min(E4, E3O3, A3) + 0.02");
    item(0, "G4", "every arm on its grid; calib shares no 16-byte run with held");
    item(0, "K0", "st_quant_copy_kl == libm KL within 1e-3 relative");
    item(0, "D1", "gate: E4 accepted, seq refused, self KL 0 accepted");
    printf("[st-qalloc] RED\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <b1_held.txt> <seed>\n", argv[0]); return 2; }
    uint32_t seed = (uint32_t)strtoul(argv[2], NULL, 0);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    hn = (int)fread(held, 1, sizeof held, f);
    fclose(f);
#if defined(ST_QALLOC_CAL)
    /* stage 8: the node's own calibration text (never held) */
    static uint8_t calt[65536];
    int cln = 0;
    if (argc < 4 || !(f = fopen(argv[3], "rb"))) { fprintf(stderr, "ST_QALLOC_CAL needs <b1_cal.txt>\n"); return 2; }
    cln = (int)fread(calt, 1, sizeof calt, f);
    fclose(f);
#if defined(ST_QALLOC_CAL_SHORT)
    if (cln > 48 * SEQ) cln = 48 * SEQ;   /* the fixture's amount of text */
#endif
#endif

    const uint8_t *fx = (const uint8_t *)TEACHER_FIXTURE;
    int fxn = (int)sizeof(TEACHER_FIXTURE) - 1;
    int fx_end = st_held_split(fxn, SEQ).train_end;
    static uint8_t cal[65536], gate[65536];
    int cn = 0, gn = 0;
    for (int w = 0; (w + 1) * SEQ <= fx_end; w++) {
        if (w % 2 == 0) { memcpy(cal + cn, fx + w * SEQ, SEQ); cn += SEQ; }
        else            { memcpy(gate + gn, fx + w * SEQ, SEQ); gn += SEQ; }
    }

    lg_a = (float *)malloc((size_t)SEQ * ST_VOCAB * sizeof(float));
    lg_b = (float *)malloc((size_t)SEQ * ST_VOCAB * sizeof(float));
    if (!lg_a || !lg_b) return 2;

    st_model m, mc, qs, qa[NARM], qx;
    if (st_init_tier(&m, seed, ST_TIER_S) || st_init_tier(&mc, seed, ST_TIER_S) ||
        st_init_tier(&qs, seed, ST_TIER_S) || st_init_tier(&qx, seed, ST_TIER_S) ||
        st_init_tier(&qa[0], seed, ST_TIER_S) || st_init_tier(&qa[1], seed, ST_TIER_S) ||
        st_init_tier(&qa[2], seed, ST_TIER_S)) { printf("[st-qalloc] FAIL init\n"); return 1; }
    for (int r = 0; r < 8; r++)
        for (int o = 0; o + SEQ <= fx_end; o += SEQ) ST_TRAIN_STEP(&m, fx + o, SEQ, lg_a, 3e-3f);
    for (int i = 0; i < m.n_params; i++) mc.w[i] = m.w[i];
    double b_fp = bpb(&m);
    printf("[st-qalloc] seed %u: trained fixture 0..%d, calib even windows %d B, gate odd windows %d B; fp32 bpb %.4f\n",
           seed, fx_end, cn, gn, b_fp);

    size_t by_s = 0, by_a[NARM] = { 0 }, by_x = 0;
    int rs = st_quant_gptq(&m, &qs, 2, cal, cn, SEQ, &by_s);
    int ra[NARM];
    for (int a = 0; a < NARM; a++)
        ra[a] = st_quant_gptq_bits(&m, &qa[a], 2, arm_bits[a][0], arm_bits[a][1], arm_bits[a][2],
                                   cal, cn, SEQ, &by_a[a]);
    printf("[st-qalloc] rc seq %d E4 %d E3O3 %d A3 %d\n", rs, ra[0], ra[1], ra[2]);
    if (rs || ra[0] != ST_OK || ra[1] != ST_OK || ra[2] != ST_OK) { red_all(); return 1; }

    int okI = 1, ok0 = 1, okA = 1, ok1 = 1, okG4 = 1, okK = 1, okD = 1;

    /* I0 */
    {
        size_t by_m = 0;
        if (st_quant_gptq_mix(&m, &qx, 2, 4, 2, cal, cn, SEQ, &by_m) != ST_OK ||
            !same_w(&qx, &qa[0]) || by_m != by_a[0]) okI = 0;
        if (st_quant_gptq_bits(&m, &qx, 2, 2, 2, 2, cal, cn, SEQ, &by_x) != ST_OK ||
            !same_w(&qx, &qs) || by_x != by_s) okI = 0;
    }

    /* P0 */
    printf("[st-qalloc] bytes seq %zu", by_s);
    for (int a = 0; a < NARM; a++) {
        printf(" %s %zu (+%ld)", arm_name[a], by_a[a], (long)by_a[a] - (long)by_s);
        if (by_a[a] != by_s + 4096) ok0 = 0;
    }
    printf("\n");

    /* omega, pick, AUTO */
    double *F = (double *)malloc((size_t)m.n_params * sizeof(double));
    double om[NARM];
    int pick = -1, worst = 0;
    if (!F || st_quant_fisher(&m, cal, cn, SEQ, F) != ST_OK) okA = 0;
    for (int a = 0; a < NARM; a++) {
        om[a] = okA ? OMEGA(&m, F, 2, arm_bits[a][0], arm_bits[a][1], arm_bits[a][2]) : -1.0;
        if (!(om[a] > 0.0)) okA = 0;
        if (om[a] > om[worst]) worst = a;
    }
    if (okA) pick = st_quant_alloc_pick(om, NARM);
    printf("[st-qalloc] " OMEGA_NAME " E4 %.6g E3O3 %.6g A3 %.6g; pick %s; largest %s\n", om[0], om[1], om[2],
           pick >= 0 && pick < NARM ? arm_name[pick] : "none", arm_name[worst]);
#if defined(ST_QALLOC_EL)
    {
        double o5[NARM];
        int k5 = 0;
        for (int a = 0; a < NARM; a++) {
            o5[a] = okA ? st_quant_omega(&m, F, 2, arm_bits[a][0], arm_bits[a][1], arm_bits[a][2]) : -1.0;
            if (o5[a] < o5[k5]) k5 = a;
        }
        printf("[st-qalloc] value omega_mat E4 %.6g E3O3 %.6g A3 %.6g; smallest %s\n",
               o5[0], o5[1], o5[2], arm_name[k5]);
    }
#endif
#if defined(ST_QALLOC_CAL)
    /* stage 8 (research/brain-quant-alloc-cal.md §2): the arm whose built copy
     * loses least bpb on the node's own text b1_cal (held is never used) */
    int pick_om = pick;
    {
        double cf = bpb_buf(&m, calt, cln, NULL), cd[NARM];
        int cp = 0;
        for (int a = 0; a < NARM; a++) cd[a] = bpb_buf(&qa[a], calt, cln, NULL) - cf;
        for (int a = 1; a < NARM; a++) {
#if defined(ST_QALLOC_CAL_INVERT)
            if (cd[a] > cd[cp]) cp = a;   /* falsifier: P1 red */
#else
            if (cd[a] < cd[cp]) cp = a;
#endif
        }
        printf("[st-qalloc] cal damage E4 %.4f E3O3 %.4f A3 %.4f on %d windows; pick %s (" OMEGA_NAME " pick %s)\n",
               cd[0], cd[1], cd[2], nwin, arm_name[cp],
               pick_om >= 0 && pick_om < NARM ? arm_name[pick_om] : "none");
        if (okA) pick = cp;
    }
#endif
#if defined(ST_QALLOC_SRC)
    /* stage 9 (research/brain-quant-alloc-src.md §2): the same three arms, the
     * damage on other texts and the arm each would pick (values; the pick
     * stays CAL). S33 = b1_cal's first 33 windows, TAIL = the fixture's
     * windows after train_end (never trained), GATE = the first 33 gate
     * windows (trained). -DST_QALLOC_SRC_SWAP feeds S33 to the TAIL line. */
    static const char *src_name[3] = { "S33", "TAIL", "GATE" };
    const uint8_t *src_buf[3] = { calt, fx + fx_end, gate };
    int src_n[3] = { cln < 33 * SEQ ? cln : 33 * SEQ, fxn - fx_end, gn < 33 * SEQ ? gn : 33 * SEQ };
#if defined(ST_QALLOC_SRC_SWAP)
    src_buf[1] = src_buf[0]; src_n[1] = src_n[0];   /* falsifier: TAIL == S33 */
#endif
    double src_d[3][NARM];
    int src_pick[3], src_nw[3];
    for (int x = 0; x < 3; x++) {
        double sf = bpb_buf(&m, src_buf[x], src_n[x], NULL);
        src_pick[x] = 0;
        for (int a = 0; a < NARM; a++) {
            src_d[x][a] = bpb_buf(&qa[a], src_buf[x], src_n[x], NULL) - sf;
            if (a > 0 && src_d[x][a] < src_d[x][src_pick[x]]) src_pick[x] = a;
        }
        src_nw[x] = nwin;
    }
#endif
    if (pick < 0 || pick >= NARM) okA = 0;
    else if (st_quant_gptq_bits(&m, &qx, 2, arm_bits[pick][0], arm_bits[pick][1], arm_bits[pick][2],
                                cal, cn, SEQ, NULL) != ST_OK || !same_w(&qx, &qa[pick])) okA = 0;

    /* P1 */
    static double lw[NARM][MAXWIN];
    double d_s = bpb(&qs) - b_fp, d_a[NARM], d_min = 1e30;
    int kmin = 0;
    for (int a = 0; a < NARM; a++) {
        d_a[a] = bpb_w(&qa[a], lw[a]) - b_fp;
        if (d_a[a] < d_min) { d_min = d_a[a]; kmin = a; }
    }
    double d_auto = pick >= 0 && pick < NARM ? d_a[pick] : 1e30;
    printf("[st-qalloc] bpb damage seq %.4f E4 %.4f E3O3 %.4f A3 %.4f; AUTO %.4f (min %.4f)\n",
           d_s, d_a[0], d_a[1], d_a[2], d_auto, d_min);
    if (!(d_auto <= d_min + 0.02)) ok1 = 0;
    printf("[st-qalloc] value P2 %d (damage of the largest omega %s %.4f > AUTO %.4f)\n",
           d_a[worst] > d_auto ? 1 : 0, arm_name[worst], d_a[worst], d_auto);

    /* E (values, stage 7): the picked arm against the held minimum, E4 against
     * E3O3 (the two close arms), and A3 against the minimum (far apart: an
     * interval that can exclude 0) */
    {
        double mu, lo, hi;
        int p = pick >= 0 && pick < NARM ? pick : kmin;
        boot_ci(lw[p], lw[kmin], nwin, seed, &mu, &lo, &hi);
        printf("[st-qalloc] value E pick %s - min %s: mean %.4f ci95 [%.4f, %.4f] contains0 %d (%d windows, B %d)%s\n",
               arm_name[p], arm_name[kmin], mu, lo, hi, lo <= 0.0 && hi >= 0.0 ? 1 : 0, nwin, BOOT_B,
               p == kmin ? " same arm" : "");
        boot_ci(lw[0], lw[1], nwin, seed, &mu, &lo, &hi);
        printf("[st-qalloc] value E4-E3O3 mean %.4f ci95 [%.4f, %.4f] contains0 %d zero-width %d\n",
               mu, lo, hi, lo <= 0.0 && hi >= 0.0 ? 1 : 0, hi - lo == 0.0 ? 1 : 0);
        boot_ci(lw[2], lw[kmin], nwin, seed, &mu, &lo, &hi);
        printf("[st-qalloc] value A3-min mean %.4f ci95 [%.4f, %.4f] contains0 %d\n",
               mu, lo, hi, lo <= 0.0 && hi >= 0.0 ? 1 : 0);
#if defined(ST_QALLOC_CAL)
        /* C1 (stage 8): a clear miss = not the held minimum and the interval
         * of (pick - min) excludes 0; for the CAL pick and the omega_el pick */
        int miss_c = 0, miss_o = 0;
        boot_ci(lw[p], lw[kmin], nwin, seed, &mu, &lo, &hi);
        miss_c = p != kmin && !(lo <= 0.0 && hi >= 0.0);
        int po = pick_om >= 0 && pick_om < NARM ? pick_om : kmin;
        boot_ci(lw[po], lw[kmin], nwin, seed, &mu, &lo, &hi);
        miss_o = po != kmin && !(lo <= 0.0 && hi >= 0.0);
        printf("[st-qalloc] value C1 cal miss %d omega_el miss %d (cal pick %s, omega_el pick %s, held min %s)\n",
               miss_c, miss_o, arm_name[p], arm_name[po], arm_name[kmin]);
#endif
#if defined(ST_QALLOC_SRC)
        /* stage 9: the clear miss (C1's definition) of the arm each text picks */
        for (int x = 0; x < 3; x++) {
            int px = src_pick[x];
            boot_ci(lw[px], lw[kmin], nwin, seed, &mu, &lo, &hi);
            printf("[st-qalloc] value SRC %s damage E4 %.4f E3O3 %.4f A3 %.4f on %d windows; pick %s miss %d (held min %s)\n",
                   src_name[x], src_d[x][0], src_d[x][1], src_d[x][2], src_nw[x], arm_name[px],
                   px != kmin && !(lo <= 0.0 && hi >= 0.0) ? 1 : 0, arm_name[kmin]);
        }
#endif
    }

    /* G4 */
    long bad = off_grid(&m, &qs, 2, 2, 2, 2);
    for (int a = 0; a < NARM; a++) bad += off_grid(&m, &qa[a], 2, arm_bits[a][0], arm_bits[a][1], arm_bits[a][2]);
    int shared = -1;
    {
        st_held_report r;
        int rc = st_held_check_seen(held, hn, hn, 0, 1, cal, cn, ST_HELD_K, &r);
        shared = rc < 0 ? -1 : r.shared;
    }
    printf("[st-qalloc] off-grid %ld; calib vs B1 held: shared 16-byte runs %d\n", bad, shared);
    if (bad != 0 || shared != 0) okG4 = 0;
#if defined(ST_QALLOC_CAL)
    {
        st_held_report r;
        int rc = st_held_check_seen(held, hn, hn, 0, 1, calt, cln, ST_HELD_K, &r);
        int sh2 = rc < 0 ? -1 : r.shared;
        printf("[st-qalloc] b1_cal (%d B) vs B1 held: shared 16-byte runs %d\n", cln, sh2);
        if (sh2 != 0) okG4 = 0;
    }
#endif
#if defined(ST_QALLOC_SRC)
    {
        /* stage 9: TAIL vs held, and b1_cal vs the whole fixture (audit-81) */
        st_held_report r;
        int rc = st_held_check_seen(held, hn, hn, 0, 1, fx + fx_end, fxn - fx_end, ST_HELD_K, &r);
        int sh3 = rc < 0 ? -1 : r.shared;
        rc = st_held_check_seen(calt, cln, cln, 0, 1, fx, fxn, ST_HELD_K, &r);
        int sh4 = rc < 0 ? -1 : r.shared;
        printf("[st-qalloc] TAIL (%d B) vs B1 held: shared 16-byte runs %d; b1_cal vs fixture (%d B): %d\n",
               fxn - fx_end, sh3, fxn, sh4);
        if (sh3 != 0 || sh4 != 0) okG4 = 0;
    }
#endif

    /* K0, D1 */
    double g_s = st_quant_copy_kl(&m, &qs, gate, gn, SEQ);
    double g_a[NARM];
    for (int a = 0; a < NARM; a++) g_a[a] = st_quant_copy_kl(&m, &qa[a], gate, gn, SEQ);
    double g_self = st_quant_copy_kl(&m, &mc, gate, gn, SEQ);
    double l_s = kl_mean(&m, &qs, gate, gn), l_e = kl_mean(&m, &qa[0], gate, gn);
    printf("[st-qalloc] gate KL seq %.6f E4 %.6f E3O3 %.6f A3 %.6f self %.6f; libm seq %.6f E4 %.6f\n",
           g_s, g_a[0], g_a[1], g_a[2], g_self, l_s, l_e);
    if (!(fabs(g_s - l_s) <= 1e-3 * l_s) || !(fabs(g_a[0] - l_e) <= 1e-3 * l_e)) okK = 0;
    if (!(g_a[0] >= 0.0 && g_a[0] <= ST_QGATE_KL) || !(g_s > ST_QGATE_KL) ||
        !(g_self == 0.0)) okD = 0;

    /* D2 (value) */
    {
        st_model *qq[NARM + 1] = { &qs, &qa[0], &qa[1], &qa[2] };
        double gg[NARM + 1] = { g_s, g_a[0], g_a[1], g_a[2] };
        int agree = 0;
        printf("[st-qalloc] held KL");
        for (int a = 0; a < NARM + 1; a++) {
            double h = kl_mean(&m, qq[a], held, hn);
            printf(" %s %.6f", a == 0 ? "seq" : arm_name[a - 1], h);
            if ((h <= ST_QGATE_KL) == (gg[a] >= 0.0 && gg[a] <= ST_QGATE_KL)) agree++;
        }
        printf("\n[st-qalloc] value D2 %d/4\n", agree);
    }

    item(okI, "I0", "bits(2,4,2,2) == mix(2,4,2), bits(2,2,2,2) == gptq(2)");
    item(ok0, "P0", "bytes E4 == E3O3 == A3 == seq + 4096");
    item(okA, "A0", "AUTO is the picked arm; every omega > 0");
    item(ok1, "P1", "bpb damage AUTO <= min(E4, E3O3, A3) + 0.02");
    item(okG4, "G4", "every arm on its grid; calib shares no 16-byte run with held");
    item(okK, "K0", "st_quant_copy_kl == libm KL within 1e-3 relative");
    item(okD, "D1", "gate: E4 accepted, seq refused, self KL 0 accepted");
    free(F);
    st_free(&m); st_free(&mc); st_free(&qs); st_free(&qx);
    for (int a = 0; a < NARM; a++) st_free(&qa[a]);
    printf("[st-qalloc] %s\n", fails ? "RED" : "ALL PASS");
    return fails ? 1 : 0;
}
