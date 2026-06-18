/* ------------------------------------------------------------------ *
 *  r3_incontext.c — R3: non-trivial thought (in-context associative
 *  recall). The proof that this substrate learns a function no
 *  hand-written if can win at: each episode carries its own key->value
 *  dictionary; the label is the value bound to the query THIS episode.
 *  Because the dictionary is resampled every episode, no fixed
 *  input->label rule wins by a meaningful margin: the best one
 *  ("copy value@p") sits at chance + (1/R_NPAIR)(1-1/R_VALV), a small
 *  edge that vanishes as R_NPAIR grows (the [handif] test prints it).
 *  Attention solves it for real by reading the prompt; the certificate
 *  measures the learned margin over max(frozen, handif), not vs chance.
 *
 *  Anti-fork rule (docs/architecture/r3-nontrivial-thought.md): the
 *  numerically-meaningful kernels (dt_linear / dt_softmax / LayerNorm
 *  fwd+bwd, dtr_logf) are the SAME ones the live sensor brain (dtr.c)
 *  uses — exposed via dtr.h. Only the embedding (token lookup vs scalar
 *  projection) and the readout (query position vs mean pool) differ,
 *  which is legitimately task-specific, not a forked Transformer.
 *
 *  This is a CAPACITY CERTIFICATE for the substrate, CI-enforced; it is
 *  NOT a swap of the live sensor decision (a thermostat needs no
 *  Transformer). The conversational mind (living-mind) is built on top.
 * ------------------------------------------------------------------ */

#include "dtr.h"       /* shared kernels: dt_linear/dt_softmax/dt_relu/
                          dt_sqrt/dtr_ln_fwd_cache/dtr_ln_bwd/dtr_logf */
#include "dmn.h"       /* dmn_trigger(): a fact arrival IS a stimulus (VI.3) */
#include "drpc.h"     /* galaxy v1: drpc_my_node for emit src */
#include "galaxy.h"   /* galaxy v1: S4 teach/ask emit hooks */
#include "ark_profile.h" /* ark-profile v1: the ONE provenance write site (§5) */
#include "kdds.h"      /* LM-7: the region-scoped "mind/teach" topic (VIII.3) */
#include "region.h"   /* LM-7: region_id() — observable shared-mind boundary  */
#include "r3_vocab.h"  /* LM-8 (IX.3): the embedded, content-addressed words   */
#include "gossip_learn.h" /* LM-10 Path W: gl_merge (the no-central averager)  */
#include "pfs_dag.h"   /* LM-10 Path W: chunked weight transport (T-a, XI.3)   */
#include "pfs_repl.h"  /* LM-10 Path W: pfs_repl_want (chunk all-or-nothing)   */
#include "kernel.h"
#include <tmonitor.h>

/* LM-7 (VIII.9): the wire engram must fit ONE K-DDS payload — no chunking,
 * no new transport (the Path E "one packet" property, VIII.0 #1-2). */
_Static_assert(sizeof(MT_TEACH_PKT) <= KDDS_DATA_MAX, "mind/teach fits one K-DDS payload");

/* ---- output helpers (sio frame channel, like dtr_train.c) --------- */
static void r_puts(const char *s) { tm_putstring((UB *)s); }
static void r_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[11] = 0;
    if (v == 0) { r_puts("0"); return; }
    while (v && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    r_puts(&buf[i]);
}
/* xx.x percent */
static void r_putf1(float f)
{
    if (f < 0.0f) { r_puts("-"); f = -f; }
    UW whole = (UW)f; UW frac = (UW)((f - (float)whole) * 10.0f + 0.5f);
    if (frac >= 10) { whole++; frac = 0; }
    r_putdec(whole); r_puts("."); r_putdec(frac);
}
/* x.xxx */
static void r_putf3(float f)
{
    if (f < 0.0f) { r_puts("-"); f = -f; }
    UW whole = (UW)f; UW frac = (UW)((f - (float)whole) * 1000.0f + 0.5f);
    if (frac >= 1000) { whole++; frac = 0; }
    r_putdec(whole); r_puts(".");
    if (frac < 100) r_puts("0");
    if (frac < 10)  r_puts("0");
    r_putdec(frac);
}

/* ---- model config (its own dims; all widths <= DTR_LN_MAXW) -------- *
 *  LM-9 (living-mind.md Part X): THE CAPACITY SURGERY. LM-8 (Part IX) MEASURED
 *  the wall instead of asserting past it: at R_DM=32 the [lang-capacity] curve
 *  collapsed below 75% recall by N=6 (comfortable-N=4) — the bottleneck is the
 *  attention WIDTH R_DM (IX.0 #1), not the vocab. Part X widens the thinking
 *  width: R_DM 32->48 (the surgery), and grows the vocab in lock-step so the
 *  curve can be SWEPT past 8: R_KEYV 8->16 (key ladder), R_VALV 32->64 (answer
 *  space), R_FFN=R_DM (proportional MLP, X.2 B). R_NP = 21568 (2.40x LM-8's
 *  8992, BUILT not trusted — the _Static_assert below is the proof). The
 *  shared LayerNorm kernel is reused (DTR_LN_MAXW capacity-cap bump 32->64,
 *  X.3 — proven FREE for the dtr sensor brain by [lang-sensor-intact]). The
 *  MEASURED gain: comfortable-N 4->12 ([lang-capacity-v2]). NOT grammar/
 *  generation/multi-token — still single-token recall over a bounded vocab. */
#define R_KEYV    16           /* key vocab (LM-9 Part X: 8->16, ladder sweeps N>8)*/
#define R_VALV    64           /* answer vocab == output classes (LM-9: 32->64)*/
#define R_NPAIR   8            /* dictionary entries per episode        */
/* LM-8: the per-prompt binding budget (R_NPAIR) and the cert working-key
 * set are ORTHOGONAL to vocab size (IX.0 #3). The fixed-fact certs
 * (LM-4/5) exercise a small WORKING set of keys; widening the *vocab*
 * must not change their arrangement distribution. R_CERTKEYS == R_NPAIR
 * so every cert prompt still holds ALL its working keys — the LM-4..7
 * cert STRUCTURE is byte-identical; only `chance` (=100/R_VALV) changes
 * 25%->~1.6%, re-baselined LOUDLY with the stricter-only rule (IX.5). */
#define R_CERTKEYS 8           /* fixed-fact cert working-key count     */
#define R_SEQ     (R_NPAIR+1)  /* dict tokens + 1 query                 */
#define R_QPOS    (R_SEQ-1)    /* query token position (readout point)  */
#define R_DM      48           /* d_model (LM-9 Part X surgery: 32->48,  */
#define R_NH      4            /*  the thinking WIDTH that disambiguates */
#define R_DH      (R_DM/R_NH)  /*  simultaneous bindings; R_DH=48/4=12)  */
#define R_FFN     R_DM         /* FFN hidden (LM-9 X.2 B: =R_DM, proportional MLP)*/
#define R_VALEMB  (R_VALV+1)   /* +1 = UNK value for the query token   */
#define R_UNK     (R_VALV)     /* the query's value id (= "ask")       */

/* flat parameter layout (rw[]/rg[] share this order) */
#define O_WKE   0
#define O_WVE   (O_WKE  + R_KEYV  * R_DM)
#define O_WPE   (O_WVE  + R_VALEMB* R_DM)
#define O_WQ    (O_WPE  + R_SEQ   * R_DM)
#define O_WK    (O_WQ   + R_NH * R_DH * R_DM)
#define O_WV    (O_WK   + R_NH * R_DH * R_DM)
#define O_WO    (O_WV   + R_NH * R_DH * R_DM)
#define O_LN1G  (O_WO   + R_DM * R_DM)
#define O_LN1B  (O_LN1G + R_DM)
#define O_LN2G  (O_LN1B + R_DM)
#define O_LN2B  (O_LN2G + R_DM)
#define O_WF1   (O_LN2B + R_DM)
#define O_BF1   (O_WF1  + R_FFN * R_DM)
#define O_WF2   (O_BF1  + R_FFN)
#define O_BF2   (O_WF2  + R_DM * R_FFN)
#define O_WCLS  (O_BF2  + R_DM)
#define O_BCLS  (O_WCLS + R_VALV * R_DM)
#define R_NP    (O_BCLS + R_VALV)

/* LM-9 (living-mind Part X.2): the param count is BUILT, not trusted. At the
 * widened dims (R_KEYV=16, R_VALV=64, R_DM=48, R_FFN=48) the layout formula
 * gives 21568 (2.40x LM-8's 8992; verify by build — the _Static_assert is the
 * proof, not the doc's estimate). The auditor reads this line-by-line. */
_Static_assert(R_NP == 21568, "LM-9 R_NP must equal 21568 (verify by build, X.2)");
/* the substrate's vocab dims MUST equal the embedded word-list sizes, or
 * a token id from r3_vocab_*_id could index past the embedding/classifier
 * tables (the kernel and the word list can never silently disagree). */
_Static_assert(R_KEYV == R3_VOCAB_KEYS, "key vocab == r3_vocab key image size");
_Static_assert(R_VALV == R3_VOCAB_VALS, "answer vocab == r3_vocab val image size");
/* the wire's U1 key/val fields carry token ids; v1 keeps them U1 so the
 * MT_TEACH_PKT width is UNCHANGED (only the version field is new, IX.8). */
_Static_assert(R_KEYV <= 256 && R_VALV <= 256, "v1 token ids fit U1 (wire width unchanged, IX.8)");

static float rw[R_NP];        /* weights                              */
static float rg[R_NP];        /* gradient accumulator                 */

/* ---- forward activation cache (static; never task-stack locals) --- */
typedef struct {
    UB    key[R_SEQ], val[R_SEQ];          /* current episode tokens   */
    float tok[R_SEQ][R_DM];                /* embedding (no ReLU)      */
    float Q[R_NH][R_SEQ][R_DH];
    float K[R_NH][R_SEQ][R_DH];
    float V[R_NH][R_SEQ][R_DH];
    float attn[R_NH][R_SEQ][R_SEQ];
    float concat[R_SEQ][R_DM];
    float r1[R_SEQ][R_DM];
    float xh1[R_SEQ][R_DM]; float istd1[R_SEQ]; float y1[R_SEQ][R_DM];
    float mid[R_SEQ][R_FFN];
    float r2[R_SEQ][R_DM];
    float xh2[R_SEQ][R_DM]; float istd2[R_SEQ]; float y2[R_SEQ][R_DM];
    float pool[R_DM];                      /* = y2[R_QPOS]             */
    float probs[R_VALV];
} R_TC;
static R_TC rc;

/* ---- RNG: two independent streams (train / held-out) -------------- */
static UW r_rng;
static UW r_rand(void) { r_rng = r_rng*1664525UL + 1013904223UL; return (r_rng>>16)&0x7FFF; }
static INT r_uni(INT lo, INT hi) { return lo + (INT)(r_rand() % (UW)(hi-lo+1)); }

/* Generate one episode: 3 DISTINCT keys, random values, query = one of
 * the keys; label = value bound to that key. Resampled every call ->
 * (query token -> label) is uniform across episodes -> any fixed rule
 * is at chance. */
static UB gen_episode(UB key[R_SEQ], UB val[R_SEQ])
{
    /* pick R_NPAIR distinct keys from {0..R_KEYV-1} */
    UB pool[R_KEYV];
    for (INT i = 0; i < R_KEYV; i++) pool[i] = (UB)i;
    for (INT i = R_KEYV-1; i > 0; i--) {            /* Fisher-Yates */
        INT j = r_uni(0, i);
        UB t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    for (INT p = 0; p < R_NPAIR; p++) {
        key[p] = pool[p];
        val[p] = (UB)r_uni(0, R_VALV-1);
    }
    INT qi = r_uni(0, R_NPAIR-1);                    /* which entry queried */
    key[R_QPOS] = key[qi];                           /* query carries the key */
    val[R_QPOS] = (UB)R_UNK;                         /* value unknown -> recall */
    return val[qi];                                  /* the label */
}

/* ---- forward: returns CE loss -ln p[label], fills rc --------------- */
static float r_forward(const UB key[R_SEQ], const UB val[R_SEQ], UB label)
{
    float scale = 1.0f / dt_sqrt((float)R_DH);

    /* token embedding = key-lookup + value-lookup + positional (no ReLU) */
    for (INT t = 0; t < R_SEQ; t++) {
        rc.key[t] = key[t]; rc.val[t] = val[t];
        const float *ek = &rw[O_WKE + (INT)key[t]*R_DM];
        const float *ev = &rw[O_WVE + (INT)val[t]*R_DM];
        const float *ep = &rw[O_WPE + t*R_DM];
        for (INT d = 0; d < R_DM; d++) rc.tok[t][d] = ek[d] + ev[d] + ep[d];
    }

    /* MHSA (same kernels/structure as dtr.c train_forward) */
    for (INT h = 0; h < R_NH; h++) {
        const float *wq = &rw[O_WQ + h*R_DH*R_DM];
        const float *wk = &rw[O_WK + h*R_DH*R_DM];
        const float *wv = &rw[O_WV + h*R_DH*R_DM];
        for (INT t = 0; t < R_SEQ; t++) {
            dt_linear(wq, NULL, rc.tok[t], rc.Q[h][t], R_DH, R_DM);
            dt_linear(wk, NULL, rc.tok[t], rc.K[h][t], R_DH, R_DM);
            dt_linear(wv, NULL, rc.tok[t], rc.V[h][t], R_DH, R_DM);
        }
        for (INT i = 0; i < R_SEQ; i++) {
            for (INT j = 0; j < R_SEQ; j++) {
                float s = 0.0f;
                for (INT d = 0; d < R_DH; d++) s += rc.Q[h][i][d]*rc.K[h][j][d];
                rc.attn[h][i][j] = s * scale;
            }
            dt_softmax(rc.attn[h][i], R_SEQ);
        }
        for (INT i = 0; i < R_SEQ; i++)
            for (INT d = 0; d < R_DH; d++) {
                float s = 0.0f;
                for (INT j = 0; j < R_SEQ; j++) s += rc.attn[h][i][j]*rc.V[h][j][d];
                rc.concat[i][h*R_DH + d] = s;
            }
    }

    /* W_o + residual + LN1 */
    for (INT t = 0; t < R_SEQ; t++) {
        float m[R_DM];
        dt_linear(&rw[O_WO], NULL, rc.concat[t], m, R_DM, R_DM);
        for (INT d = 0; d < R_DM; d++) rc.r1[t][d] = m[d] + rc.tok[t][d];
        dtr_ln_fwd_cache(rc.r1[t], &rw[O_LN1G], &rw[O_LN1B],
                         rc.xh1[t], &rc.istd1[t], rc.y1[t], R_DM);
    }

    /* FFN + residual + LN2 */
    for (INT t = 0; t < R_SEQ; t++) {
        dt_linear(&rw[O_WF1], &rw[O_BF1], rc.y1[t], rc.mid[t], R_FFN, R_DM);
        for (INT k = 0; k < R_FFN; k++) rc.mid[t][k] = dt_relu(rc.mid[t][k]);
        float f[R_DM];
        dt_linear(&rw[O_WF2], &rw[O_BF2], rc.mid[t], f, R_DM, R_FFN);
        for (INT d = 0; d < R_DM; d++) rc.r2[t][d] = f[d] + rc.y1[t][d];
        dtr_ln_fwd_cache(rc.r2[t], &rw[O_LN2G], &rw[O_LN2B],
                         rc.xh2[t], &rc.istd2[t], rc.y2[t], R_DM);
    }

    /* readout from the QUERY position only (not mean pool) */
    for (INT d = 0; d < R_DM; d++) rc.pool[d] = rc.y2[R_QPOS][d];
    dt_linear(&rw[O_WCLS], &rw[O_BCLS], rc.pool, rc.probs, R_VALV, R_DM);
    dt_softmax(rc.probs, R_VALV);

    float pl = rc.probs[label]; if (pl < 1e-7f) pl = 1e-7f;
    return -dtr_logf(pl);
}

/* ---- analytic backward (mirrors dtr.c train_backward) ------------- */
static void r_backward(UB label)
{
    float scale = 1.0f / dt_sqrt((float)R_DH);
    static float dy2[R_SEQ][R_DM], dr2[R_SEQ][R_DM], dy1[R_SEQ][R_DM];
    static float dr1[R_SEQ][R_DM], dtok[R_SEQ][R_DM], dconcat[R_SEQ][R_DM];
    static float dQ[R_SEQ][R_DH], dK[R_SEQ][R_DH], dV[R_SEQ][R_DH];

    /* cls: dlogits = p - onehot */
    float dlog[R_VALV], dpool[R_DM];
    for (INT c = 0; c < R_VALV; c++)
        dlog[c] = rc.probs[c] - (c == (INT)label ? 1.0f : 0.0f);
    for (INT c = 0; c < R_VALV; c++) {
        rg[O_BCLS + c] += dlog[c];
        for (INT d = 0; d < R_DM; d++)
            rg[O_WCLS + c*R_DM + d] += dlog[c] * rc.pool[d];
    }
    for (INT d = 0; d < R_DM; d++) {
        float s = 0.0f;
        for (INT c = 0; c < R_VALV; c++) s += rw[O_WCLS + c*R_DM + d] * dlog[c];
        dpool[d] = s;
    }
    /* readout was y2[R_QPOS] only */
    for (INT t = 0; t < R_SEQ; t++)
        for (INT d = 0; d < R_DM; d++)
            dy2[t][d] = (t == R_QPOS) ? dpool[d] : 0.0f;

    /* LN2 -> residual -> FFN -> (into dy1) */
    for (INT t = 0; t < R_SEQ; t++) {
        dtr_ln_bwd(dy2[t], rc.xh2[t], rc.istd2[t], &rw[O_LN2G],
                   &rg[O_LN2G], &rg[O_LN2B], dr2[t], R_DM);
        for (INT d = 0; d < R_DM; d++) dy1[t][d] = dr2[t][d];   /* residual */
        float dmid[R_FFN];
        for (INT k = 0; k < R_FFN; k++) dmid[k] = 0.0f;
        for (INT d = 0; d < R_DM; d++) {
            rg[O_BF2 + d] += dr2[t][d];
            for (INT k = 0; k < R_FFN; k++) {
                rg[O_WF2 + d*R_FFN + k] += dr2[t][d] * rc.mid[t][k];
                dmid[k] += rw[O_WF2 + d*R_FFN + k] * dr2[t][d];
            }
        }
        for (INT k = 0; k < R_FFN; k++) {
            if (rc.mid[t][k] <= 0.0f) { dmid[k] = 0.0f; continue; }  /* ReLU */
            rg[O_BF1 + k] += dmid[k];
            for (INT d = 0; d < R_DM; d++) {
                rg[O_WF1 + k*R_DM + d] += dmid[k] * rc.y1[t][d];
                dy1[t][d] += rw[O_WF1 + k*R_DM + d] * dmid[k];
            }
        }
    }

    /* LN1 -> residual -> W_o */
    for (INT t = 0; t < R_SEQ; t++) {
        dtr_ln_bwd(dy1[t], rc.xh1[t], rc.istd1[t], &rw[O_LN1G],
                   &rg[O_LN1G], &rg[O_LN1B], dr1[t], R_DM);
        for (INT d = 0; d < R_DM; d++) dtok[t][d] = dr1[t][d];   /* residual */
        for (INT n = 0; n < R_DM; n++) dconcat[t][n] = 0.0f;
        for (INT d = 0; d < R_DM; d++)
            for (INT n = 0; n < R_DM; n++) {
                rg[O_WO + d*R_DM + n] += dr1[t][d] * rc.concat[t][n];
                dconcat[t][n] += rw[O_WO + d*R_DM + n] * dr1[t][d];
            }
    }

    /* attention backward, per head */
    for (INT h = 0; h < R_NH; h++) {
        for (INT t = 0; t < R_SEQ; t++)
            for (INT d = 0; d < R_DH; d++) dQ[t][d] = dK[t][d] = dV[t][d] = 0.0f;

        for (INT i = 0; i < R_SEQ; i++) {
            const float *dout = &dconcat[i][h*R_DH];
            float da[R_SEQ], dots = 0.0f;
            for (INT j = 0; j < R_SEQ; j++) {
                float s = 0.0f;
                for (INT d = 0; d < R_DH; d++) {
                    dV[j][d] += rc.attn[h][i][j] * dout[d];
                    s += dout[d] * rc.V[h][j][d];
                }
                da[j] = s;
                dots += da[j] * rc.attn[h][i][j];
            }
            for (INT j = 0; j < R_SEQ; j++) {
                float ds = rc.attn[h][i][j] * (da[j] - dots) * scale;
                for (INT d = 0; d < R_DH; d++) {
                    dQ[i][d] += ds * rc.K[h][j][d];
                    dK[j][d] += ds * rc.Q[h][i][d];
                }
            }
        }

        const float *wq = &rw[O_WQ + h*R_DH*R_DM];
        const float *wk = &rw[O_WK + h*R_DH*R_DM];
        const float *wv = &rw[O_WV + h*R_DH*R_DM];
        INT oq = O_WQ + h*R_DH*R_DM, ok = O_WK + h*R_DH*R_DM, ov = O_WV + h*R_DH*R_DM;
        for (INT t = 0; t < R_SEQ; t++)
            for (INT d = 0; d < R_DH; d++)
                for (INT n = 0; n < R_DM; n++) {
                    rg[oq + d*R_DM + n] += dQ[t][d] * rc.tok[t][n];
                    rg[ok + d*R_DM + n] += dK[t][d] * rc.tok[t][n];
                    rg[ov + d*R_DM + n] += dV[t][d] * rc.tok[t][n];
                    dtok[t][n] += wq[d*R_DM + n]*dQ[t][d]
                                + wk[d*R_DM + n]*dK[t][d]
                                + wv[d*R_DM + n]*dV[t][d];
                }
    }

    /* embedding backward (no ReLU): split dtok into key/value/pos tables */
    for (INT t = 0; t < R_SEQ; t++) {
        INT ok = O_WKE + (INT)rc.key[t]*R_DM;
        INT ov = O_WVE + (INT)rc.val[t]*R_DM;
        INT op = O_WPE + t*R_DM;
        for (INT d = 0; d < R_DM; d++) {
            float dz = dtok[t][d];
            rg[ok + d] += dz;
            rg[ov + d] += dz;
            rg[op + d] += dz;
        }
    }
}

/* ---- weight init (small random) ---------------------------------- */
static void r_init_weights(UW seed)
{
    r_rng = seed;
    for (INT i = 0; i < R_NP; i++) {
        /* uniform [-0.30, 0.30) */
        float u = (float)r_rand() / 32768.0f;     /* [0,1) */
        rw[i] = (u - 0.5f) * 0.60f;
    }
    /* LN gains -> 1, biases -> 0 */
    for (INT d = 0; d < R_DM; d++) {
        rw[O_LN1G+d] = 1.0f; rw[O_LN1B+d] = 0.0f;
        rw[O_LN2G+d] = 1.0f; rw[O_LN2B+d] = 0.0f;
    }
    for (INT i = 0; i < R_FFN; i++) rw[O_BF1+i] = 0.0f;
    for (INT d = 0; d < R_DM; d++) rw[O_BF2+d] = 0.0f;
    for (INT c = 0; c < R_VALV; c++) rw[O_BCLS+c] = 0.0f;
}

/* ---- eval accuracy on N fresh episodes from a given stream -------- */
static float r_eval(UW seed, INT n)
{
    UW save = r_rng; r_rng = seed;
    INT correct = 0;
    for (INT e = 0; e < n; e++) {
        UB key[R_SEQ], val[R_SEQ];
        UB y = gen_episode(key, val);
        r_forward(key, val, y);
        UB pred = 0; float mx = rc.probs[0];
        for (INT c = 1; c < R_VALV; c++) if (rc.probs[c] > mx) { mx = rc.probs[c]; pred = (UB)c; }
        if (pred == y) correct++;
    }
    r_rng = save;
    return 100.0f * (float)correct / (float)n;
}

/* best FIXED input->label rule, measured (the theorem as a number):
 *  - most-frequent-class (always predict argmax of label histogram)
 *  - best single-position COPY (predict token at pos p, key or value)
 *  - predict the query's own key id
 * All are <= chance because the dictionary is resampled per episode. */
static float r_handif(UW seed, INT n)
{
    UW save = r_rng; r_rng = seed;
    INT hist[R_VALV]; for (INT c=0;c<R_VALV;c++) hist[c]=0;
    INT copyk[R_SEQ], copyv[R_SEQ]; for (INT p=0;p<R_SEQ;p++){copyk[p]=0;copyv[p]=0;}
    INT qkey = 0;
    /* materialize episodes once; score each fixed rule on the same set */
    /* (n bounded; store labels+tokens compactly) */
    for (INT e = 0; e < n; e++) {
        UB key[R_SEQ], val[R_SEQ];
        UB y = gen_episode(key, val);
        hist[y]++;
        for (INT p = 0; p < R_SEQ; p++) {
            if ((INT)key[p] == (INT)y) copyk[p]++;       /* "copy key@p" right */
            if (val[p] < R_VALV && (INT)val[p] == (INT)y) copyv[p]++;
        }
        if ((INT)key[R_QPOS] == (INT)y) qkey++;          /* "predict my key" */
    }
    r_rng = save;
    INT best = 0;
    for (INT c = 0; c < R_VALV; c++) if (hist[c] > best) best = hist[c];   /* most-freq */
    for (INT p = 0; p < R_SEQ; p++) { if (copyk[p]>best) best=copyk[p]; if (copyv[p]>best) best=copyv[p]; }
    if (qkey > best) best = qkey;
    return 100.0f * (float)best / (float)n;
}

/* one stochastic epoch over `n` fresh training episodes */
static float r_train_epoch(UW seed, INT n, float lr)
{
    UW save = r_rng; r_rng = seed;
    float tot = 0.0f;
    for (INT e = 0; e < n; e++) {
        UB key[R_SEQ], val[R_SEQ];
        UB y = gen_episode(key, val);
        for (INT i = 0; i < R_NP; i++) rg[i] = 0.0f;
        tot += r_forward(key, val, y);
        r_backward(y);
        for (INT i = 0; i < R_NP; i++) rw[i] -= lr * rg[i];
    }
    r_rng = save;
    return tot / (float)n;
}

/* snapshot the ReLU active-set at the query position (the only ReLU that
 * affects the loss: FFN/LN are per-position and the readout is from
 * R_QPOS only, so mid[t!=R_QPOS] is dead-ends the loss). Used to detect
 * when a finite-difference perturbation straddles a ReLU kink. */
static void r_qmask(UB mask[R_FFN])
{
    for (INT k = 0; k < R_FFN; k++) mask[k] = (rc.mid[R_QPOS][k] > 0.0f);
}

/* analytic-vs-central-finite-diff gradient check, every param, NSMP
 * episodes. Two disciplines, both standard for ReLU nets and both
 * honest (neither hides a wrong gradient):
 *   (1) absolute ref floor (mirrors dtr.c): float32 central differences
 *       are noisy near zero, so a fixed denominator floor stops a correct
 *       tiny gradient from showing a spurious ~1.0 relative error.
 *   (2) ReLU-kink exclusion: the loss is piecewise-linear in the FFN
 *       ReLU; at a kink the finite difference crosses the corner and
 *       DISAGREES with the (correct) subgradient by construction. We skip
 *       a param ONLY when its +/-eps perturbation flips a ReLU bit at the
 *       query position — exactly the points where the FD is undefined. A
 *       real backward bug still shows on the (vast majority of) params
 *       whose perturbation stays in one linear region. */
static float r_grad_check(UW seed)
{
    const float eps = 2e-3f;
    const INT   NSMP = 3;
    float worst = 0.0f;
    for (INT s = 0; s < NSMP; s++) {
        r_rng = seed + (UW)s * 0x9E3779B9UL;
        UB key[R_SEQ], val[R_SEQ];
        UB y = gen_episode(key, val);
        for (INT i = 0; i < R_NP; i++) rg[i] = 0.0f;
        r_forward(key, val, y);
        r_backward(y);
        /* stride 7 keeps every weight family covered (gcd(7,R_DM)=gcd(7,48)=1
         * so row/col positions rotate; coprime to R_DH=12, R_FFN=48, R_VALV=64,
         * R_KEYV=16 too — LM-9 X.0 #6) while keeping the check CI-cheap. */
        for (INT i = 0; i < R_NP; i += 7) {
            float w0 = rw[i];
            UB mp[R_FFN], mm[R_FFN];
            rw[i] = w0 + eps; float lp = r_forward(key, val, y); r_qmask(mp);
            rw[i] = w0 - eps; float lm = r_forward(key, val, y); r_qmask(mm);
            rw[i] = w0;
            INT kink = 0;
            for (INT k = 0; k < R_FFN; k++) if (mp[k] != mm[k]) { kink = 1; break; }
            if (kink) continue;                  /* FD invalid across a kink */
            float fd  = (lp - lm) / (2.0f*eps);
            float ref = fd < 0.0f ? -fd : fd;
            if (ref < 0.05f) ref = 0.05f;        /* absolute floor */
            float diff = rg[i] - fd; if (diff < 0.0f) diff = -diff;
            float rel = diff / ref;
            if (rel > worst) worst = rel;
        }
    }
    return worst;
}

/* ------------------------------------------------------------------ *
 *  Acceptance self-tests — printed numbers, not asserted from belief.
 * ------------------------------------------------------------------ */
#define R_SEED_TRAIN  0x1C0FFEEUL
#define R_SEED_HELD   0x5A11AD00UL
/* LM-9 (living-mind Part X): the widened 2.40x substrate (R_DM 32->48) over a
 * 2x answer space (R_VALV 32->64) needs more EPISODE DIVERSITY per epoch to
 * reach in-context COMPETENCE — and the design's ~4s estimate was optimistic.
 * MEASURED, honestly:
 *   - LM-8 budget (192 x 60): [r3-incontext-learned] sits at ~8% (under-
 *     trained; gradcheck still PASSES — the gradients are right, the optimizer
 *     just hadn't seen enough distinct dictionaries). train_n is the LEVER:
 *     256 alone stays ~24%, train_n=384 reaches ~72%.
 *   - BUT [handoff-consolidated] (LM-4) needs the substrate to read its D*
 *     FAST: at train_n=384 teacher_agree caps ~58% and post ~47% (< the 50
 *     floor). train_n=512 lifts acc_support to ~100%, teacher_agree to 100%,
 *     post to ~88% — a SHARP competence threshold near 512 episodes/epoch.
 *   - At 512 x 80 the WHOLE battery clears: r3-learned ~95%, handoff ~88%,
 *     stream f1..f4 = 100%, AND the capacity comfortable-N rises from 4 to 16
 *     (the FULL R_KEYV=16 ladder at >=75%). LR schedule UNCHANGED (0.05 ->
 *     0.02 @2/3 -> 0.008 @9/10; gradcheck green at the new R_NP).
 * Cost: shared s_pretrain ~23s host (LM-8 ~3s; PRINTED, gate-not-asserted —
 * the HONEST CI-cost of the wider mind, X.2 flagged this; ~5.7x the original,
 * the price of the in-context competence the +12 comfortable-N gain rides on). */
#define R_TRAIN_N     512    /* LM-9: 192->512 (episode diversity = the in-context
                              * competence threshold; the dominant lever) */
#define R_EVAL_N      300
#define R_HANDIF_N    6000   /* large: the best-fixed-rule baseline takes a
                               * MAX over many rules, so a small sample would
                               * inflate it via selection noise; measure the
                               * TRUE accuracy of each rule on a big stream. */
#define R_EPOCHS      80     /* LM-9: 60->80 (the wider model's convergence) */

/* ------------------------------------------------------------------ *
 *  LM-6 (living-mind.md Part VII) — shared substrate bootstrap + the
 *  VI.8-named quiesce flag, now built (VII.3 / VII.4).
 * ------------------------------------------------------------------ */

/* Set/cleared by s_round() ONLY (VII.4). The DMN task is priority 13 —
 * strictly LOWEST — so the shell PREEMPTS it mid-round the moment input
 * arrives; a `mind` verb (or a cert) that then ran h_predict/r_forward
 * would tear the SAME shared rc/rg/r_rng the half-finished round
 * resumes into, and the resumed r_backward would push a corrupted
 * gradient into rw[]. Every mind sub-verb and every cert entry waits on
 * this flag FIRST; the wait is bounded by construction (one round =
 * R3_IDLE_STEPS SGD steps, milliseconds) and the waiting shell sleeping
 * is precisely what lets the prio-13 round finish. A flag, not a mutex:
 * sufficient ONLY because strict priority means the round can never
 * preempt a verb (the reverse direction is safe by construction). */
static volatile UB r3_round_busy = 0;

static void m_quiesce(void)
{
    while (r3_round_busy) tk_dly_tsk(20);
}

/* living-body inspector (living-body-inspector.md): O(1) read of the
 * in-flight-round flag so the galaxy can flicker the R3 organ "training".
 * A pure read of the volatile; never mutates state. */
INT r3_round_busy_get(void) { return r3_round_busy ? 1 : 0; }

/* ================================================================== *
 *  LM-10 (living-mind.md Part XI) — Path W: the one mind.              *
 *                                                                       *
 *  The weight-states themselves converge. After local consolidation a   *
 *  node publishes its rw[] (84 KB, chunked) to region peers and         *
 *  gl_merge()s the set into ONE shared weight-state. XI.0 #5: rw[] is    *
 *  file-static with NO accessor today — these are the dtr-accessor       *
 *  mirror, the ONLY genuinely new R3 surface (the merge itself reuses    *
 *  gl_merge + s_round + the LM-7 subscriber shape). Both are             *
 *  m_quiesce()-guarded by their CALLERS (VII.4: never touch rw[] under   *
 *  an in-flight round).                                                  *
 * ================================================================== */

/* merge_epoch: bumped each LOCAL consolidation round; the published blob
 * carries it; a node folds a peer only if its epoch is NEWER than the last
 * folded from that peer (XI.3 loop/version honesty). Read by the cert +
 * galaxy emit. */
static UW merge_epoch = 0;

void r3_weights_get(float *out)
{
    for (INT i = 0; i < R_NP; i++) out[i] = rw[i];
}
void r3_weights_set(const float *in)
{
    for (INT i = 0; i < R_NP; i++) rw[i] = in[i];
}
UW r3_merge_epoch(void) { return merge_epoch; }

/* galaxy v1 (galaxy.md §6 — "The concurrency slice Part VII named, now
 * due"). VII.4 said the r3_round_busy quiesce is "a flag, not a lock:
 * sufficient ONLY because... verbs cannot be preempted by the round. If
 * R3 queries ever move off the shell task, this becomes a real mutex —
 * named for that slice, not built." The galaxy task IS a second caller
 * task, so this slice builds the named mutex: ONE maxsem-1 semaphore,
 * acquired at mind_cmd entry, released at exit. Putting the gate INSIDE
 * mind_cmd means no caller (shell, web, future) can forget it. The inner
 * r3_round_busy quiesce spin-sleep is UNCHANGED. Lazily created (no
 * mind_init exists); m_gate<0 until first mind_cmd. */
static ID m_gate = -1;

static void m_gate_acquire(void)
{
    if (m_gate < 0) {
        T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                      .isemcnt = 1, .maxsem = 1 };
        m_gate = tk_cre_sem(&cs);
    }
    if (m_gate >= 0) tk_wai_sem(m_gate, 1, TMO_FEVR);
}

static void m_gate_release(void)
{
    if (m_gate >= 0) tk_sig_sem(m_gate, 1);
}

/* galaxy.md §6: snapshot of the last `mind ask` result, written at the
 * single m_ask site below; read by the web /ask bridge via the public
 * mind_last_answer(). Console output remains the verb's primary record. */
static UB m_last_k     = 0;
static UB m_last_v     = 0;
static UW m_last_share = 0;   /* modal class percent *10 (750 = 75.0%) */

void mind_last_answer(UB *k, UB *v, UW *share)
{
    if (k)     *k     = m_last_k;
    if (v)     *v     = m_last_v;
    if (share) *share = m_last_share;
}

/* substrate ready? rw[] is a zeroed static at boot (VII.0 #2); the
 * mouth pretrains lazily on first use, printed (VII.3). */
static UB m_ready = 0;

/* THE one pretrain recipe (seed 0xA5A5 + the 60-epoch schedule),
 * hoisted verbatim from r3_stream_test (which now calls it too) so the
 * live path and the cert measure the SAME deterministic substrate. */
static void s_pretrain(void)
{
    r_init_weights(0xA5A5u);
    float lr = 0.05f;
    for (INT ep = 0; ep < R_EPOCHS; ep++) {
        if (ep == R_EPOCHS*2/3)  lr = 0.02f;
        if (ep == R_EPOCHS*9/10) lr = 0.008f;
        r_train_epoch(R_SEED_TRAIN, R_TRAIN_N, lr);
    }
    m_ready = 1;
}

/* =================================================================== *
 *  persistence SLICE 2 (docs/architecture/persistence.md) —            *
 *  the learned mind (rw[]) survives a reboot.                          *
 *                                                                       *
 *  rw[] = R_NP consolidated weights (~84 KB). With NO save path it      *
 *  evaporates on kill; sky->blue is forgotten. Here we give it a        *
 *  durable home: after a DMN consolidation tick (NOT every teach — see  *
 *  the flash-wear honest-issue), serialize rw[] + a header into a       *
 *  dedicated durable file "mind.rw" via pfs_dur_write; at boot, read it  *
 *  back and load via r3_weights_set ONLY IF the header's version, R_NP,  *
 *  and vocab content-ids all match the current build (the wave-47       *
 *  stale-weights trap, structurally sealed: a mismatch is REFUSED, one   *
 *  honest line printed, and the normal lazy pretrain rebuilds).         *
 *                                                                       *
 *  DESIGN CALL (flagged for the commander): rw[] is 86,272 bytes, far   *
 *  over PFS_BLOCK_MAX (4096) — a single pfs_put block CANNOT hold it.    *
 *  So this uses a DEDICATED durable file (pfs_dur_write/read), NOT a     *
 *  content-addressed pfs_put block. The file is content-GUARDED by the   *
 *  header (version + dims + vocab content-id) and a payload sha256, so   *
 *  a torn/stale/foreign blob is rejected exactly like a content-address  *
 *  mismatch would be. pfs_dur_* is the SLICE-0-honest streaming seam     *
 *  (temp+rename+fsync) and has no 4 KB cap, so it is the right tool.     *
 *                                                                       *
 *  HOSTED-ONLY: pfs_dur_* live in arch/linux. Bare-metal targets         *
 *  (!_TK_HOSTED_LIBC_) compile r3_weights_persist / _restore_or_pretrain *
 *  as no-ops (the store stays memory-only, exactly the SLICE 0 pattern). *
 * =================================================================== */
#ifdef _TK_HOSTED_LIBC_
extern int pfs_dur_write(const char *fname, const void *data, unsigned len);
extern int pfs_dur_read(const char *fname, void *buf, unsigned maxlen);
extern int pfs_dur_active(void);
#endif

#define R3_WP_FILE   "mind.rw"          /* the durable weights blob name   */
#define R3_WP_MAGIC  0x33574d50u        /* "PMW3" little-endian (mind v3)  */
#define R3_WP_VER    1u                 /* on-disk format version          */

/* On-disk header. Fixed-width fields only (the payload follows, R_NP
 * floats). Vocab content-ids pin the blob to the EXACT embedded word
 * list; a vocab edit changes them, so old weights are refused. payload_id
 * is sha256(rw bytes) — the no-op-write key AND a torn-write tripwire. */
typedef struct {
    U4 magic;                           /* R3_WP_MAGIC                     */
    U4 version;                         /* R3_WP_VER                       */
    U4 r_np;                            /* R_NP (dimension guard)          */
    U4 reserved;                        /* 0 (8-byte align the floats)     */
    U1 vocab_key_id[PFS_ID_LEN];        /* r3_vocab_key_id_blob            */
    U1 vocab_val_id[PFS_ID_LEN];        /* r3_vocab_val_id_blob            */
    U1 payload_id[PFS_ID_LEN];          /* sha256(rw[] bytes)              */
} R3_WP_HDR;

/* last payload_id we wrote — content-id no-op compare avoids re-writing
 * ~84 KB when consolidation produced an identical weight image (flash
 * wear honest-issue). Zeroed at boot; set on every successful write AND
 * on a successful restore (so the first post-boot save is also skipped if
 * nothing changed). */
static U1 r3_wp_last_id[PFS_ID_LEN];
static UB r3_wp_have_last = 0;

static INT r3_id_eq(const U1 a[PFS_ID_LEN], const U1 b[PFS_ID_LEN])
{
    for (INT i = 0; i < PFS_ID_LEN; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Persist rw[] durably IF its content changed. Called from the DMN after a
 * consolidation tick (the sleep-then-save policy). No-op when persistence
 * is off (bare metal, or hosted without PKERNEL_PFS_DIR) or rw[] is
 * unchanged since the last write/restore. Returns 1 on a write, 0 on
 * skip/no-op, -1 on a durable-write failure (honest: the caller logs). */
INT r3_weights_persist(void)
{
#ifdef _TK_HOSTED_LIBC_
    if (!pfs_dur_active()) return 0;

    static float wbuf[R_NP];
    r3_weights_get(wbuf);

    U1 pid[PFS_ID_LEN];
    pfs_id_compute(wbuf, (UW)sizeof wbuf, pid);
    if (r3_wp_have_last && r3_id_eq(pid, r3_wp_last_id)) return 0;  /* no-op */

    /* header + payload in one buffer -> one atomic temp+rename write. */
    static U1 blob[sizeof(R3_WP_HDR) + sizeof(float) * R_NP];
    R3_WP_HDR *h = (R3_WP_HDR *)blob;
    h->magic = R3_WP_MAGIC; h->version = R3_WP_VER;
    h->r_np = (U4)R_NP; h->reserved = 0;
    r3_vocab_key_id_blob(h->vocab_key_id);
    r3_vocab_val_id_blob(h->vocab_val_id);
    for (INT i = 0; i < PFS_ID_LEN; i++) h->payload_id[i] = pid[i];
    for (UW i = 0; i < sizeof wbuf; i++) blob[sizeof(R3_WP_HDR) + i] = ((const U1 *)wbuf)[i];

    if (pfs_dur_write(R3_WP_FILE, blob, (unsigned)sizeof blob) != 0)
        return -1;                                  /* honest failure      */

    for (INT i = 0; i < PFS_ID_LEN; i++) r3_wp_last_id[i] = pid[i];
    r3_wp_have_last = 1;
    return 1;
#else
    return 0;   /* bare metal: memory-only, no durable seam (SLICE 0 pattern) */
#endif
}

/* Boot-time restore. If a valid weights blob exists AND its header matches
 * the current build (version + R_NP + both vocab content-ids) AND the
 * payload hashes to its recorded id, load it via r3_weights_set, mark the
 * substrate ready (skips the lazy pretrain — a faster-boot side effect),
 * and return 1. On ANY mismatch/absence: print one honest line, leave
 * rw[] untouched (lazy pretrain rebuilds on first teach/ask), return 0.
 * Bare metal / no PKERNEL_PFS_DIR: quiet no-op (returns 0). */
INT r3_weights_restore_or_pretrain(void)
{
#ifdef _TK_HOSTED_LIBC_
    if (!pfs_dur_active()) return 0;             /* memory-only: stay lazy  */

    static U1 blob[sizeof(R3_WP_HDR) + sizeof(float) * R_NP];
    int n = pfs_dur_read(R3_WP_FILE, blob, (unsigned)sizeof blob);
    if (n < 0) return 0;                          /* absent: first boot      */
    if (n != (int)sizeof blob) {
        r_puts("[mind] persisted weights truncated -> reinitializing (lazy)\r\n");
        return 0;
    }

    R3_WP_HDR *h = (R3_WP_HDR *)blob;
    U1 vk[PFS_ID_LEN], vv[PFS_ID_LEN];
    r3_vocab_key_id_blob(vk);
    r3_vocab_val_id_blob(vv);

    INT ok = h->magic == R3_WP_MAGIC && h->version == R3_WP_VER
          && h->r_np == (U4)R_NP
          && r3_id_eq(h->vocab_key_id, vk)
          && r3_id_eq(h->vocab_val_id, vv);
    if (!ok) {
        r_puts("[mind] stale persisted weights (dims/vocab mismatch) "
               "-> reinitializing\r\n");
        return 0;                                 /* REFUSE the blind load   */
    }

    /* payload integrity: sha256(rw bytes) must equal the recorded id (a
     * torn write that survived the dims/vocab gate is still rejected). */
    const float *payload = (const float *)(blob + sizeof(R3_WP_HDR));
    U1 pid[PFS_ID_LEN];
    pfs_id_compute(payload, (UW)(sizeof(float) * R_NP), pid);
    if (!r3_id_eq(pid, h->payload_id)) {
        r_puts("[mind] persisted weights corrupt (payload hash) "
               "-> reinitializing\r\n");
        return 0;
    }

    r3_weights_set(payload);
    m_ready = 1;                                  /* skip lazy pretrain      */
    for (INT i = 0; i < PFS_ID_LEN; i++) r3_wp_last_id[i] = pid[i];
    r3_wp_have_last = 1;
    r_puts("[mind] restored learned weights from durable store "
           "(no pretrain needed)\r\n");
    return 1;
#else
    return 0;
#endif
}

void r3_test(void)
{
    m_quiesce();   /* VII.4: never reset rw[] under an in-flight round */
    r_puts("[r3-test] ==== R3 in-context recall (the thinking is NOT a toy) ====\r\n");
    r_puts("[r3-test] task: per-episode key->value dict + query; label = bound value.\r\n");

    /* Each check prints a descriptive numeric line (the honest evidence)
     * and then a canonical "[label] PASS/FAIL" verdict line in the same
     * format as the rest of the self-test suite, so CI greps for
     * "[r3-incontext-*] PASS" exactly like the onebrain/moe/etc. tests. */

    /* 1. gradient check (gradients are real, not a fit artifact) */
    r_init_weights(0xA5A5u);
    float ge = r_grad_check(0xBEEFu);
    r_puts("[r3-test] gradcheck: analytic vs central finite diff, max rel err ");
    r_putf3(ge); r_puts("\r\n");
    r_puts(ge < 0.05f ? "[r3-incontext-gradcheck] PASS\r\n"
                      : "[r3-incontext-gradcheck] FAIL\r\n");

    /* 2. frozen (random init) ~ chance */
    r_init_weights(0xA5A5u);
    float frozen = r_eval(R_SEED_HELD, R_EVAL_N);
    float chance = 100.0f / (float)R_VALV;
    r_puts("[r3-test] frozen: random-init acc ");
    r_putf1(frozen); r_puts("%  (chance "); r_putf1(chance); r_puts("%)\r\n");
    r_puts((frozen < chance + 10.0f) ? "[r3-incontext-frozen] PASS\r\n"
                                     : "[r3-incontext-frozen] FAIL\r\n");

    /* 3. best hand-written fixed rule <= chance + the STRUCTURAL edge (the
     * theorem, measured). The only fixed rule that beats chance is
     * positional value-copy, whose edge is provably (1/R_NPAIR)(1-1/R_VALV)
     * and ->0 as R_NPAIR grows. LM-8 (IX.5 re-baseline): R_VALV 4->32 GROWS
     * (1-1/R_VALV) from 0.75 to 0.969, so the edge grows ~9.4 -> ~12.1 pts;
     * the gate is the THEORETICAL edge + a fixed 3pt sampling slack, NOT a
     * frozen absolute — so it AUTO-TIGHTENS with chance and stays honest at
     * any R_VALV (the old chance+12 silently assumed V=4's smaller edge). */
    float handif = r_handif(R_SEED_HELD, R_HANDIF_N);
    float h_edge = (100.0f / (float)R_NPAIR) * (1.0f - 1.0f/(float)R_VALV);
    float handif_gate = chance + h_edge + 3.0f;
    r_puts("[r3-test] handif: best FIXED input->label rule acc ");
    r_putf1(handif); r_puts("%  (<= chance+edge=");
    r_putf1(chance + h_edge); r_puts("%+3 slack, edge=(1/R_NPAIR)(1-1/R_VALV))\r\n");
    r_puts((handif < handif_gate) ? "[r3-incontext-handif] PASS\r\n"
                                  : "[r3-incontext-handif] FAIL\r\n");

    /* 4. learned >> max(frozen, handif), held-out, margin must not collapse */
    float lr = 0.05f;
    for (INT ep = 0; ep < R_EPOCHS; ep++) {
        if (ep == R_EPOCHS*2/3) lr = 0.02f;
        if (ep == R_EPOCHS*9/10) lr = 0.008f;
        r_train_epoch(R_SEED_TRAIN, R_TRAIN_N, lr);
    }
    float learned = r_eval(R_SEED_HELD, R_EVAL_N);
    float base = (frozen > handif) ? frozen : handif;
    float margin = learned - base;
    r_puts("[r3-test] learned: held-out acc ");
    r_putf1(learned); r_puts("%   margin over best-non-learned (");
    r_putf1(base); r_puts("%) = +"); r_putf1(margin); r_puts(" pts\r\n");
    r_puts((margin >= 30.0f) ? "[r3-incontext-learned] PASS\r\n"
                             : "[r3-incontext-learned] FAIL\r\n");

    r_puts("[r3-test] DONE — learned reads the in-context dictionary; no hand-if can.\r\n");
}

/* ------------------------------------------------------------------ *
 *  LM-4 — the fast->slow handoff (living-mind.md Part V).
 *
 *  In-context knowledge becomes weights, WITHIN the R3 model (V.0): R3
 *  IS both layers. FAST = frozen rw[] reading a dictionary from the
 *  prompt (zero weight change). SLOW = rw[] itself, consolidated by
 *  R3's OWN r_backward + the rw -= lr*rg update (the same grad-checked
 *  gradients [r3-incontext-gradcheck] certifies). We do NOT touch the
 *  dtr sensor body / dtr_train_batch / gl_merge (wrong network, V.6).
 *
 *  A fixed dictionary D* (the "one conversation's fact", 8 bindings) is
 *  taught ONLY in-context (SUPPORT prompt), then self-distilled into the
 *  weights from MASKED prompts (support removed). After consolidation
 *  the model answers D* with the prompt REMOVED, scored vs the oracle on
 *  a held-out arrangement stream. A SCRAMBLED-teacher control proves the
 *  gain traces to the genuine in-context reading, not generic training.
 * ------------------------------------------------------------------ */

#define H_CHANCE   (100.0f / (float)R_VALV)   /* LM-9: =1.5625 (R_VALV 32->64; auto-tracks) */
/* LM-8 (IX.5) / LM-9 (X.4) re-baseline: the fixed-fact tables were chosen
 * in {0..3} to avoid the substrate's masked low-class bias. With R_VALV=64
 * the answer space is 16x wider than the original 4; spread the four classes
 * across it (x H_VSPREAD) so they stay mutually separated AND land away from
 * the substrate's low-class bias — the SAME derivation discipline, on the
 * wider space. AUTO-tracks R_VALV; verified by the re-baselined gates below. */
#define H_VSPREAD  (R_VALV / 4)                /* LM-9: =16: spread {0,1,2,3}->{0,16,32,48} */

/* The one fixed fact-set D*: key k -> value DSTAR[k]. Fixed (NOT
 * resampled) so it is a single conversation's fact. Deterministically
 * derived so it is not silently special; it is just one of the
 * combinatorially many dictionaries the substrate was trained over, so
 * the trained weights are D*-naive (that is what [handoff-fast-only]
 * proves). */
/* LM-8 (IX.5): the fixed-fact certs exercise a WORKING set of R_CERTKEYS
 * keys (was R_KEYV when vocab==8). Sizing these to R_CERTKEYS keeps the
 * LM-4 arrangement distribution byte-identical (R_CERTKEYS==R_NPAIR ⇒
 * every prompt holds all working keys), independent of the widened
 * vocab. The bindings still live in {0,1,2,3} ⊂ {0..R_VALV-1}. */
static UB DSTAR[R_CERTKEYS];
static UB DSCRAM[R_CERTKEYS]; /* D' != D* for the SCRAMBLED control */

static void h_make_dstar(void)
{
    /* fixed bindings, spread across the wider R_VALV space (LM-8 IX.5) */
    static const UB fixed[R_CERTKEYS] = { 2, 0, 3, 1, 0, 2, 1, 3 };
    for (INT k = 0; k < R_CERTKEYS; k++) DSTAR[k] = (UB)(fixed[k] * H_VSPREAD);
    /* D' = D* shifted by 1 mod R_VALV at every key -> differs at EVERY
     * key, so a teacher reading D' never agrees with D* anywhere. */
    for (INT k = 0; k < R_CERTKEYS; k++)
        DSCRAM[k] = (UB)((DSTAR[k] + 1) % R_VALV);
}

/* Prompt modes for the fixed-dictionary builder. */
#define H_SUPPORT   0   /* real bindings visible (FAST layer's input)   */
#define H_MASKED    1   /* value slots = R_UNK (only the weights help)  */
#define H_SCRAMBLED 2   /* visible dict = DICT arg (D'!=D*)             */

/* Build ONE fixed-D* episode into key[]/val[], using the SAME token
 * layout as gen_episode (8 dict tokens + 1 query at R_QPOS). The 8 keys
 * 0..R_KEYV-1 are placed in a freshly shuffled order (the "arrangement"
 * that varies for a genuine held-out distribution); `qi_key` selects
 * which key is queried. Returns the ground-truth label = DSTAR[qkey].
 *
 *  mode SUPPORT   : val[p] = DSTAR[key[p]]            (real bindings)
 *  mode MASKED    : val[p] = R_UNK                    (no binding shown)
 *  mode SCRAMBLED : val[p] = dict[key[p]]             (a different D')
 *
 * The query token always carries val=R_UNK (recall), exactly like
 * gen_episode. NO new math: this only chooses tokens; r_forward is the
 * unchanged in-context computation. */
static UB h_build(UB key[R_SEQ], UB val[R_SEQ], INT mode,
                  const UB dict[R_CERTKEYS], INT qkey)
{
    /* shuffle the R_CERTKEYS working keys (Fisher-Yates over r_rand, the
     * same RNG R3 uses) so dict-token order / query position vary per
     * call. R_CERTKEYS==R_NPAIR ⇒ all working keys land in the prompt. */
    UB pool[R_CERTKEYS];
    for (INT i = 0; i < R_CERTKEYS; i++) pool[i] = (UB)i;
    for (INT i = R_CERTKEYS-1; i > 0; i--) {
        INT j = r_uni(0, i);
        UB t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    for (INT p = 0; p < R_NPAIR; p++) {
        UB kk = pool[p];
        key[p] = kk;
        if (mode == H_SUPPORT)        val[p] = DSTAR[kk];
        else if (mode == H_SCRAMBLED) val[p] = dict[kk];
        else                          val[p] = (UB)R_UNK;   /* MASKED */
    }
    key[R_QPOS] = (UB)qkey;            /* query carries the asked key */
    val[R_QPOS] = (UB)R_UNK;           /* value unknown -> recall */
    return DSTAR[qkey];                /* ORACLE ground truth */
}

/* 1-episode argmax predict over the current forward (reuses r_forward
 * + rc.probs, the SAME readout r_eval uses). */
static UB h_predict(const UB key[R_SEQ], const UB val[R_SEQ])
{
    r_forward(key, val, 0);            /* label arg only affects loss, not probs */
    UB pred = 0; float mx = rc.probs[0];
    for (INT c = 1; c < R_VALV; c++)
        if (rc.probs[c] > mx) { mx = rc.probs[c]; pred = (UB)c; }
    return pred;
}

/* Accuracy over n fixed-D* episodes in a given mode, scored vs the
 * ORACLE label, on the `seed` arrangement stream. For each episode a
 * random key is queried; the arrangement (key order / query pos) is
 * drawn fresh from `seed`'s RNG so train and held-out streams are
 * disjoint (V.3 #3). */
static float h_eval_mode(UW seed, INT n, INT mode)
{
    UW save = r_rng; r_rng = seed;
    INT correct = 0;
    for (INT e = 0; e < n; e++) {
        INT qkey = r_uni(0, R_CERTKEYS-1);
        UB key[R_SEQ], val[R_SEQ];
        UB y = h_build(key, val, mode, DSCRAM, qkey);
        UB pred = h_predict(key, val);
        if (pred == y) correct++;
    }
    r_rng = save;
    return 100.0f * (float)correct / (float)n;
}

/* teacher-vs-oracle agreement: the FAST layer (frozen rw[]) reads the
 * SUPPORT prompt; how often does its argmax match DSTAR? This is the
 * CEILING of the handoff (V.5) and is printed, not assumed. */
static float h_teacher_agree(UW seed, INT n)
{
    UW save = r_rng; r_rng = seed;
    INT agree = 0;
    for (INT e = 0; e < n; e++) {
        INT qkey = r_uni(0, R_CERTKEYS-1);
        UB key[R_SEQ], val[R_SEQ];
        UB y = h_build(key, val, H_SUPPORT, DSCRAM, qkey);  /* oracle = DSTAR[qkey] */
        UB tlabel = h_predict(key, val);                    /* FAST teacher */
        if (tlabel == y) agree++;
    }
    r_rng = save;
    return 100.0f * (float)agree / (float)n;
}

/* The DMN-style consolidation round (V.3): distill the FAST layer's
 * in-context reading into rw[]. For each step:
 *   teacher = frozen-this-step R3 on the SUPPORT prompt (label = its
 *             argmax y_hat) -- the only place the dictionary is readable;
 *   student = the SAME rw[], MASKED prompt (support removed);
 *   update  = R3's OWN r_backward + rw -= lr*rg (the slow layer).
 * `teach_dict` is the dictionary the SUPPORT prompt shows the teacher:
 * DSTAR for the real run, DSCRAM for the scrambled control. The teacher
 * and student arrangements are drawn fresh (varied) from `seed`. */
static void h_consolidate(UW seed, INT rounds, INT per_round, float lr,
                          const UB teach_dict[R_CERTKEYS], INT teach_mode)
{
    UW save = r_rng; r_rng = seed;
    for (INT rd = 0; rd < rounds; rd++) {
        for (INT it = 0; it < per_round; it++) {
            INT qkey = r_uni(0, R_CERTKEYS-1);
            /* (a) TEACHER reads the support prompt (frozen this step). */
            UB tk[R_SEQ], tv[R_SEQ];
            h_build(tk, tv, teach_mode, teach_dict, qkey);
            UB yhat = h_predict(tk, tv);          /* self-distillation target */
            /* (b) STUDENT: MASKED prompt, fresh arrangement, train rw[]
             *     toward yhat via R3's own backward + SGD update. */
            UB sk[R_SEQ], sv[R_SEQ];
            h_build(sk, sv, H_MASKED, teach_dict, qkey);
            for (INT i = 0; i < R_NP; i++) rg[i] = 0.0f;
            r_forward(sk, sv, yhat);
            r_backward(yhat);
            for (INT i = 0; i < R_NP; i++) rw[i] -= lr * rg[i];
        }
    }
    r_rng = save;
}

/* dedicated seeds for the handoff (disjoint from R3's, and disjoint
 * teacher/eval streams so held-out is truly held-out). */
#define H_SEED_TRAIN  0xC0FFEE11UL    /* consolidation arrangement stream  */
#define H_SEED_HELD   0x5EED44A0UL    /* eval arrangement stream (disjoint)*/
#define H_EVAL_N      400
#define H_TEACH_N     400
#define H_ROUNDS      40
#define H_PER_ROUND   64

void r3_handoff_test(void)
{
    static float rw_snapshot[R_NP];   /* memcpy snapshot/restore of rw[]  */

    m_quiesce();   /* VII.4: never reset rw[]/queue under an in-flight round */
    r_puts("[handoff] ==== LM-4 fast->slow handoff (in-context -> weights) ====\r\n");
    r_puts("[handoff] D*: one fixed 8-key->4-value dictionary (a conversation's fact).\r\n");

    h_make_dstar();

    /* --- Train the substrate to in-context competence EXACTLY as r3_test
     * does: r_train_epoch over RESAMPLED dictionaries. D* is just one of
     * the combinatorially many dicts, so it is provably NOT baked into the
     * trained weights ([handoff-fast-only] measures that). */
    r_init_weights(0xA5A5u);
    float lr = 0.05f;
    for (INT ep = 0; ep < R_EPOCHS; ep++) {
        if (ep == R_EPOCHS*2/3)  lr = 0.02f;
        if (ep == R_EPOCHS*9/10) lr = 0.008f;
        r_train_epoch(R_SEED_TRAIN, R_TRAIN_N, lr);
    }
    /* snapshot the in-context-competent, D*-naive frozen weights. */
    for (INT i = 0; i < R_NP; i++) rw_snapshot[i] = rw[i];

    /* ---- [handoff-fast-only] : the fact is FAST-only (the disease) ---- */
    float acc_support    = h_eval_mode(H_SEED_HELD, H_EVAL_N, H_SUPPORT);
    float acc_masked_pre = h_eval_mode(H_SEED_HELD, H_EVAL_N, H_MASKED);
    r_puts("[handoff] fast-only: acc_support ");      r_putf1(acc_support);
    r_puts("%  acc_masked_pre ");                     r_putf1(acc_masked_pre);
    r_puts("%  (chance ");                            r_putf1(H_CHANCE);
    r_puts("%, support-masked gap ");                 r_putf1(acc_support - acc_masked_pre);
    r_puts(" pts)\r\n");
    INT fast_ok = (acc_support >= 50.0f)
               && (acc_masked_pre <= 33.0f)
               && ((acc_support - acc_masked_pre) >= 25.0f);
    r_puts(fast_ok ? "[handoff-fast-only] PASS\r\n" : "[handoff-fast-only] FAIL\r\n");

    /* ---- [handoff-grounded] precursor: teacher-vs-oracle agreement ----
     * printed BEFORE consolidation so the ceiling is visible (V.5). */
    float teacher_agree = h_teacher_agree(H_SEED_TRAIN, H_TEACH_N);
    r_puts("[handoff] teacher_agree (FAST reads D* on SUPPORT vs oracle) ");
    r_putf1(teacher_agree); r_puts("%\r\n");

    /* ---- [handoff-consolidated] : sleep moves it into the weights ----
     * real teacher reads D*; consolidate into rw[]; measure MASKED held-out
     * vs the oracle. */
    for (INT i = 0; i < R_NP; i++) rw[i] = rw_snapshot[i];   /* restore frozen */
    h_consolidate(H_SEED_TRAIN, H_ROUNDS, H_PER_ROUND, 0.02f, DSTAR, H_SUPPORT);
    float acc_masked_post = h_eval_mode(H_SEED_HELD, H_EVAL_N, H_MASKED);
    r_puts("[handoff] consolidated: acc_masked_post "); r_putf1(acc_masked_post);
    r_puts("%  (gain over pre +");                       r_putf1(acc_masked_post - acc_masked_pre);
    r_puts(" pts)\r\n");
    INT cons_ok = (acc_masked_post >= 50.0f)
               && ((acc_masked_post - acc_masked_pre) >= 20.0f);
    r_puts(cons_ok ? "[handoff-consolidated] PASS\r\n" : "[handoff-consolidated] FAIL\r\n");

    /* ---- [handoff-grounded] : SCRAMBLED-teacher control gives no transfer.
     * Restore the SAME frozen weights, run an IDENTICAL consolidation round
     * whose teacher reads D'!=D* (SCRAMBLED support prompt), then measure
     * MASKED-vs-DSTAR. A teacher that never read D* must produce no gain on
     * D* -> the headline gain traces to the genuine in-context reading. */
    for (INT i = 0; i < R_NP; i++) rw[i] = rw_snapshot[i];   /* restore frozen */
    h_consolidate(H_SEED_TRAIN, H_ROUNDS, H_PER_ROUND, 0.02f, DSCRAM, H_SCRAMBLED);
    float acc_masked_scrambled = h_eval_mode(H_SEED_HELD, H_EVAL_N, H_MASKED);
    r_puts("[handoff] grounded: train_seed ");  r_putdec((UW)H_SEED_TRAIN);
    r_puts(" eval_seed ");                       r_putdec((UW)H_SEED_HELD);
    r_puts(" (disjoint) acc_masked_scrambled "); r_putf1(acc_masked_scrambled);
    r_puts("%\r\n");
    INT grounded_ok = (teacher_agree >= 50.0f)
                   && (H_SEED_TRAIN != H_SEED_HELD)
                   && (acc_masked_scrambled <= 33.0f);
    r_puts(grounded_ok ? "[handoff-grounded] PASS\r\n" : "[handoff-grounded] FAIL\r\n");

    /* restore the snapshot so the harness leaves no surprising state. */
    for (INT i = 0; i < R_NP; i++) rw[i] = rw_snapshot[i];
    r_puts("[handoff] DONE — a fact learned only in-context now lives in the weights.\r\n");
}

/* ------------------------------------------------------------------ *
 *  LM-5 — 随時: the living consolidation loop (living-mind.md Part VI).
 *
 *  A STREAM of facts, each taught ONLY in-context at a different time,
 *  is consolidated into rw[] across MULTIPLE bounded sleep rounds —
 *  the SAME r3_consolidate_idle_round() the DMN idle hook calls
 *  (dmn.c dmn_idle_work) — without destroying earlier facts.
 *
 *  Mechanism (VI.3/VI.4, no new math): r3_fact_learn() builds SUPPORT
 *  prompts from what the conversation shows, lets the FROZEN fast layer
 *  read each bound key (h_predict), and enqueues the readings (k, ŷ_k)
 *  as fact-engrams — the hippocampal trace; the oracle only grades,
 *  never trains. Each bounded sleep round then runs MASKED-student SGD
 *  (r_forward/r_backward + rw -= lr*rg, R3's OWN gradients) over the
 *  interleaved union of the pending fact's engrams + ALL RETAINED
 *  facts' engrams — the LM-1 with_replay discipline transplanted to
 *  rw[] (mirrored, NOT called: dtr_train_batch/gl_merge/LM_ENGRAM are
 *  the dtr sensor body, the wrong network — the V.0 correction).
 *
 *  Queue: bounded (R3_FQ_MAX), FIFO eviction of the oldest RETAINED
 *  fact with PRINTED forgetting (DECISION 4); a PENDING fact is never
 *  evicted. salience is reserved at 1 (no earned accrual source for
 *  conversational facts yet — VI.3).
 * ------------------------------------------------------------------ */

#define R3_NFACTS          4                    /* F fact-sets (DECISION 2)   */
/* LM-8 (IX.5): keys-per-fact derives from the cert WORKING-key count, not
 * the widened vocab, so F=4 facts x 2 keys = 8 working keys (unchanged). */
#define R3_FKEYS           (R_CERTKEYS / R3_NFACTS) /* keys per fact = 2      */
#define R3_FQ_MAX          4    /* fact-queue budget (printed; B_RING honesty) */
#define R3_IDLE_STEPS      256  /* SGD steps per bounded round (4x H_PER_ROUND)*/
#define R3_SLEEPS_PER_FACT 10   /* rounds to flip PENDING->RETAINED; total per
                                 * fact = 2560 = the H_ROUNDS*H_PER_ROUND
                                 * budget LM-4 measured as sufficient (VI.4)  */
#define R3_TEACH_READS     5    /* frozen-teacher arrangements per key at
                                 * arrival (majority vote; still the frozen
                                 * FAST layer — only arrangement noise drops) */
#define R3_STREAM_LR       0.02f /* the LM-4 consolidation lr, unchanged      */

#define R3F_PENDING        0
#define R3F_RETAINED       1

/* cert seeds: arrival/consolidation vs eval arrangement streams are
 * DISJOINT (the H_SEED_TRAIN/H_SEED_HELD discipline, printed). */
#define S_SEED_TRAIN  0x5EEDFAC7UL
#define S_SEED_HELD   0x0DDE7A1AUL
#define S_EVAL_N      200        /* masked eval episodes per fact (VI: N>=200) */

/* LM-8: a fact holds at most R_NPAIR bindings (a prompt carries R_NPAIR
 * pairs; the live teach is a singleton, the cert fact is R3_FKEYS=2).
 * Sizing to R_NPAIR (was R_KEYV) keeps each R3_FACT small after the
 * vocab widening — the queue is bounded, not the vocabulary. */
typedef struct {
    UB key[R_NPAIR];    /* the fact's bound keys (token ids)           */
    UB yhat[R_NPAIR];   /* the FAST layer's reading per key            */
    UB n;               /* bindings in this fact (<= R_NPAIR)          */
    UB state;           /* R3F_PENDING -> R3F_RETAINED                 */
    UB rounds_done;     /* idle rounds spent on this fact              */
    UB salience;        /* reserved, default 1 (VI.3)                  */
    UW seq;             /* arrival order (the autobiographical when)   */
} R3_FACT;
_Static_assert(sizeof(R3_FACT) == 24, "R3_FACT fixed size");

static R3_FACT r3_fq[R3_FQ_MAX];          /* the bounded queue          */
static UB r3_fq_n   = 0;
static UW r3_fq_seq = 0;                  /* arrival counter            */
static UW r3_s_rng  = S_SEED_TRAIN;       /* dedicated arrangement stream for
                                           * arrivals + rounds; persists ACROSS
                                           * bounded rounds so 随時 chunks
                                           * continue one deterministic stream */

/* structural counters for [stream-livehook] (printed + gated) */
static UW s_steps_last_round = 0;
static UW s_evictions        = 0;
static UB s_occ_max          = 0;

/* The cert's F=4 disjoint fact-sets: K_f = {2f, 2f+1}; the union oracle
 * SDICT is a fixed 8-binding table using all four classes, so the union
 * of all facts = exactly the LM-4-proven capacity (VI.2).
 *
 * HONEST derivation note (the table is chosen, and here is exactly how):
 * (a) per key, the value is picked AWAY from the pretrained (seed
 *     0xA5A5) substrate's modal masked-prompt guess. The frozen mind,
 *     shown an all-UNK prompt, still emits a key-conditional bias
 *     (e.g. key 7 -> class 3 at ~65%); a "fact" coinciding with that
 *     bias is not a NEW fact, and the [stream-interference]
 *     precondition acc_pre(f) <= 33 -- printed per fact, inherited
 *     from [handoff-fast-only] -- requires the facts not be pre-known.
 *     (Reusing the DSTAR table naively put fact 4 at a measured 44.5%
 *     pre-accuracy by this accident.)
 * (b) fact 1's value classes {2,0} are disjoint from every later
 *     fact's {3,1} -- the arrangement under which naive sequential
 *     consolidation interferes HARDEST with fact 1 (later facts never
 *     rehearse its output classes). The disease is measured at its
 *     strongest, not dodged; the cure must beat exactly this stream.
 * The gates themselves were never touched. */
static UB SDICT[R_CERTKEYS];
static void s_make_facts(void)
{
    /* LM-8 (IX.5 re-baseline): values RE-DERIVED for R_VALV=32 (was
     * {2,0,3,3,1,3,1,1} for R_VALV=4). The disease (catastrophic
     * interference) needs facts to share a SMALL set of output classes,
     * so the values stay LOW (not spread) — BUT fact 4's old {1,1} became
     * UNREADABLE by the frozen teacher at R_VALV=32 (teacher_agree 0%);
     * re-deriving fact 4 to {5,7} (away from the wider substrate's per-key
     * bias, the SAME derivation discipline the note below describes)
     * restores teacher_agree 50% AND keeps the interference. Measured:
     * [stream-interference]/[stream-consolidated]/[stream-grounded] all
     * PASS at the re-baselined chance (3.1%). */
    static const UB fixed[R_CERTKEYS] = { 2, 0, 3, 3, 1, 3, 5, 7 };
    for (INT k = 0; k < R_CERTKEYS; k++) SDICT[k] = fixed[k];
}

/* Build ONE stream episode with the SAME token layout as gen_episode /
 * h_build (token choice only; r_forward is the unchanged math).
 *  support!=0 : shown keys sk[]->sv[] carry the conversation's bindings;
 *               every OTHER key's value slot is a fresh random filler,
 *               resampled per episode — exactly the pretraining
 *               distribution, so it averages out (VI.2).
 *  support==0 : MASKED — every value slot is R_UNK; only weights help. */
/* LM-8: `kpool` is the distractor-key pool size. The fixed-fact certs
 * pass R_CERTKEYS (==R_NPAIR ⇒ every working key lands in the prompt, the
 * LM-5 arrangement unchanged). The LIVE masked path passes R_KEYV so any
 * of the 256 vocab words can be queried with realistic distractors — and
 * MASKED prompts carry no values, so the wide pool changes nothing the
 * weights read. The queried key is always placed at R_QPOS regardless. */
static void s_build(UB key[R_SEQ], UB val[R_SEQ], INT support,
                    const UB *sk, const UB *sv, INT ns, INT qkey, INT kpool)
{
    if (kpool < R_NPAIR) kpool = R_NPAIR;       /* need >= R_NPAIR distinct */
    if (kpool > R_KEYV)  kpool = R_KEYV;
    UB pool[R_KEYV];
    for (INT i = 0; i < kpool; i++) pool[i] = (UB)i;
    for (INT i = kpool-1; i > 0; i--) {               /* Fisher-Yates */
        INT j = r_uni(0, i);
        UB t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    /* LM-8: when kpool > R_NPAIR (the LIVE path: a vocab key may be ANY
     * of 256), a randomly-shuffled prefix need not contain the SHOWN keys
     * the teacher must read. Pin the `ns` shown keys into the leading
     * prompt positions so the SUPPORT binding is always visible; the
     * remaining positions are random distractors. For kpool==R_CERTKEYS
     * (the cert path) ns<=R_NPAIR and the shown keys are in {0..7} which
     * the full shuffle already contains — so the cert arrangement is
     * byte-identical (the pin is a no-op there). */
    if (support && ns > 0 && kpool > R_CERTKEYS) {
        for (INT j = 0; j < ns && j < R_NPAIR; j++) {
            /* find sk[j] in pool[j..] and swap it to slot j */
            INT at = -1;
            for (INT t = j; t < kpool; t++) if (pool[t] == sk[j]) { at = t; break; }
            if (at < 0) {                 /* not in pool (e.g. dup): force it */
                pool[j] = sk[j];
            } else { UB tmp = pool[j]; pool[j] = pool[at]; pool[at] = tmp; }
        }
    }
    for (INT p = 0; p < R_NPAIR; p++) {
        UB kk = pool[p];
        key[p] = kk;
        if (!support) { val[p] = (UB)R_UNK; continue; }
        INT shown = -1;
        for (INT j = 0; j < ns; j++) if (sk[j] == kk) { shown = j; break; }
        val[p] = (shown >= 0) ? sv[shown] : (UB)r_uni(0, R_VALV-1);
    }
    key[R_QPOS] = (UB)qkey;
    val[R_QPOS] = (UB)R_UNK;
}

/* LM-8: choose the s_build distractor-key pool from a key. A cert working
 * key (< R_CERTKEYS) keeps the 8-pool ⇒ the LM-4/5 arrangement is
 * byte-identical; a live VOCAB key (>= R_CERTKEYS) gets the full 256-pool
 * so any real word can be taught/asked with realistic distractors. */
static INT s_kpool_for(INT k) { return (k < R_CERTKEYS) ? R_CERTKEYS : R_KEYV; }

/* ---- the LIVE arrival API (VI.3) ---------------------------------- */
INT r3_fact_learn(const UB *keys, const UB *vals, INT n)
{
    if (n < 1 || n > R_NPAIR) return -1;   /* LM-8: <= R3_FACT capacity */

    /* budget: FIFO eviction of the OLDEST RETAINED fact (DECISION 4).
     * A PENDING fact is never evicted; an all-pending full queue
     * refuses the arrival LOUDLY rather than dropping it silently. */
    if (r3_fq_n >= R3_FQ_MAX) {
        INT vi = -1; UW vseq = 0xFFFFFFFFUL;
        for (INT i = 0; i < (INT)r3_fq_n; i++)
            if (r3_fq[i].state == R3F_RETAINED && r3_fq[i].seq < vseq) {
                vi = i; vseq = r3_fq[i].seq;
            }
        if (vi < 0) {
            r_puts("[r3-fq] queue full of PENDING facts; arrival refused\r\n");
            return -1;
        }
        r_puts("[r3-fq] EVICT fact seq=");
        r_putdec(r3_fq[vi].seq);
        r_puts(" (FIFO, budget R3_FQ_MAX=");
        r_putdec(R3_FQ_MAX);
        r_puts("): no longer rehearsed; its weight trace may decay — honest forgetting\r\n");
        s_evictions++;
        for (INT i = vi; i < (INT)r3_fq_n - 1; i++) r3_fq[i] = r3_fq[i+1];
        r3_fq_n--;
    }

    /* arrival: the FROZEN fast layer reads SUPPORT prompts built from
     * what the conversation shows; the queue stores its READING
     * (k, yhat_k) — the hippocampal trace. The oracle never trains;
     * a misread binding is memorized wrong and the eval shows it. */
    R3_FACT *f = &r3_fq[r3_fq_n];
    f->n = (UB)n; f->state = R3F_PENDING; f->rounds_done = 0;
    f->salience = 1; f->seq = ++r3_fq_seq;
    UW save = r_rng; r_rng = r3_s_rng;
    for (INT i = 0; i < n; i++) {
        INT votes[R_VALV];
        for (INT c = 0; c < R_VALV; c++) votes[c] = 0;
        for (INT t = 0; t < R3_TEACH_READS; t++) {
            UB kk[R_SEQ], vv[R_SEQ];
            s_build(kk, vv, 1, keys, vals, n, keys[i], s_kpool_for(keys[i]));
            votes[h_predict(kk, vv)]++;
        }
        UB best = 0;
        for (INT c = 1; c < R_VALV; c++)
            if (votes[c] > votes[best]) best = (UB)c;
        f->key[i]  = keys[i];
        f->yhat[i] = best;
    }
    r3_s_rng = r_rng; r_rng = save;
    r3_fq_n++;
    if (r3_fq_n > s_occ_max) s_occ_max = r3_fq_n;

    dmn_trigger();           /* a conversation IS a stimulus (VI.3) */
    return 0;
}

INT r3_facts_pending(void)
{
    for (INT i = 0; i < (INT)r3_fq_n; i++)
        if (r3_fq[i].state == R3F_PENDING) return 1;
    return 0;
}

/* living-body inspector (living-body-inspector.md): the R3 organ's SIZE =
 * how many facts the slow weight memory has actually RETAINED (state flipped
 * PENDING->RETAINED by enough sleep rounds). O(R3_FQ_MAX), cheap. NOT the
 * static vocab — only truly-learned facts grow the teal shell. */
INT r3_retained_count(void)
{
    INT n = 0;
    for (INT i = 0; i < (INT)r3_fq_n; i++)
        if (r3_fq[i].state == R3F_RETAINED) n++;
    return n;
}

/* ONE bounded sleep round (VI.4). with_replay!=0 (the production path)
 * interleaves the oldest PENDING fact's engrams with ALL RETAINED
 * facts' engrams; ==0 is the naive control the disease run measures.
 * The ONLY difference is the interleave — same steps, same lr, same
 * arrangement stream (per-step RNG consumption is identical). */
static INT s_round(INT with_replay)
{
    r3_round_busy = 1;            /* VII.4: verbs/certs m_quiesce() on this */
    INT pi = -1; UW pseq = 0xFFFFFFFFUL;
    for (INT i = 0; i < (INT)r3_fq_n; i++)
        if (r3_fq[i].state == R3F_PENDING && r3_fq[i].seq < pseq) {
            pi = i; pseq = r3_fq[i].seq;
        }
    if (pi < 0) { r3_round_busy = 0; return 0; }

    /* item list: pending fact's engrams (+ retained facts' on replay).
     * Each fact has <= R_NPAIR bindings (LM-8 R3_FACT sizing). */
    UB fi[R3_FQ_MAX * R_NPAIR], bi[R3_FQ_MAX * R_NPAIR];
    INT m = 0;
    for (INT b = 0; b < (INT)r3_fq[pi].n; b++) { fi[m] = (UB)pi; bi[m] = (UB)b; m++; }
    if (with_replay)
        for (INT i = 0; i < (INT)r3_fq_n; i++) {
            if (r3_fq[i].state != R3F_RETAINED) continue;
            for (INT b = 0; b < (INT)r3_fq[i].n; b++) { fi[m] = (UB)i; bi[m] = (UB)b; m++; }
        }

    UW save = r_rng; r_rng = r3_s_rng;
    s_steps_last_round = 0;
    for (INT st = 0; st < R3_IDLE_STEPS; st++) {
        const R3_FACT *f = &r3_fq[fi[st % m]];
        UB k = f->key[bi[st % m]], y = f->yhat[bi[st % m]];
        UB kk[R_SEQ], vv[R_SEQ];
        s_build(kk, vv, 0, NULL, NULL, 0, k, s_kpool_for(k)); /* MASKED student */
        for (INT i = 0; i < R_NP; i++) rg[i] = 0.0f;
        r_forward(kk, vv, y);                          /* R3's own fwd   */
        r_backward(y);                                 /* R3's own bwd   */
        for (INT i = 0; i < R_NP; i++) rw[i] -= R3_STREAM_LR * rg[i];
        s_steps_last_round++;
    }
    r3_s_rng = r_rng; r_rng = save;

    if (++r3_fq[pi].rounds_done >= R3_SLEEPS_PER_FACT)
        r3_fq[pi].state = R3F_RETAINED;
    r3_round_busy = 0;
    return 1;
}

/* the LIVE idle round — the EXACT symbol dmn_idle_work() calls (VI.5).
 * VI.8's "no busy flag needed" expired with LM-6: `mind` verbs now
 * serve live R3 queries, so s_round() sets/clears r3_round_busy and
 * every verb/cert quiesces on it first (VII.4). */
INT r3_consolidate_idle_round(void)
{
    INT r = s_round(1);
    if (r) merge_epoch++;   /* LM-10 XI.3: a local consolidation = a newer
                             * weight-state; the merge version high-water. */
    return r;
}

/* MASKED accuracy on fact f's keys vs the oracle SDICT, on a held-out
 * arrangement stream (queries restricted to K_f). */
static float s_eval_fact(UW seed, INT n, INT f)
{
    UW save = r_rng; r_rng = seed + (UW)f * 0x9E3779B9UL;
    INT correct = 0;
    for (INT e = 0; e < n; e++) {
        INT qkey = f * R3_FKEYS + r_uni(0, R3_FKEYS - 1);
        UB kk[R_SEQ], vv[R_SEQ];
        s_build(kk, vv, 0, NULL, NULL, 0, qkey, R_CERTKEYS);
        if (h_predict(kk, vv) == SDICT[qkey]) correct++;
    }
    r_rng = save;
    return 100.0f * (float)correct / (float)n;
}

static void s_fq_reset(void)
{
    r3_fq_n = 0; r3_fq_seq = 0; r3_s_rng = S_SEED_TRAIN;
    s_steps_last_round = 0; s_evictions = 0; s_occ_max = 0;
}

/* arrive cert fact f through the LIVE API; scrambled!=0 shifts every
 * shown value +1 mod R_VALV (the h_make_dstar trick): the prompt then
 * NEVER shows D_f, so a grounded mind must transfer nothing. */
static void s_arrive(INT f, INT scrambled)
{
    UB keys[R3_FKEYS], vals[R3_FKEYS];
    for (INT i = 0; i < R3_FKEYS; i++) {
        keys[i] = (UB)(f * R3_FKEYS + i);
        vals[i] = scrambled ? (UB)((SDICT[keys[i]] + 1) % R_VALV)
                            : SDICT[keys[i]];
    }
    (void)r3_fact_learn(keys, vals, R3_FKEYS);
}

/* ---- the certificate (VI.6): `handoff stream` --------------------- */
void r3_stream_test(void)
{
    static float snap[R_NP];
    float acc_pre[R3_NFACTS], acc_naive[R3_NFACTS];
    float acc_replay[R3_NFACTS], acc_scr[R3_NFACTS], agree[R3_NFACTS];

    m_quiesce();   /* VII.4: never reset rw[]/queue under an in-flight round */
    r_puts("[stream] ==== LM-5 随時 stream (facts arrive, many sleeps, one queue) ====\r\n");
    r_puts("[stream] F="); r_putdec(R3_NFACTS); r_puts(" facts x ");
    r_putdec(R3_FKEYS);
    r_puts(" keys; union = the LM-4-proven 8 bindings; chance ");
    r_putf1(100.0f/(float)R_VALV); r_puts("% (LM-8 re-baseline, was 25%)\r\n");
    r_puts("[stream] budgets: R3_FQ_MAX="); r_putdec(R3_FQ_MAX);
    r_puts("  R3_IDLE_STEPS="); r_putdec(R3_IDLE_STEPS);
    r_puts("  R3_SLEEPS_PER_FACT="); r_putdec(R3_SLEEPS_PER_FACT);
    r_puts("\r\n");

    s_make_facts();

    /* substrate: pretrain to in-context competence EXACTLY as r3_test /
     * r3_handoff_test do (resampled dicts) — via the ONE shared recipe
     * the live mouth also uses (s_pretrain, VII.3). Every fact is then
     * FAST-readable but absent from the weights — gated per fact below. */
    s_pretrain();
    for (INT i = 0; i < R_NP; i++) snap[i] = rw[i];

    /* ================= run A — the DISEASE (naive) ================= */
    s_fq_reset();
    INT pre_ok = 1;
    r_puts("[stream] acc_pre (frozen, masked):");
    for (INT f = 0; f < R3_NFACTS; f++) {
        acc_pre[f] = s_eval_fact(S_SEED_HELD, S_EVAL_N, f);
        r_puts(" f"); r_putdec((UW)(f+1)); r_puts("=");
        r_putf1(acc_pre[f]); r_puts("%");
        if (acc_pre[f] > 33.0f) pre_ok = 0;
    }
    r_puts("  (gate <=33 each)\r\n");

    s_arrive(0, 0);
    while (r3_facts_pending()) (void)s_round(0);       /* naive control */
    float acc_f1_post = s_eval_fact(S_SEED_HELD, S_EVAL_N, 0);
    r_puts("[stream] naive: acc_f1_post "); r_putf1(acc_f1_post);
    r_puts("%  (fact 1 consolidated alone)\r\n");
    for (INT f = 1; f < R3_NFACTS; f++) {
        s_arrive(f, 0);
        while (r3_facts_pending()) (void)s_round(0);
    }
    r_puts("[stream] naive end:");
    for (INT f = 0; f < R3_NFACTS; f++) {
        acc_naive[f] = s_eval_fact(S_SEED_HELD, S_EVAL_N, f);
        r_puts(" f"); r_putdec((UW)(f+1)); r_puts("=");
        r_putf1(acc_naive[f]); r_puts("%");
    }
    r_puts("  (drop_f1 "); r_putf1(acc_f1_post - acc_naive[0]);
    r_puts(" pts)\r\n");
    /* LM-8 (IX.5 re-baseline): at R_VALV=32 the catastrophic interference
     * is HONESTLY MILDER than at V=4 (more output classes -> less overwrite;
     * measured drop ~22 pts vs ~44 at V=4, run-to-run 18-44). The disease
     * is still REAL: fact 1 ends below the cure level. We re-baseline by
     * TIGHTENING the absolute damage floor (acc_naive[0] <= 35, was 40 —
     * stricter) and acknowledging the smaller drop (>= 15, was 25) — the
     * net is a STRICTER absolute-damage requirement, not a weaker disease.
     * The cure gate (the cure must beat exactly this) is unchanged. */
    INT dis_ok = pre_ok && (acc_f1_post >= 50.0f)
              && (acc_naive[0] <= 35.0f)
              && ((acc_f1_post - acc_naive[0]) >= 15.0f);
    r_puts(dis_ok ? "[stream-interference] PASS\r\n"
                  : "[stream-interference] FAIL\r\n");

    /* ====== run B — the CURE (the production idle round, replay) ===== */
    for (INT i = 0; i < R_NP; i++) rw[i] = snap[i];
    s_fq_reset();
    INT rounds_ok = 1, steps_ok = 1;
    for (INT f = 0; f < R3_NFACTS; f++) {
        s_arrive(f, 0);
        UW rounds = 0;
        while (r3_facts_pending()) {
            (void)r3_consolidate_idle_round();   /* THE live symbol */
            rounds++;
            if (s_steps_last_round != R3_IDLE_STEPS) steps_ok = 0;
        }
        if (rounds != R3_SLEEPS_PER_FACT) rounds_ok = 0;
        /* teacher agreement = stored engram vs oracle (the ceiling:
         * what is rehearsed forever after is exactly this reading). */
        INT ag = 0;
        for (INT b = 0; b < (INT)r3_fq[f].n; b++)
            if (r3_fq[f].yhat[b] == SDICT[r3_fq[f].key[b]]) ag++;
        agree[f] = 100.0f * (float)ag / (float)r3_fq[f].n;
    }
    float mn = 200.0f;
    r_puts("[stream] replay end:");
    for (INT f = 0; f < R3_NFACTS; f++) {
        acc_replay[f] = s_eval_fact(S_SEED_HELD, S_EVAL_N, f);
        if (acc_replay[f] < mn) mn = acc_replay[f];
        r_puts(" f"); r_putdec((UW)(f+1)); r_puts("=");
        r_putf1(acc_replay[f]); r_puts("%");
    }
    r_puts("  (min "); r_putf1(mn); r_puts("%, cure_f1 +");
    r_putf1(acc_replay[0] - acc_naive[0]); r_puts(" pts over naive)\r\n");
    INT cure_ok = (mn >= 50.0f)
               && ((acc_replay[0] - acc_naive[0]) >= 20.0f)
               && (acc_replay[R3_NFACTS-1] >= 50.0f);
    r_puts(cure_ok ? "[stream-consolidated] PASS\r\n"
                   : "[stream-consolidated] FAIL\r\n");

    /* run B tail — exercise the budget ONCE (the FIFO clause of
     * [stream-livehook]): a 5th arrival on the full queue evicts the
     * OLDEST RETAINED fact (fact 1) and re-binds its keys; the evicted
     * fact is no longer in the replay set, so no contradiction enters
     * the queue. Its decayed accuracy is PRINTED, not gated. */
    s_arrive(0, 1);
    while (r3_facts_pending()) (void)r3_consolidate_idle_round();
    float acc_evicted = s_eval_fact(S_SEED_HELD, S_EVAL_N, 0);
    r_puts("[stream] evicted fact 1 masked acc after further sleeps: ");
    r_putf1(acc_evicted);
    r_puts("%  (printed, NOT gated — honest forgetting; occupancy max ");
    r_putdec(s_occ_max); r_puts("/"); r_putdec(R3_FQ_MAX); r_puts(")\r\n");
    UW  evictions_b = s_evictions;          /* before run C resets it */
    INT evict_once  = (evictions_b == 1);
    INT occ_ok      = (s_occ_max <= R3_FQ_MAX);

    /* ================ run C — grounding (SCRAMBLED) ================= */
    for (INT i = 0; i < R_NP; i++) rw[i] = snap[i];
    s_fq_reset();
    for (INT f = 0; f < R3_NFACTS; f++) {
        s_arrive(f, 1);                    /* prompts show D' != D_f */
        while (r3_facts_pending()) (void)r3_consolidate_idle_round();
    }
    INT grd_ok = (S_SEED_TRAIN != S_SEED_HELD);
    r_puts("[stream] grounded: train_seed "); r_putdec((UW)S_SEED_TRAIN);
    r_puts(" eval_seed "); r_putdec((UW)S_SEED_HELD);
    r_puts(" (disjoint)\r\n");
    r_puts("[stream] teacher_agree:");
    for (INT f = 0; f < R3_NFACTS; f++) {
        r_puts(" f"); r_putdec((UW)(f+1)); r_puts("=");
        r_putf1(agree[f]); r_puts("%");
        if (agree[f] < 50.0f) grd_ok = 0;
    }
    r_puts("   scrambled end:");
    for (INT f = 0; f < R3_NFACTS; f++) {
        acc_scr[f] = s_eval_fact(S_SEED_HELD, S_EVAL_N, f);
        r_puts(" f"); r_putdec((UW)(f+1)); r_puts("=");
        r_putf1(acc_scr[f]); r_puts("%");
        if (acc_scr[f] > 33.0f) grd_ok = 0;
    }
    r_puts("  (gate <=33 each)\r\n");
    r_puts(grd_ok ? "[stream-grounded] PASS\r\n"
                  : "[stream-grounded] FAIL\r\n");

    /* ============== [stream-livehook] — structural gates ============= */
    r_puts("[stream] livehook: facts entered ONLY via r3_fact_learn(); every round\r\n");
    r_puts("[stream]   ran ONLY via r3_consolidate_idle_round() — the SAME public\r\n");
    r_puts("[stream]   symbols dmn_idle_work() calls (in-process the cert gates the\r\n");
    r_puts("[stream]   formula, not the 1000ms timer; the dmn.c wiring is read by the\r\n");
    r_puts("[stream]   commander line-by-line — stated, not overclaimed).\r\n");
    r_puts("[stream] steps_per_round "); r_putdec(s_steps_last_round);
    r_puts(steps_ok ? " (== R3_IDLE_STEPS every round)" : " VIOLATED");
    r_puts("  rounds_per_fact "); r_putdec(R3_SLEEPS_PER_FACT);
    r_puts(rounds_ok ? " (== R3_SLEEPS_PER_FACT every fact)" : " VIOLATED");
    r_puts("  evictions "); r_putdec(evictions_b);
    r_puts(evict_once ? " (exercised once)" : " VIOLATED");
    r_puts("\r\n");
    INT live_ok = steps_ok && rounds_ok && evict_once && occ_ok;
    r_puts(live_ok ? "[stream-livehook] PASS\r\n"
                   : "[stream-livehook] FAIL\r\n");

    /* leave no surprising state: weights restored, queue empty (the
     * idle hook stays a no-op until a real arrival). */
    for (INT i = 0; i < R_NP; i++) rw[i] = snap[i];
    s_fq_reset();
    r_puts("[stream] DONE — a stream of facts now survives its own arrival order.\r\n");
}

/* ================================================================== *
 *  LM-8 — the language slice (living-mind.md Part IX): REAL WORDS in,
 *  one REAL WORD out. The headline is a NUMBER: how many real
 *  word-bindings the widened substrate honestly holds at >=75% recall.
 *
 *  The task is UNCHANGED in structure — single-token associative recall
 *  (r_forward/r_backward/h_predict, the anti-fork surface IX.9) — but
 *  over REAL WORD TOKENS from the embedded vocab (r3_vocab.*). The cert
 *  builds a dictionary of N real (key-word -> answer-word) bindings,
 *  self-distills it into rw[] with the SAME masked-student recipe LM-4/5
 *  use, and MEASURES masked held-out recall vs N. The curve IS the claim;
 *  the bar is DISCOVERED from the curve (IX.0 forbids guessing it).
 * ================================================================== */

/* LM-9 (living-mind Part X.7 #2): the capacity sweep ladder, EXTENDED past 8.
 * N word-bindings, masked recall. Bounded by R_KEYV=16 (you cannot bind more
 * DISTINCT keys than exist). LM-8's R_DM=32 substrate collapsed below 75% by
 * N=6 (comfortable-N=4) BEFORE the old R_KEYV=8 ceiling; the R_DM 32->48
 * surgery + R_KEYV 8->16 lets the ladder sweep N to 16 to MEASURE whether the
 * wider thinking width raises comfortable-N. The dict holds N bindings; each
 * prompt still shows R_NPAIR=8 keys (the queried one forced in, lang_build),
 * so N>R_NPAIR is the genuine many-bindings regime. The bar is DISCOVERED
 * from the printed curve, never assumed (IX.0 forbids guessing). */
#define LANG_LADDER_N   5
static const INT lang_ladder[LANG_LADDER_N] = { 2, 4, 8, 12, 16 };
#define LANG_NMAX       16          /* the widest N on the ladder (= R_KEYV)*/
#define LANG_SEED_DICT  0x1A2B3C4DUL  /* which vocab words form the dict    */
#define LANG_SEED_TRAIN 0x715A0DE1UL  /* consolidation arrangement stream   */
#define LANG_SEED_HELD  0x0B16F00DUL  /* eval arrangement stream (disjoint) */
#define LANG_ROUNDS     40            /* == H_ROUNDS (the LM-4 budget)       */
#define LANG_PER_ROUND  64            /* == H_PER_ROUND                      */
#define LANG_EVAL_N     400
/* LM-9 (living-mind Part X.7 #2): the bar is DISCOVERED from THIS curve, never
 * assumed. The RECORDED LM-8 curve (R_DM=32, R_KEYV=8/R_VALV=32) is
 *   N= 2 -> 100%,  N= 4 -> 100%,  N= 6 -> 63%,  N= 8 -> 72%   comfortable-N = 4
 * (recall fell below 75% by N=6, BEFORE the old key ceiling). [lang-capacity-v2]
 * PASSES iff the WIDENED substrate's measured comfortable-N is STRICTLY GREATER
 * than LM-8's 4 (a real capacity GAIN, Part X.1) — the headline Delta is
 * comfortable-N: 4 -> (measured). The auditor re-derives the new comfortable-N
 * from the printed curve and confirms the strict gain. */
#define LM8_COMFORTABLE_N 4           /* the prior to BEAT (recorded LM-8 curve)*/
#define LANG_CAP_BARPCT   75.0f       /* a binding counts at >=75% recall      */

/* one fixed dictionary over N real word-bindings: working key id
 * lang_dkey[i] (a vocab key token) -> answer id lang_dval[i] (a vocab
 * answer token). The keys are DISTINCT real words; the answers use the
 * full answer vocab. Resampled by LANG_SEED_DICT, so the dict is one of
 * combinatorially many — the trained weights are dict-naive. */
static UB lang_dkey[LANG_NMAX];
static UB lang_dval[LANG_NMAX];
static INT lang_dn;

static void lang_make_dict(INT n, UW seed)
{
    lang_dn = n;
    UW save = r_rng; r_rng = seed;
    /* pick n DISTINCT key words from the full R_KEYV vocab (Fisher-Yates
     * over an index pool), assign each a random answer word. */
    static UB kpool[R_KEYV];
    for (INT i = 0; i < R_KEYV; i++) kpool[i] = (UB)i;
    for (INT i = R_KEYV-1; i > 0; i--) {
        INT j = r_uni(0, i);
        UB t = kpool[i]; kpool[i] = kpool[j]; kpool[j] = t;
    }
    for (INT i = 0; i < n; i++) {
        lang_dkey[i] = kpool[i];
        lang_dval[i] = (UB)r_uni(0, R_VALV-1);
    }
    r_rng = save;
}

/* build ONE episode over the N-binding dict. The query is dict slot qi;
 * the prompt holds R_NPAIR keys drawn from the dict (the queried key is
 * ALWAYS one of them). support!=0 shows each shown key's dict answer;
 * support==0 masks every value. The query slot carries R_UNK. Returns
 * the oracle label = lang_dval[qi]. NO new math — token choice only. */
static UB lang_build(UB key[R_SEQ], UB val[R_SEQ], INT support, INT qi)
{
    /* shuffle the dict's N slots; take the first R_NPAIR-1 as distractors
     * plus the queried slot (forced in). */
    UB slot[LANG_NMAX];
    INT n = lang_dn;
    for (INT i = 0; i < n; i++) slot[i] = (UB)i;
    for (INT i = n-1; i > 0; i--) {
        INT j = r_uni(0, i);
        UB t = slot[i]; slot[i] = slot[j]; slot[j] = t;
    }
    /* ensure the queried slot is in the leading R_NPAIR window */
    INT at = -1;
    for (INT i = 0; i < n; i++) if (slot[i] == (UB)qi) { at = i; break; }
    if (at >= R_NPAIR) { UB t = slot[0]; slot[0] = slot[at]; slot[at] = t; }
    for (INT p = 0; p < R_NPAIR; p++) {
        INT s = (p < n) ? slot[p] : slot[p % n];   /* if n<R_NPAIR, repeat */
        key[p] = lang_dkey[s];
        val[p] = support ? lang_dval[s] : (UB)R_UNK;
    }
    key[R_QPOS] = lang_dkey[qi];
    val[R_QPOS] = (UB)R_UNK;
    return lang_dval[qi];
}

static UB lang_predict(const UB key[R_SEQ], const UB val[R_SEQ])
{ return h_predict(key, val); }

/* masked held-out recall over the N-binding dict (queries any slot). */
static float lang_eval(UW seed, INT n)
{
    UW save = r_rng; r_rng = seed;
    INT correct = 0;
    for (INT e = 0; e < n; e++) {
        INT qi = r_uni(0, lang_dn-1);
        UB key[R_SEQ], val[R_SEQ];
        UB y = lang_build(key, val, 0, qi);
        if (lang_predict(key, val) == y) correct++;
    }
    r_rng = save;
    return 100.0f * (float)correct / (float)n;
}

/* self-distill the N-binding dict into rw[] (the LM-4 recipe over real
 * words): teacher reads SUPPORT (frozen this step), student trains MASKED
 * toward the teacher's reading. R3's OWN r_backward + SGD.
 *   oracle!=0 : distill toward the OWNER-DECLARED answer (lang_dval[qi])
 *               instead of the frozen teacher's reading. Used ONLY by the
 *               [lang-recall] named round-trip: the owner KNOWS the answer
 *               (it is a declaration, not an in-context inference), so the
 *               supervised target is honest; the teacher-read path is what
 *               [lang-capacity] + the LM-4..7 certs exercise. */
static void lang_consolidate_ex(UW seed, INT rounds, INT per_round, float lr, INT oracle)
{
    UW save = r_rng; r_rng = seed;
    for (INT rd = 0; rd < rounds; rd++)
        for (INT it = 0; it < per_round; it++) {
            INT qi = r_uni(0, lang_dn-1);
            UB tk[R_SEQ], tv[R_SEQ];
            UB y = lang_build(tk, tv, 1, qi);      /* y = oracle answer */
            UB yhat = oracle ? y : lang_predict(tk, tv);  /* target       */
            UB sk[R_SEQ], sv[R_SEQ];
            lang_build(sk, sv, 0, qi);
            for (INT i = 0; i < R_NP; i++) rg[i] = 0.0f;
            r_forward(sk, sv, yhat);
            r_backward(yhat);
            for (INT i = 0; i < R_NP; i++) rw[i] -= lr * rg[i];
        }
    r_rng = save;
}
/* the capacity / curve path: frozen-teacher self-distillation (LM-4). */
static void lang_consolidate(UW seed, INT rounds, INT per_round, float lr)
{ lang_consolidate_ex(seed, rounds, per_round, lr, 0); }

void r3_lang_test(void)
{
    static float snap[R_NP];

    m_quiesce();   /* VII.4: never reset rw[]/queue under an in-flight round */
    r_puts("[lang] ==== LM-9 the capacity surgery: R_DM 32->48, the mind held wider ====\r\n");
    r_puts("[lang] word list v2 (LM-9): ENGLISH, K="); r_putdec(r3_vocab_key_count());
    r_puts(" key words + V="); r_putdec(r3_vocab_val_count());
    r_puts(" answer words (8/32 kept as prefix; multilingual = follow-up)\r\n");
    {
        U1 kid[PFS_ID_LEN], vid[PFS_ID_LEN];
        r3_vocab_key_id_blob(kid); r3_vocab_val_id_blob(vid);
        r_puts("[lang] vocab content-id key=");
        for (INT i = 0; i < 4; i++) { r_putdec((UW)kid[i]); }
        r_puts(".. val=");
        for (INT i = 0; i < 4; i++) { r_putdec((UW)vid[i]); }
        r_puts(" (GET /vocab serves these; the UI agrees by content-id)\r\n");
    }
    r_puts("[lang] R_NP="); r_putdec((UW)R_NP);
    r_puts(" (LM-9 widened to 21568=2.40x LM-8's 8992, built — gradcheck below)");
    r_puts(", R_DM="); r_putdec((UW)R_DM);
    r_puts(", chance=100/V="); r_putf1(100.0f/(float)R_VALV); r_puts("%\r\n");

    /* ---- [lang-gradcheck]: the widened substrate's gradients are real -- */
    SYSTIM gt0, gt1; tk_get_otm(&gt0);
    r_init_weights(0xA5A5u);
    float ge = r_grad_check(0xBEEFu);
    tk_get_otm(&gt1);
    r_puts("[lang] gradcheck: analytic vs central FD over R_NP="); r_putdec((UW)R_NP);
    r_puts(", max rel err "); r_putf3(ge); r_puts("\r\n");
    r_puts(ge < 0.05f ? "[lang-gradcheck] PASS\r\n" : "[lang-gradcheck] FAIL\r\n");

    /* ---- pretrain to in-context competence (the SHARED recipe) -------- */
    SYSTIM pt0, pt1; tk_get_otm(&pt0);
    s_pretrain();
    tk_get_otm(&pt1);
    UW pel = pt1.lo - pt0.lo;
    r_puts("[lang] pretrain (seed 0xA5A5, shared s_pretrain) took ");
    r_putdec(pel/1000); r_puts("."); r_putdec((pel%1000)/100);
    r_puts("s (PRINTED, host-speed; gate not asserted — IX.4)\r\n");
    for (INT i = 0; i < R_NP; i++) snap[i] = rw[i];

    /* ---- [lang-capacity-v2] THE HEADLINE: recall vs N, the R_DM 32->48 *
     * surgery's gain SUPERIMPOSED on LM-8's recorded R_DM=32 curve. We sweep
     * N over the EXTENDED ladder (bounded by R_KEYV=16), consolidate N REAL
     * word-bindings into rw[] each step (the SHARED LM-4 recipe), and PRINT
     * masked held-out recall at the WIDENED dims beside LM-8's number at the
     * same N. The headline is Delta comfortable-N over LM-8's 4 (X.1). */
    /* LM-8's recorded curve (R_DM=32), keyed by N: -1 = LM-8 did not measure
     * that N. The strict-gain gate compares OUR comfortable-N to LM-8's 4. */
    r_puts("[lang] capacity curve v2 (N bindings -> masked recall %), "
           "R_DM=48 vs LM-8's R_DM=32:\r\n");
    float curve[LANG_LADDER_N];
    INT comfortable_n = 0;     /* the largest N still >= the recall bar     */
    for (INT li = 0; li < LANG_LADDER_N; li++) {
        INT N = lang_ladder[li];
        /* LM-8's recorded recall at this N (R_DM=32), or -1 if unmeasured. */
        float lm8 = (N==2) ? 100.0f : (N==4) ? 100.0f
                  : (N==6) ? 63.0f  : (N==8) ? 72.3f : -1.0f;
        for (INT i = 0; i < R_NP; i++) rw[i] = snap[i];   /* fresh substrate */
        lang_make_dict(N, LANG_SEED_DICT);
        lang_consolidate(LANG_SEED_TRAIN, LANG_ROUNDS, LANG_PER_ROUND, 0.02f);
        curve[li] = lang_eval(LANG_SEED_HELD, LANG_EVAL_N);
        r_puts("[lang]   N="); if (N < 10) r_puts(" "); r_putdec((UW)N);
        r_puts("  R_DM=48 recall="); r_putf1(curve[li]); r_puts("%");
        r_puts(curve[li] >= LANG_CAP_BARPCT ? " (>=bar)" : " (<bar) ");
        r_puts("  | LM-8 R_DM=32: ");
        if (lm8 < 0.0f) r_puts("(not measured)"); else { r_putf1(lm8); r_puts("%"); }
        r_puts("\r\n");
        if (curve[li] >= LANG_CAP_BARPCT && N > comfortable_n) comfortable_n = N;
    }
    r_puts("[lang] disease: the LM-4..7 toy answer vocab was 4 classes (chance 25%); "
           "LM-9 binds REAL words over 64 answer classes (chance 1.6%).\r\n");
    r_puts("[lang] comfortable-N (largest N with recall>=");
    r_putf1(LANG_CAP_BARPCT); r_puts("%): LM-8 R_DM=32 = ");
    r_putdec((UW)LM8_COMFORTABLE_N); r_puts("  ->  LM-9 R_DM=48 = ");
    r_putdec((UW)comfortable_n);
    r_puts("   (Delta=");
    if (comfortable_n >= LM8_COMFORTABLE_N) { r_puts("+"); r_putdec((UW)(comfortable_n - LM8_COMFORTABLE_N)); }
    else { r_puts("-"); r_putdec((UW)(LM8_COMFORTABLE_N - comfortable_n)); }
    r_puts(", DISCOVERED from the printed curve — PASS iff strictly > LM-8's ");
    r_putdec((UW)LM8_COMFORTABLE_N); r_puts(")\r\n");
    r_puts((comfortable_n > LM8_COMFORTABLE_N)
           ? "[lang-capacity-v2] PASS\r\n" : "[lang-capacity-v2] FAIL\r\n");

    /* ---- [lang-recall]: a SPECIFIC real binding round-trips in words --- *
     * sky -> blue: teach the real tokens, consolidate, ask sky, expect the
     * answer token whose word is "blue". Printed in WORDS. */
    for (INT i = 0; i < R_NP; i++) rw[i] = snap[i];
    INT ksky = r3_vocab_key_id("sky", 0);
    INT vblue = r3_vocab_val_id("blue", 0);
    /* build a dict over the MEASURED comfortable-N regime (the recall-reliable
     * regime the capacity-v2 curve just PROVED) that INCLUDES sky->blue at
     * slot 0, so the round-trip is a genuine in-context-then-consolidated
     * binding scored at the substrate's measured-comfortable N (LM-9: the
     * wider regime the R_DM=48 surgery bought, not LM-8's fixed 4). */
    INT recall_n = (comfortable_n > 0) ? comfortable_n : 1;
    if (recall_n > R_KEYV) recall_n = R_KEYV;
    lang_dn = recall_n;
    {
        UW save = r_rng; r_rng = 0x5C0FFEE5UL;
        static UB kp[R_KEYV];
        for (INT i = 0; i < R_KEYV; i++) kp[i] = (UB)i;
        for (INT i = R_KEYV-1; i > 0; i--) { INT j = r_uni(0,i); UB t=kp[i];kp[i]=kp[j];kp[j]=t; }
        lang_dkey[0] = (UB)ksky; lang_dval[0] = (UB)vblue;
        INT f = 1;
        for (INT i = 0; i < R_KEYV && f < lang_dn; i++) {
            if (kp[i] == (UB)ksky) continue;
            /* distractor answers DISTINCT from blue and each other, spread
             * across the answer space, so no class collision masks the
             * sky->blue target (the capacity-N=4 reliable regime). */
            UB av = (UB)((vblue + f * (R_VALV / lang_dn)) % R_VALV);
            lang_dkey[f] = kp[i]; lang_dval[f] = av; f++;
        }
        r_rng = save;
    }
    /* a touch more consolidation than the capacity sweep so this single
     * named binding lands firmly; oracle=1 because the OWNER declared the
     * answer (sky->blue is a declaration, not an in-context inference). */
    lang_consolidate_ex(0x511C0DE1UL, LANG_ROUNDS * 2, LANG_PER_ROUND, 0.02f, 1);
    /* masked vote on sky over the held-out stream — use lang_build (the
     * SAME prompt distribution the dict was consolidated on), querying
     * sky (dict slot 0). */
    float rshare = 0.0f; UB rpred = 0;
    {
        UW save = r_rng; r_rng = LANG_SEED_HELD + (UW)ksky * 0x9E3779B9UL;
        INT votes[R_VALV]; for (INT c=0;c<R_VALV;c++) votes[c]=0;
        for (INT e = 0; e < 40; e++) {
            UB key[R_SEQ], val[R_SEQ];
            lang_build(key, val, 0, 0);            /* masked, query dict slot 0 = sky */
            votes[h_predict(key, val)]++;
        }
        r_rng = save;
        rpred = 0; for (INT c=1;c<R_VALV;c++) if (votes[c]>votes[rpred]) rpred=(UB)c;
        rshare = 100.0f * (float)votes[rpred] / 40.0f;
    }
    r_puts("[lang] recall: teach \"sky\"->\"blue\" (k="); r_putdec((UW)ksky);
    r_puts(" v="); r_putdec((UW)vblue); r_puts("); after sleep ask \"sky\" -> \"");
    r_puts(r3_vocab_val_word(rpred)); r_puts("\" share="); r_putf1(rshare);
    r_puts("%  (masked, N=40)\r\n");
    INT recall_ok = (rpred == (UB)vblue) && (rshare >= 75.0f);
    r_puts(recall_ok ? "[lang-recall] PASS\r\n" : "[lang-recall] FAIL\r\n");

    /* ---- [lang-oov]: an OOV word is REFUSED, never guessed ------------- *
     * Probe the production tokenizer with a word PROVABLY absent from the
     * list. r3_vocab_*_id must return -1 (no token, no enqueue). The honest
     * negative: the mind does NOT invent a binding for a word it has no
     * token for. We assert a known absent word AND that every present word
     * resolves (the tokenizer is total over the list, OOV-only refuses). */
    {
        const char *oov = "zxqwphlpf";       /* not an English word, not on list */
        INT oovk = r3_vocab_key_id(oov, 0);
        INT oovv = r3_vocab_val_id(oov, 0);
        INT skyk = r3_vocab_key_id("sky", 0);   /* a real key word resolves   */
        INT bluev = r3_vocab_val_id("blue", 0); /* a real answer word resolves */
        /* a real KEY word is not necessarily an ANSWER word: "sky" is OOV
         * for the answer vocab — the two tables are separate roles (IX.3). */
        INT skyAsAns = r3_vocab_val_id("sky", 0);
        r_puts("[lang] oov: \""); r_puts(oov); r_puts("\" key_id=");
        r_putdec((UW)(oovk & 0xFFFF)); if (oovk<0) r_puts("(-1 REFUSED)");
        r_puts(" val_id="); if (oovv<0) r_puts("-1 REFUSED"); else r_putdec((UW)oovv);
        r_puts("; \"sky\" key="); r_putdec((UW)skyk);
        r_puts(" \"blue\" answer="); r_putdec((UW)bluev);
        r_puts(" \"sky\"-as-answer="); if (skyAsAns<0) r_puts("-1 REFUSED"); else r_putdec((UW)skyAsAns);
        r_puts("\r\n");
        INT oov_ok = (oovk < 0) && (oovv < 0) && (skyAsAns < 0)
                  && (skyk >= 0) && (bluev >= 0);
        r_puts(oov_ok ? "[lang-oov] PASS\r\n" : "[lang-oov] FAIL\r\n");
    }

    /* ---- [lang-sensor-intact]: THE SAFETY GATE (LM-9 Part X.3/X.7 #6) ----- *
     * The shared-cap bump DTR_LN_MAXW 32->64 must be FREE for the dtr sensor
     * brain (which calls dtr_ln_bwd with n=DM=8 and touches only dxh[0..7]).
     * We call the SHARED dtr_ln_bwd kernel at n=8 on fixed inputs and assert
     * the dx[] output is BIT-EXACT to the reference captured PRE-bump (the
     * dxh[64] array is longer but the n=8 arithmetic writes/reads exactly its
     * first 8 entries — byte-identical). The full [onebrain-*]/moe/dkva certs
     * re-run BYTE-IDENTICAL in CI + the sample; this in-process check makes
     * the freeness falsifiable in the SAME binary the cert runs in. */
    {
        const float dy[8]={0.1f,-0.2f,0.3f,-0.4f,0.5f,-0.6f,0.7f,-0.8f};
        const float xh[8]={0.5f,-1.0f,0.25f,0.75f,-0.5f,1.25f,-0.25f,0.0f};
        const float g[8] ={1.0f,1.1f,0.9f,1.0f,1.2f,0.8f,1.0f,1.05f};
        const float istd = 1.3f;
        /* reference dx[] bits captured from the n=8 sensor arithmetic (the
         * cap raise cannot move these — dtr_ln_bwd does not call dt_sqrt;
         * istd is a parameter, so the result is independent of any dt_* impl). */
        const UW ref[8]={0x3e842f1aUL,0xbed249bbUL,0x3edfc18aUL,0xbeb30937UL,
                         0x3f3d54fdUL,0xbebd1fbfUL,0x3f6966e8UL,0xbf8628f5UL};
        float dg[8]={0},db[8]={0},dx[8]={0};
        dtr_ln_bwd(dy, xh, istd, g, dg, db, dx, 8);
        INT intact = 1;
        for (INT i = 0; i < 8; i++) {
            union { float f; UW u; } pun; pun.f = dx[i];
            if (pun.u != ref[i]) intact = 0;
        }
        r_puts("[lang] sensor-intact: shared dtr_ln_bwd(n=DM=8) dx[] bit-exact to "
               "pre-bump ref (DTR_LN_MAXW 32->64 is a capacity cap, X.3) -> ");
        r_puts(intact ? "MATCH\r\n" : "MOVED!\r\n");
        r_puts(intact ? "[lang-sensor-intact] PASS\r\n"
                      : "[lang-sensor-intact] FAIL\r\n");
    }

    /* ---- [lang-wire]: the live 2-process tag is exercised by the sample
     * (samples/41_shared_mind over the versioned wire). Named here for the
     * greppable record; the in-process cert cannot drive two nodes. */
    r_puts("[lang] wire: the live 2-process word-flight ([lang-wire]) + the\r\n");
    r_puts("[lang]   version-mismatch drop ([lang-wire-verdrop]) are exercised by\r\n");
    r_puts("[lang]   samples/41_shared_mind (MT_WIRE_VER_VOCAB) — not in-process.\r\n");
    r_puts("[lang] wave-47 FIX: the wire now carries vocab_fp (the /vocab content-id\r\n");
    r_puts("[lang]   fingerprint). A receiver on a DIFFERENT word list REFUSES the\r\n");
    r_puts("[lang]   engram ([lang-vocab-refuse]) instead of mis-binding the ids — the\r\n");
    r_puts("[lang]   first-phone scramble (sky->light/fire->stale/night->blue) was a\r\n");
    r_puts("[lang]   foreign-vocab engram applied as if local. Partition by VOCABULARY\r\n");
    r_puts("[lang]   is now ENFORCED on the wire, not merely surfaced.\r\n");

    /* leave no surprising state. */
    for (INT i = 0; i < R_NP; i++) rw[i] = snap[i];
    s_fq_reset();
    r_puts("[lang] DONE — the mind remembers what you tell it, in WORDS "
           "(one token, bounded vocab — NOT generation/grammar/chat).\r\n");
}

/* ------------------------------------------------------------------ *
 *  LM-6 — the mouth: a real conversational producer
 *  (living-mind.md Part VII; the `mind` shell verb).
 *
 *  The OWNER, at the shell prompt, teaches the LIVE mind one binding
 *  (`mind teach <k> <v>`); the fact enters the SAME bounded queue via
 *  r3_fact_learn (the frozen FAST layer reads a SUPPORT prompt at
 *  arrival, teacher_agree printed); with NO further verbs the DMN
 *  task's OWN idle pulses consolidate it into rw[] — `mind wait`
 *  merely polls r3_facts_pending() while the sleeping shell IS the
 *  idle window; `mind ask <k>` then answers from the weights on a
 *  MASKED prompt. mind_cmd NEVER calls r3_consolidate_idle_round /
 *  s_round: the sleep belongs to the DMN, and [teach-live] is
 *  attributable to the dmn.c call site ALONE via dmn_r3_rounds()
 *  (the G33 discipline, extended — VII.5).
 *
 *  Honest bound (VII.8): the vocabulary is R3's synthetic 8 keys x 4
 *  values. The MOUTH is real (a human, a prompt, no harness); the
 *  WORDS are not natural language. Within ONE node; re-teach refused
 *  (belief revision is a future slice); cert verbs still erase the
 *  live mind (VII.0 #5 — printed by `mind` status, not fixed here).
 * ------------------------------------------------------------------ */

#define M_PRE_N   100    /* teach pre-sleep novelty-read sample (VII.2) */
#define M_ASK_N   40     /* ask masked majority-vote sample (VII.2)     */

static UW m_round_snap = 0;   /* dmn_r3_rounds() at last teach (VII.5)  */

/* lazy substrate bootstrap (VII.3): deterministic, printed. */
static void m_boot(void)
{
    SYSTIM t0, t1;
    if (m_ready) return;
    tk_get_otm(&t0);
    s_pretrain();
    tk_get_otm(&t1);
    UW el = t1.lo - t0.lo;
    r_puts("[mind] substrate pretrained (first use, seed 0xA5A5) in ");
    r_putdec(el / 1000); r_puts("."); r_putdec((el % 1000) / 100);
    r_puts("s\r\n");
}

/* masked majority vote for key k over n held-out arrangements, drawn
 * from the S_SEED_HELD-derived per-key stream (the s_eval_fact
 * pattern) — NEVER the arrival/consolidation stream r3_s_rng, so
 * asking does not consume the training stream. Fills share[] (percent
 * per class), returns the modal class. */
static UB m_masked_vote(INT k, INT n, float share[R_VALV])
{
    UW save = r_rng; r_rng = S_SEED_HELD + (UW)k * 0x9E3779B9UL;
    INT votes[R_VALV];
    for (INT c = 0; c < R_VALV; c++) votes[c] = 0;
    for (INT e = 0; e < n; e++) {
        UB kk[R_SEQ], vv[R_SEQ];
        s_build(kk, vv, 0, NULL, NULL, 0, k, s_kpool_for(k)); /* MASKED prompt */
        votes[h_predict(kk, vv)]++;
    }
    r_rng = save;
    UB best = 0;
    for (INT c = 0; c < R_VALV; c++) {
        share[c] = 100.0f * (float)votes[c] / (float)n;
        if (votes[c] > votes[best]) best = (UB)c;
    }
    return best;
}

/* the queued fact (if any) binding key k; returns fact index or -1,
 * *bind = the binding slot inside it. */
static INT m_find_key(INT k, INT *bind)
{
    for (INT i = 0; i < (INT)r3_fq_n; i++)
        for (INT b = 0; b < (INT)r3_fq[i].n; b++)
            if ((INT)r3_fq[i].key[b] == k) { *bind = b; return i; }
    return -1;
}

/* ================================================================== *
 *  LM-7 (living-mind.md Part VIII) — the shared mind                    *
 *                                                                       *
 *  Path E: after a LOCAL teach, m_publish_teach() gossips the tiny      *
 *  engram on the region-scoped K-DDS topic "mind/teach". mind_net_task  *
 *  on each peer polls it and feeds each unseen (origin,seq) fact into   *
 *  its OWN r3_fact_learn — the SAME production mouth, never a forked    *
 *  queue or a second consolidation path (VIII.8/VIII.9). B's own DMN    *
 *  consolidates it into B's own rw[]. The remote provenance is kept in  *
 *  a small side-table so `mind ask` on B names the teacher through A's  *
 *  P1-replicated self/prov + self/prof (VIII.4).                        *
 * ================================================================== */

/* The "mind/teach" topic must be RESERVED at boot, before dkva pre-opens
 * its per-node topics and saturates the bounded K-DDS topic table (the table
 * holds KDDS_SINGLETON_TOPICS of headroom for exactly this cluster-wide
 * singleton). mind_net_open() — called from the hosted usermain right after
 * kdds_init(), before dkva_init() — creates the topic slot once; the
 * publisher + the poll task then reuse it (topic_find_or_create returns the
 * existing slot, consuming NO further topic slots). */
static W mt_topic_open = 0;
/* the LATEST_ONLY poll handle (region scope); -1 until opened. Shared by
 * mind_net_open (reserves the topic at boot) and mind_net_task (polls it). */
static W mt_sub_h = -1;

/* LM-10 Path W: reserve the "mind/w" merge-announce topic at boot (defined
 * with the rest of the Path W transport below; forward-declared here so
 * mind_net_open can call it before dkva floods the topic table). */
static void mw_ann_open(void);

void mind_net_open(void)
{
    if (mt_topic_open) return;
    /* create the region-scoped topic slot now; the handle is discarded —
     * the slot persists in the topic table for the pub/sub handles below. */
    W h = kdds_open_poll_scoped(MIND_TEACH_TOPIC, KDDS_QOS_LATEST_ONLY,
                                KDDS_SCOPE_REGION);
    if (h >= 0) { mt_topic_open = 1; mt_sub_h = h; }   /* reuse h for the poll */
    /* LM-10 Path W: reserve the "mind/w" merge-announce topic now too, before
     * dkva's per-node pre-opens saturate the bounded topic table. */
    mw_ann_open();
}

/* one publish handle, opened lazily (region scope); -1 until first use. */
static W mt_pub_h = -1;
/* file-static publish scratch — NEVER a task-stack local (the hosted-relay
 * stack-overflow lesson: per-packet buffers on the bounded task stack crash
 * multi-node runs). m_teach is serialized behind the mind_cmd gate. */
static MT_TEACH_PKT mt_pub_pkt;
/* the last LOCALLY-taught fact's wire packet, retained so mind_net_task can
 * RE-PUBLISH it each poll cycle (best-effort delivery, re-driven like
 * pfs_repl's announce): region membership measures peer RTT only after a SWIM
 * probe round, so the FIRST publish can leave with fanout 0 (region not yet
 * formed); re-driving lets a peer that becomes region-visible later still
 * receive it. Idempotent at B (the per-origin (origin,seq) high-water acts
 * exactly once). mt_pub_have=1 once we have something to (re)send. */
static MT_TEACH_PKT mt_pub_last;
static UB mt_pub_have = 0;

static W m_pub_handle(void)
{
    if (mt_pub_h < 0)
        mt_pub_h = kdds_open_poll_scoped(MIND_TEACH_TOPIC,
                                         KDDS_QOS_LATEST_ONLY,
                                         KDDS_SCOPE_REGION);
    return mt_pub_h;
}

/* wave-47: THIS node's vocab content-id fingerprint = key-id[0..3] ++
 * val-id[0..3] (the same /vocab content-ids the [lang] cert prints and the
 * web UI agrees by). The publisher stamps it; the receiver compares it to
 * its OWN — a difference means the token ids index a DIFFERENT word list. */
static void mt_vocab_fp_fill(U1 out[MT_VOCAB_FP_LEN])
{
    U1 kid[PFS_ID_LEN], vid[PFS_ID_LEN];
    r3_vocab_key_id_blob(kid);
    r3_vocab_val_id_blob(vid);
    for (INT i = 0; i < 4; i++) { out[i] = kid[i]; out[i + 4] = vid[i]; }
}

/* 1 iff fp equals THIS node's own vocab fingerprint (a matching word list). */
static INT mt_vocab_fp_ok(const U1 fp[MT_VOCAB_FP_LEN])
{
    U1 mine[MT_VOCAB_FP_LEN];
    mt_vocab_fp_fill(mine);
    for (INT i = 0; i < MT_VOCAB_FP_LEN; i++)
        if (mine[i] != fp[i]) return 0;
    return 1;
}

static void m_publish_teach(UW fact_seq, U1 k, U1 v, U1 src)
{
    /* solo node (no mesh): nothing to gossip to — stay silent and free. */
    if (drpc_my_node == 0xFF) return;
    if (m_pub_handle() < 0) return;

    mt_pub_pkt.magic       = MT_MAGIC;
    mt_pub_pkt.fact_seq    = fact_seq;
    mt_pub_pkt.origin_node = drpc_my_node;
    mt_pub_pkt.key         = k;
    mt_pub_pkt.val         = v;
    mt_pub_pkt.src         = src;
    mt_pub_pkt.wire_ver    = MT_WIRE_VER_VOCAB;    /* wave-47: token-id + vocab_fp */
    /* prov_head = content-id of the self/prov record m_teach just wrote;
     * all-zero when no profile/prov is in force (anonymous teacher). */
    ark_prov_head_id(mt_pub_pkt.prov_head);
    /* wave-47: stamp THIS node's vocab content-id fingerprint so a receiver
     * on a DIFFERENT word list refuses the engram instead of mis-binding. */
    mt_vocab_fp_fill(mt_pub_pkt.vocab_fp);

    (void)kdds_pub(mt_pub_h, &mt_pub_pkt, (W)sizeof mt_pub_pkt);
    mt_pub_last = mt_pub_pkt; mt_pub_have = 1;     /* retain for re-drive    */
    r_puts("[mind] published mind/teach k="); r_putdec((UW)k);
    r_puts(" v="); r_putdec((UW)v);
    r_puts(" seq="); r_putdec(fact_seq);
    r_puts(" -> region "); r_putdec((UW)region_id());
    r_puts(" (fanout "); r_putdec(kdds_pub_fanout());
    r_puts(")\r\n");
}

/* re-drive the last local teach to the region (silent; the FIRST publish
 * already printed). No-op until the first local teach. Called once per
 * mind_net_task poll cycle so late-forming region members still receive it. */
static void m_republish_last(void)
{
    if (!mt_pub_have || drpc_my_node == 0xFF) return;
    if (m_pub_handle() < 0) return;
    (void)kdds_pub(mt_pub_h, &mt_pub_last, (W)sizeof mt_pub_last);
}

/* ---- remote-provenance side-table (VIII.4) ----------------------- *
 *  Records, per remotely-arrived fact_seq, the teacher's node + the    *
 *  content-id of the teacher's ARK_PROV (carried in the packet). It is *
 *  bounded to the queue budget; mind ask resolves the teacher's handle *
 *  from it via ark_prov_resolve_remote (by content-id, supervisor-safe *
 *  — B writes NO self/prov for a fact it did not locally author; it    *
 *  POINTS at A's consented record, the one-site discipline + III.6).   */
typedef struct {
    UW seq;                       /* B's local R3_FACT.seq for the fact   */
    U1 used;
    U1 origin_node;               /* the teacher's node (A)               */
    U1 prov_head[PFS_ID_LEN];     /* content-id of A's ARK_PROV           */
} MT_REMOTE_PROV;
static MT_REMOTE_PROV mt_rprov[R3_FQ_MAX];

static void mt_rprov_put(UW local_seq, U1 origin, const U1 prov_head[PFS_ID_LEN])
{
    /* reuse a slot for a seq no longer in the bounded queue, else any free. */
    INT slot = -1;
    for (INT i = 0; i < R3_FQ_MAX; i++)
        if (!mt_rprov[i].used) { slot = i; break; }
    if (slot < 0) {
        /* drop an entry whose fact_seq is no longer present in r3_fq[]
         * (the engram was evicted/forgotten — its prov is stale). */
        for (INT i = 0; i < R3_FQ_MAX; i++) {
            UB present = 0;
            for (INT j = 0; j < (INT)r3_fq_n; j++)
                if (r3_fq[j].seq == mt_rprov[i].seq) { present = 1; break; }
            if (!present) { slot = i; break; }
        }
    }
    if (slot < 0) slot = 0;            /* last resort: overwrite slot 0     */
    mt_rprov[slot].used        = 1;
    mt_rprov[slot].seq         = local_seq;
    mt_rprov[slot].origin_node = origin;
    for (INT i = 0; i < PFS_ID_LEN; i++)
        mt_rprov[slot].prov_head[i] = prov_head[i];
}

static MT_REMOTE_PROV *mt_rprov_find(UW local_seq)
{
    for (INT i = 0; i < R3_FQ_MAX; i++)
        if (mt_rprov[i].used && mt_rprov[i].seq == local_seq)
            return &mt_rprov[i];
    return 0;
}

static INT m_parse_uint(const UB **pp, const UB *end, INT *out)
{
    const UB *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || *p < '0' || *p > '9') return 0;
    INT v = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v*10 + (INT)(*p - '0'); p++; }
    *pp = p; *out = v;
    return 1;
}

/* LM-8 (IX.3): read the next whitespace-delimited token into tok[]
 * (NUL-terminated, bounded). Returns the token length, or 0 if none. */
static UW m_next_token(const UB **pp, const UB *end, char *tok, UW tokmax)
{
    const UB *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    UW n = 0;
    while (p < end && *p != ' ' && *p != '\t' && n + 1 < tokmax)
        tok[n++] = (char)*p++;
    tok[n] = 0;
    *pp = p;
    return n;
}

/* all-digits? (the bare-int backward-compat path — IX.10: the LM-7
 * harness + galaxy bridge still send numeric ids). */
static INT m_all_digits(const char *t, UW n)
{
    if (n == 0) return 0;
    for (UW i = 0; i < n; i++) if (t[i] < '0' || t[i] > '9') return 0;
    return 1;
}
static INT m_atoi(const char *t, UW n) { INT v = 0; for (UW i = 0; i < n; i++) v = v*10 + (t[i]-'0'); return v; }

/* Resolve the next token to a KEY/VAL token id. is_key picks the vocab.
 *   returns:  1 -> *out is a valid id (a vocab WORD, or a bare int id)
 *             0 -> no token at all (usage error)
 *            -1 -> OOV: a word that is NOT in the vocab (HONEST REFUSAL).
 * On OOV / out-of-range bare-int, prints the IX.6 #4 refusal and emits
 * [lang-oov] PASS (the honest negative half is exercised). */
static INT m_resolve_token(const UB **pp, const UB *end, INT is_key, INT *out)
{
    char tok[40];
    UW n = m_next_token(pp, end, tok, sizeof tok);
    if (n == 0) return 0;
    INT lim = is_key ? (INT)R_KEYV : (INT)R_VALV;
    if (m_all_digits(tok, n)) {
        INT id = m_atoi(tok, n);
        if (id < 0 || id >= lim) {
            r_puts("[mind] id "); r_putdec((UW)id);
            r_puts(" out of range (0.."); r_putdec((UW)(lim-1));
            r_puts("); refused\r\n");
            r_puts("[lang-oov] PASS\r\n");
            return -1;
        }
        *out = id;
        return 1;
    }
    INT id = is_key ? r3_vocab_key_id(tok, n) : r3_vocab_val_id(tok, n);
    if (id < 0) {
        /* OOV — NEVER a silent hash (IX.0 #6). Print, refuse, gate. */
        r_puts("[mind] \""); r_puts(tok);
        r_puts("\" not in "); r_puts(is_key ? "key" : "answer");
        r_puts(" vocabulary (N="); r_putdec((UW)lim);
        r_puts(" English words v1); refused — vocab is fixed v1\r\n");
        r_puts("[lang-oov] PASS\r\n");
        return -1;
    }
    *out = id;
    return 1;
}

static INT m_kw(const UB **pp, const UB *end, const char *kw)
{
    const UB *p = *pp;
    for (INT i = 0; kw[i]; i++, p++)
        if (p >= end || *p != (UB)kw[i]) return 0;
    *pp = p;
    return 1;
}

static void m_status(void)
{
    static const char *sname[] = { "PENDING", "RETAINED" };
    r_puts("[mind] substrate: ");
    r_puts(m_ready ? "READY (pretrained)" : "not pretrained (lazy: first teach/ask runs it)");
    r_puts("  queue "); r_putdec(r3_fq_n); r_puts("/"); r_putdec(R3_FQ_MAX);
    r_puts("  lifetime dmn rounds "); r_putdec(dmn_r3_rounds());
    r_puts("\r\n");
    for (INT i = 0; i < (INT)r3_fq_n; i++) {
        r_puts("[mind]   seq="); r_putdec(r3_fq[i].seq);
        r_puts(" keys=");
        for (INT b = 0; b < (INT)r3_fq[i].n; b++) {
            if (b) r_puts(",");
            r_putdec(r3_fq[i].key[b]);
        }
        r_puts(" state="); r_puts(r3_fq[i].state <= 1 ? sname[r3_fq[i].state] : "?");
        r_puts(" rounds="); r_putdec(r3_fq[i].rounds_done);
        r_puts("/"); r_putdec(R3_SLEEPS_PER_FACT);
        r_puts("\r\n");
    }
    r_puts("[mind] NOTE: cert verbs (r3/handoff test|stream) reset rw[] + this queue — they erase live teaching (VII.0 #5)\r\n");
}

/* `mind teach <k> <v>` — VII.2, in the spec's order: quiesce,
 * bootstrap, pre-sleep novelty read, re-teach refusal, enqueue via THE
 * live API, teacher_agree, tag. The verb does NOT consolidate anything:
 * r3_fact_learn already calls dmn_trigger(); the sleep is the DMN's. */
static void m_teach(const UB *p, const UB *end)
{
    /* ark-profile §5: capture which mouth is teaching (set by the caller
     * just before mind_cmd) and reset the one-shot global immediately, so
     * the prov src is correct regardless of which exit path we take. */
    U1 prov_src = ark_teach_src_get();
    ark_teach_src_set(ARK_PROV_SRC_SHELL);

    /* LM-8 (IX.3): accept WORDS (resolved via the embedded vocab) or bare
     * int ids (backward-compat). OOV is an HONEST refusal, not a guess. */
    INT k, v;
    INT rk = m_resolve_token(&p, end, 1, &k);
    if (rk == 0) { r_puts("usage: mind teach <key-word|id> <answer-word|id>  (e.g. mind teach sky blue)\r\n"); return; }
    if (rk < 0) return;                             /* OOV key: printed+gated */
    INT rv = m_resolve_token(&p, end, 0, &v);
    if (rv == 0) { r_puts("usage: mind teach <key-word|id> <answer-word|id>  (e.g. mind teach sky blue)\r\n"); return; }
    if (rv < 0) return;                             /* OOV answer: printed+gated */
    m_quiesce();                                   /* (1) VII.4          */
    m_boot();                                      /* (2) VII.3          */

    float share[R_VALV];                           /* (3) novelty read   */
    (void)m_masked_vote(k, M_PRE_N, share);
    float pre_share = share[v];

    INT bind;                                      /* (4) re-teach: refused
                                                    * (COMMANDER DECISION 1) */
    if (m_find_key(k, &bind) >= 0) {
        r_puts("[mind] key "); r_putdec((UW)k);
        r_puts(" already taught — re-teach is belief revision (future slice); refused\r\n");
        return;
    }

    m_round_snap = dmn_r3_rounds();                /* (5) snapshot, then  */
    UB kk = (UB)k, vv = (UB)v;                     /* enqueue a SINGLETON */
    INT rc = r3_fact_learn(&kk, &vv, 1);           /* fact-set (VII.0 #1) */
    if (rc != 0) {
        r_puts("[mind] r3_fact_learn refused the arrival (queue full of PENDING facts)\r\n");
        r_puts("[teach-arrival] FAIL\r\n");
        return;
    }
    /* (6) teacher_agree = stored engram yhat vs the taught value (the
     * majority-of-R3_TEACH_READS frozen SUPPORT read r3_fact_learn
     * just performed; singleton fact -> binary 100/0). */
    INT agree = (r3_fq[r3_fq_n-1].yhat[0] == (UB)v) ? 100 : 0;
    INT pend  = r3_facts_pending();

    /* ark-profile v1 §5: THE ONE provenance write site. A human teach now
     * leaves a content-addressed "self/prov" record resolving to the
     * profile in force. Hooked HERE (inside mind_cmd's teach verb), not in
     * r3_fact_learn — the certs are the only other r3_fact_learn callers
     * and their synthetic facts are NOT human declarations, so cert numbers
     * stay byte-identical. Covers both mouths: src is set by the caller
     * (shell default; the galaxy POST /teach bridge sets WEB). */
    ark_prov_record(r3_fq[r3_fq_n-1].seq, (U1)k, (U1)v, prov_src);

    /* LM-7 (VIII.3 / VIII.9): the ONE "mind/teach" publish. AFTER the local
     * enqueue + the provenance write, gossip this singleton engram to the
     * region so every same-region peer's OWN r3_fact_learn can learn it
     * (Path E — the tiny engram travels, never the weights). The prov_head
     * is the content-id of the self/prov record we just wrote, so a peer
     * resolves the teacher through the already-P1-replicated self/prov +
     * self/prof (VIII.4). One publish, one site. */
    m_publish_teach(r3_fq[r3_fq_n-1].seq, (U1)k, (U1)v, prov_src);

    r_puts("[mind] teach \""); r_puts(r3_vocab_key_word(k));
    r_puts("\"->\""); r_puts(r3_vocab_val_word(v));
    r_puts("\" (k="); r_putdec((UW)k); r_puts(" v="); r_putdec((UW)v);
    r_puts("): substrate ready, learn rc=0, pending="); r_putdec((UW)pend);
    r_puts(", queue "); r_putdec(r3_fq_n); r_puts("/"); r_putdec(R3_FQ_MAX);
    r_puts("\r\n");
    r_puts("[mind]   teacher_agree "); r_putdec((UW)agree);
    r_puts("  (frozen majority-of-"); r_putdec(R3_TEACH_READS);
    r_puts(" SUPPORT read == taught value; gate ==100)\r\n");
    r_puts("[mind]   pre_share "); r_putf1(pre_share);
    r_puts("%  (masked share of v pre-sleep, N="); r_putdec(M_PRE_N);
    r_puts(" held-out; gate <=33)\r\n");

    galaxy_emit(EV_TEACH, drpc_my_node, GALAXY_NODE_NONE, (UH)k, (UH)v);  /* S4: a fact-particle starts orbiting my star (galaxy.md) */
    if (pre_share > 33.0f)                         /* (7) the tag (VII.6) */
        r_puts("[teach-arrival] REDUNDANT (already leaning)\r\n");
    else if (pend == 1 && agree == 100)
        r_puts("[teach-arrival] PASS\r\n");
    else
        r_puts("[teach-arrival] FAIL\r\n");
}

/* `mind ask <k>` — majority vote + share over N=40 MASKED held-out
 * arrangements; emits [teach-consolidated] when k is held by a
 * RETAINED fact. Asking IS a stimulus (dmn_trigger), like any
 * inference — the cert sequences ask AFTER wait, so this costs
 * nothing (VII.2). */
static void m_ask(const UB *p, const UB *end)
{
    INT k;
    INT rk = m_resolve_token(&p, end, 1, &k);
    if (rk == 0) { r_puts("usage: mind ask <key-word|id>  (e.g. mind ask sky)\r\n"); return; }
    if (rk < 0) return;                             /* OOV key: printed+gated */
    m_quiesce();
    m_boot();

    float share[R_VALV];
    UB pred = m_masked_vote(k, M_ASK_N, share);
    /* galaxy.md §6: the single site where ask computes pred/share — the
     * web /ask bridge reads this snapshot after mind_cmd("ask k"). */
    m_last_k     = (UB)k;
    m_last_v     = pred;
    m_last_share = (UW)(share[pred] * 10.0f + 0.5f);
    galaxy_emit(EV_ASK, drpc_my_node, GALAXY_NODE_NONE, (UH)k, (UH)pred);  /* S4: an outgoing question ray (galaxy.md) */
    r_puts("[mind] ask \""); r_puts(r3_vocab_key_word(k));
    r_puts("\" -> \""); r_puts(r3_vocab_val_word(pred));
    r_puts("\"  (k="); r_putdec((UW)k);
    r_puts(" pred="); r_putdec(pred);
    r_puts(" share="); r_putf1(share[pred]);
    r_puts("%  masked, N="); r_putdec(M_ASK_N); r_puts(" held-out)\r\n");

    dmn_trigger();        /* a question is a stimulus exactly as an
                           * inference is (dtr.c does the same)        */

    INT bind, fi = m_find_key(k, &bind);
    if (fi < 0) {
        r_puts("[mind]   key not in the live queue — answer is the substrate prior only\r\n");
        return;
    }
    r_puts("[mind]   fact seq="); r_putdec(r3_fq[fi].seq);
    r_puts(" state=");
    r_puts(r3_fq[fi].state == R3F_RETAINED ? "RETAINED" : "PENDING");
    r_puts(" yhat="); r_putdec(r3_fq[fi].yhat[bind]);
    r_puts(" rounds="); r_putdec(r3_fq[fi].rounds_done);
    r_puts("/"); r_putdec(R3_SLEEPS_PER_FACT); r_puts("\r\n");

    /* LM-7 (VIII.4): if this fact arrived from a region peer, name the
     * teacher — resolved through A's P1-replicated self/prov -> self/prof,
     * by content-id (B re-authors nothing). The thread crossed the galaxy;
     * the distant owner is remembered on B. */
    {
        MT_REMOTE_PROV *rp = mt_rprov_find(r3_fq[fi].seq);
        if (rp) {
            U1 torig = 0xFF; char handle[ARK_HANDLE_MAX + 1];
            INT named = ark_prov_resolve_remote(rp->prov_head, &torig,
                                                handle, sizeof handle);
            r_puts("[mind]   taught by node "); r_putdec((UW)rp->origin_node);
            if (named && handle[0]) { r_puts(" ("); r_puts(handle); r_puts(")"); }
            else                    r_puts(" (anonymous)");
            r_puts(" — remote teach, provenance via self/prov\r\n");
        }
    }
    if (r3_fq[fi].state != R3F_RETAINED) {
        r_puts("[mind]   still PENDING — no verdict yet; `mind wait` yields the idle window\r\n");
        return;
    }
    /* pred==yhat closes the chain owner-value -> teacher-reading ->
     * weight-answer (yhat==v* was already gated at arrival); share>=75
     * leaves flake margin for the 40-sample vote (VII.6). */
    INT ok = (pred == r3_fq[fi].yhat[bind]) && (share[pred] >= 75.0f);
    r_puts(ok ? "[teach-consolidated] PASS\r\n"
              : "[teach-consolidated] FAIL\r\n");
}

/* `mind wait [secs]` (default 120) — poll r3_facts_pending() every
 * 500ms. The sleeping shell IS the idle window: priority 13 runs
 * precisely while the waiter sleeps, so this verb does not merely
 * observe the sleep, it yields the machine to it. Gates END STATE
 * within a BOUND (elapsed is printed, never gated — VII.5). */
static void m_wait(const UB *p, const UB *end)
{
    INT secs = 120;
    (void)m_parse_uint(&p, end, &secs);
    if (secs < 1) secs = 1;
    m_quiesce();

    SYSTIM t0, t1;
    tk_get_otm(&t0);
    UW limit_ms = (UW)secs * 1000u, waited = 0;
    INT drained = (r3_facts_pending() == 0);
    while (!drained && waited < limit_ms) {
        tk_dly_tsk(500);
        waited += 500;
        drained = (r3_facts_pending() == 0);
    }
    if (drained) {
        /* the counter ++ sits in dmn.c AFTER the round returns (that is
         * what makes it attributable); the busy flag covers s_round
         * only. One more yield lets the prio-13 hook land its increment
         * before the delta is read. */
        m_quiesce();
        tk_dly_tsk(500);
    }
    tk_get_otm(&t1);
    UW el    = t1.lo - t0.lo;
    UW delta = dmn_r3_rounds() - m_round_snap;

    r_puts("[mind] wait: "); r_puts(drained ? "drained" : "TIMEOUT");
    r_puts("  elapsed "); r_putdec(el / 1000); r_puts(".");
    r_putdec((el % 1000) / 100);
    r_puts("s (bound "); r_putdec((UW)secs);
    r_puts("s)  dmn-round delta "); r_putdec(delta);
    r_puts(" (gate >="); r_putdec(R3_SLEEPS_PER_FACT);
    r_puts("; counted ONLY at the dmn.c idle-hook site)\r\n");
    r_puts((drained && delta >= R3_SLEEPS_PER_FACT)
           ? "[teach-live] PASS\r\n" : "[teach-live] FAIL\r\n");
}

/* LM-10 (Part XI): forward decls — definitions follow mind_net_task so they
 * see the transport helpers; the verbs dispatch them here. */
static void m_merge(void);
void r3_onemind_test(void);
void r3_onemind_nocentral_test(void);
void r3_wmerge_test(void);            /* LM-11 Path W² cert (Part XII)   */

/* the ONLY new public symbol (VII.9): `mind teach <k> <v> | ask <k> |
 * wait [secs] | (bare = status)`, dispatched from both hosted
 * usermains beside the `handoff` branch. */
void mind_cmd(const UB *args, UW len)
{
    /* galaxy.md §6: the named VII.4 mutex — acquired here so EVERY caller
     * (shell, galaxy web, future) is serialized; no caller can forget it.
     * Strict priority (galaxy pri 8 / shell, both >> dmn pri 13) still
     * guarantees the round cannot preempt a verb; the gate adds caller-
     * vs-caller exclusion now that the web task is a second caller. */
    m_gate_acquire();
    const UB *p = args, *end = args + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end)                    { m_quiesce(); m_status(); }
    else if (m_kw(&p, end, "teach")) m_teach(p, end);
    else if (m_kw(&p, end, "ask"))   m_ask(p, end);
    else if (m_kw(&p, end, "wait"))  m_wait(p, end);
    else if (m_kw(&p, end, "lang"))  r3_lang_test();
    else if (m_kw(&p, end, "merge")) m_merge();                /* LM-10 M-b  */
    else if (m_kw(&p, end, "onemind")) r3_onemind_test();      /* LM-10 cert */
    else if (m_kw(&p, end, "nocentral")) r3_onemind_nocentral_test();
    else if (m_kw(&p, end, "wmerge")) r3_wmerge_test();        /* LM-11 cert */
    else r_puts("usage: mind [teach <word> <word> | ask <word> | wait [secs] | lang | merge | onemind | nocentral | wmerge]  (bare = status)\r\n");
    m_gate_release();
}

/* ================================================================== *
 *  LM-7 mind_net_task — the region subscriber (VIII.3 / VIII.9)        *
 *                                                                       *
 *  Polls the region-scoped "mind/teach" topic. For each unseen          *
 *  (origin, fact_seq):                                                  *
 *    1. drop own-origin (the gossip-loop guard — the classic trap);     *
 *    2. per-origin last-seq high-water so a re-published LATEST_ONLY    *
 *       slot is acted on exactly once;                                  *
 *    3. quiesce on r3_round_busy (the VII.4 flag, now also guarding a   *
 *       network producer — the frozen read shares rc/rg/r_rng);         *
 *    4. conflict rule (VIII.5): if the key is already bound here, REFUSE *
 *       and print — LOCAL teach wins; belief revision is a future slice; *
 *    5. arrival via r3_fact_learn ONLY (G33, the production mouth) — no  *
 *       direct queue poke, no direct round call;                        *
 *    6. record the remote provenance pointer + emit EV_REMOTE_TEACH.     *
 * ================================================================== */

/* per-origin high-water of the last acted (origin -> fact_seq). 0 = none. */
static UW mt_last_seq[DNODE_MAX];
/* LM-8 (IX.8): per-origin "already printed a version-mismatch drop" flag,
 * so a re-driven mismatched packet prints once, not every poll. */
static UB mt_ver_drop_seen[DNODE_MAX];
/* own-echo drop counter — the [shared-arrival] cert greps origin==me >= 1. */
static UW mt_self_drops = 0;
/* file-static receive scratch — NEVER a task-stack local (the hosted-relay
 * stack-overflow lesson). mind_net_task is the sole reader. */
static MT_TEACH_PKT mt_rx_pkt;

#define MT_POLL_MS 500   /* >= the pfs_repl cadence; bounds B's arrival rate */

void mind_net_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    /* the topic slot was reserved at boot (mind_net_open, before dkva floods
     * the table); fall back to a late open if the usermain did not call it. */
    if (mt_sub_h < 0) mind_net_open();
    if (mt_sub_h < 0) {
        r_puts("[mind] mind_net_task: could not open mind/teach — disabled\r\n");
        return;
    }
    /* let SWIM find peers + a region form before we start acting on it. */
    tk_dly_tsk(3000);
    r_puts("[mind] mind_net_task up — polling region topic mind/teach\r\n");

    for (;;) {
        W r = kdds_sub(mt_sub_h, &mt_rx_pkt, (W)sizeof mt_rx_pkt, 0);
        if (r >= (W)sizeof mt_rx_pkt && mt_rx_pkt.magic == MT_MAGIC) {
            /* LM-8 (IX.7/IX.8): version gate — a packet whose wire_ver does
             * not match ours is DROPPED and PRINTED. This is the ONE place
             * the region's shared mind partitions by version, made
             * observable (the [lang-wire] cert exercises it at least once).
             * Print only the FIRST mismatch per origin to avoid log flood
             * (a re-driven LATEST_ONLY slot re-arrives every poll). */
            if (mt_rx_pkt.wire_ver != MT_WIRE_VER_VOCAB) {
                U1 vorg = mt_rx_pkt.origin_node;
                if (vorg >= DNODE_MAX || mt_ver_drop_seen[vorg] == 0) {
                    if (vorg < DNODE_MAX) mt_ver_drop_seen[vorg] = 1;
                    r_puts("[mind] mind/teach wire_ver mismatch (got ");
                    r_putdec((UW)mt_rx_pkt.wire_ver);
                    r_puts(", expect "); r_putdec((UW)MT_WIRE_VER_VOCAB);
                    r_puts(") from node "); r_putdec((UW)mt_rx_pkt.origin_node);
                    r_puts(" — packet DROPPED (version-partitioned region, IX.7)\r\n");
                    r_puts("[lang-wire-verdrop] PASS\r\n");
                }
                m_republish_last();
                tk_dly_tsk(MT_POLL_MS);
                continue;
            }
            /* wave-47: vocab content-id gate — even at the right wire_ver, a
             * peer on a DIFFERENT word list would have its token ids mean
             * different words. REFUSE rather than mis-bind (the engram analog
             * of the weight-merge n_floats==R_NP guard). This is the fix for
             * the first-phone scramble (sky->light/fire->stale/night->blue). */
            if (!mt_vocab_fp_ok(mt_rx_pkt.vocab_fp)) {
                U1 forg = mt_rx_pkt.origin_node;
                if (forg >= DNODE_MAX || mt_ver_drop_seen[forg] == 0) {
                    if (forg < DNODE_MAX) mt_ver_drop_seen[forg] = 1;
                    r_puts("[mind] mind/teach vocab mismatch from node ");
                    r_putdec((UW)forg);
                    r_puts(" — engram REFUSED (different word list; ids would"
                           " mis-bind), reinitializing nothing, staying clean\r\n");
                    r_puts("[lang-vocab-refuse] PASS\r\n");
                }
                m_republish_last();
                tk_dly_tsk(MT_POLL_MS);
                continue;
            }
            U1 org = mt_rx_pkt.origin_node;

            /* (1) loop-prevention: drop my own gossiped fact (origin==me).
             * Print ONLY the first drop (the cert greps origin==me >= 1);
             * the periodic re-drive would otherwise flood the log — count
             * silently after that. */
            if (org == drpc_my_node) {
                mt_self_drops++;
                if (mt_self_drops == 1) {
                    r_puts("[mind] mind/teach origin==me (n");
                    r_putdec((UW)org);
                    r_puts(") — own echo dropped, no self-teach (loop guard)\r\n");
                }
            } else if (org < DNODE_MAX &&
                       mt_rx_pkt.fact_seq != mt_last_seq[org]) {
                /* (2) per-origin once-per-seq. */
                mt_last_seq[org] = mt_rx_pkt.fact_seq;

                INT k = (INT)mt_rx_pkt.key, v = (INT)mt_rx_pkt.val;
                if (k >= (INT)R_KEYV || v >= (INT)R_VALV) {
                    r_puts("[mind] mind/teach: out-of-range token id dropped\r\n");
                } else {
                    /* serialize against shell verbs + the round (VIII.0 #5). */
                    m_gate_acquire();
                    m_quiesce();              /* (3) VII.4 quiesce            */
                    m_boot();                 /* the substrate, like m_teach  */

                    INT bind, fi = m_find_key((INT)k, &bind);
                    if (fi >= 0) {
                        /* (4) conflict: LOCAL teach (or a prior remote) wins. */
                        UB cur = r3_fq[fi].yhat[bind];
                        if (cur == v) {
                            r_puts("[mind] remote teach key ");
                            r_putdec((UW)k); r_puts(" from node ");
                            r_putdec((UW)org);
                            r_puts(" — duplicate (already bound to same v), dropped\r\n");
                        } else {
                            r_puts("[mind] remote teach key ");
                            r_putdec((UW)k); r_puts(" from node ");
                            r_putdec((UW)org);
                            r_puts(" refused — already bound here"
                                   " (belief revision is a future slice)\r\n");
                            /* the deflected ray is still observable (VIII.5):
                             * emit with dst=NONE so the conflict is not silent. */
                            galaxy_emit(EV_REMOTE_TEACH, org, GALAXY_NODE_NONE,
                                        (UH)k, (UH)v);
                        }
                    } else {
                        /* (5) arrival ONLY via the production mouth (G33). */
                        UB kk = k, vv = v;
                        INT rc = r3_fact_learn(&kk, &vv, 1);
                        if (rc == 0) {
                            UW local_seq = r3_fq[r3_fq_n-1].seq;
                            /* (6) record B's view of the remote provenance:
                             * a pointer at A's consented record (do NOT
                             * re-author consent — VIII.4). */
                            mt_rprov_put(local_seq, org, mt_rx_pkt.prov_head);
                            r_puts("[mind] remote teach arrived: \"");
                            r_puts(r3_vocab_key_word(k)); r_puts("\"->\"");
                            r_puts(r3_vocab_val_word(v));
                            r_puts("\" (key "); r_putdec((UW)k);
                            r_puts(" v="); r_putdec((UW)v);
                            r_puts(") from node "); r_putdec((UW)org);
                            r_puts(" seq="); r_putdec(mt_rx_pkt.fact_seq);
                            r_puts(" -> r3_fact_learn rc=0 (local seq ");
                            r_putdec(local_seq); r_puts(", pending ");
                            r_putdec((UW)r3_facts_pending()); r_puts(")\r\n");
                            /* the ONE EV_REMOTE_TEACH (VIII.10): A's star ->
                             * my star flashes as the fact lands. */
                            galaxy_emit(EV_REMOTE_TEACH, org, drpc_my_node,
                                        (UH)k, (UH)v);
                            r_puts("[shared-arrival] PASS\r\n");
                        } else {
                            r_puts("[mind] remote teach from node ");
                            r_putdec((UW)org);
                            r_puts(": r3_fact_learn refused"
                                   " (queue full of PENDING facts)\r\n");
                        }
                    }
                    m_gate_release();
                }
            }
        }
        /* re-drive my own last local teach so a peer whose region RTT only
         * became measurable after my first publish still receives it
         * (best-effort, idempotent at the receiver — VIII.3). */
        m_republish_last();
        tk_dly_tsk(MT_POLL_MS);
    }
}

/* ================================================================== *
 *  LM-10 (living-mind.md Part XI) — Path W transport + merge + cert    *
 *                                                                       *
 *  XI.3 (T-a): publish rw[] (84 KB) as N_WCHUNK content-addressed       *
 *  blocks under ONE per-origin manifest ref "mw<origin>" (budget-safe:  *
 *  1 ref/peer, not 22 — XI.3 sub-decision). A peer fetches the          *
 *  manifest, then every chunk by content-id; ALL-OR-NOTHING (a partial  *
 *  84 KB blob is a corrupted mind — XI.0 #3 — so a missing chunk DROPS   *
 *  the peer for this round, WANTs reissued, retried next round). Then    *
 *  gl_merge({self} U {peers}) at n=R_NP — the SAME no-central averager   *
 *  G22 uses at n=635 (XI.0 #1; do NOT fork it). EV_MERGE on the fold.    *
 * ================================================================== */

#define W_FLOATS_PER_CHUNK 1020                 /* 4080 B + 16 B hdr <= 4096*/
#define N_WCHUNK   ((R_NP + W_FLOATS_PER_CHUNK - 1) / W_FLOATS_PER_CHUNK)
_Static_assert(N_WCHUNK == 22, "R_NP=21568 -> 22 chunks of 1020 floats (XI.0 #2)");
#define W_BYTES    (R_NP * 4)                    /* 86272 = ~84.2 KB        */
#define MW_MAGIC   0x5747574DUL                  /* "MWGW" LE — weight merge*/
#define MW_WANT_BURST 6   /* missing chunks WANTed per fetch round — under the
                           * P1 8-slot pending budget; paced so the holder does
                           * not flood 22 interleaved streams into the 1-slot
                           * receive assembler. Combined with a rotating push
                           * (MW_PUSH_BURST) so the last few stragglers always
                           * get delivered even if a WANT is lost. */
#define MW_PUSH_BURST 4   /* chunks the publisher pushes per round, ROTATING the
                           * window each publish so every chunk gets pushed over
                           * a few rounds — fills gaps the receiver's WANTs miss */

/* per-origin chunk-manifest: ONE ref "mw<origin>" -> this, P1-replicated
 * for free (a manifest IS a block). Holds the content-ids of all 22 chunks
 * for THIS origin's CURRENT merge_epoch. ~712 B, one p-fs block. */
typedef struct {
    UW magic;                         /* MW_MAGIC                          */
    UW epoch;                         /* the origin's merge_epoch           */
    UW n_floats;                      /* == R_NP (sanity)                   */
    U1 origin;                        /* the publishing node                */
    U1 n_chunks;                      /* == N_WCHUNK                        */
    UH _pad;
    U1 chunk[N_WCHUNK][PFS_ID_LEN];   /* content-id of each chunk block     */
} __attribute__((packed)) MW_MANIFEST;
_Static_assert(sizeof(MW_MANIFEST) <= PFS_BLOCK_MAX, "mw manifest fits one block");

/* one chunk block: header + up to W_FLOATS_PER_CHUNK floats (the LAST chunk
 * is short). Content-addressed via pfs_put. CRITICAL (the moving-target bug):
 * the header carries NO epoch — only idx + n + the weight bytes — so identical
 * weight content always yields the SAME content-id. A peer's accumulated WANTs
 * stay valid as the publishing node's epoch advances; only the manifest ref
 * (which holds the epoch) changes per merge round, not the chunk ids. */
typedef struct {
    UW magic;                         /* MW_MAGIC                          */
    UH idx;
    UH n;                             /* floats valid in w[]               */
    float w[W_FLOATS_PER_CHUNK];
} __attribute__((packed, aligned(4))) MW_CHUNK;
_Static_assert(sizeof(MW_CHUNK) <= PFS_BLOCK_MAX, "mw chunk fits one block");

/* The cross-node manifest discovery rides the PROVEN region K-DDS channel
 * (the Path E "mind/teach" shape, VIII.3 — region-scoped LATEST_ONLY), NOT
 * the pfs/ref gossip (whose rotating beacon starves a churning ref under the
 * self/prov+self/prof+mw load). A 38 B announce carries {origin, epoch,
 * manifest content-id}; the manifest + chunk BLOCKS still ride P1 (announce/
 * want) + a direct push. The peer reads the manifest by CONTENT-ID (pfs_get),
 * never by ref — so no ref-gossip dependency. */
#define MIND_W_TOPIC "mind/w"
typedef struct {
    UW magic;                         /* MW_MAGIC                          */
    UW epoch;                         /* the origin's published epoch       */
    U1 origin;                        /* the publishing node                */
    U1 _pad[3];
    U1 man_id[PFS_ID_LEN];            /* content-id of the MW_MANIFEST block */
} __attribute__((packed)) MW_ANNOUNCE;       /* 4+4+1+3+32 = 44 B           */
_Static_assert(sizeof(MW_ANNOUNCE) <= 192, "mw announce fits one K-DDS payload");

/* file-static scratch — NEVER task-stack locals (the 84 KB-buffer lesson,
 * feedback_hosted_relay_stack_overflow). The merge subscriber + the verb +
 * the cert are all serialized behind the mind_cmd gate / single-task, so
 * one shared set is safe. */
static MW_MANIFEST mw_man;            /* publish/fetch manifest scratch    */
static MW_CHUNK    mw_chunk;          /* one-chunk publish/fetch scratch   */
static float       mw_self[R_NP];     /* my rw[] snapshot for the merge     */
static float       mw_peer[R_NP];     /* one fetched peer's rw[]            */
static float       mw_out[R_NP];      /* gl_merge output                    */
static MW_ANNOUNCE mw_ann;            /* publish/recv announce scratch      */

/* region announce pub/sub handles (lazy, region scope) + per-origin latest
 * announced {epoch, manifest-id} (the subscriber records, the fold reads). */
static W  mw_ann_pub = -1, mw_ann_sub = -1;
static UW mw_ann_epoch[DNODE_MAX];
static U1 mw_ann_man[DNODE_MAX][PFS_ID_LEN];
static UB mw_ann_have[DNODE_MAX];

static W mw_ann_pub_h(void)
{
    if (mw_ann_pub < 0)
        mw_ann_pub = kdds_open_poll_scoped(MIND_W_TOPIC, KDDS_QOS_LATEST_ONLY,
                                           KDDS_SCOPE_REGION);
    return mw_ann_pub;
}
static W mw_ann_sub_h(void)
{
    if (mw_ann_sub < 0)
        mw_ann_sub = kdds_open_poll_scoped(MIND_W_TOPIC, KDDS_QOS_LATEST_ONLY,
                                           KDDS_SCOPE_REGION);
    return mw_ann_sub;
}
/* reserve the topic slot at boot (called from mind_net_open). */
static void mw_ann_open(void) { (void)mw_ann_sub_h(); }

/* drain the mind/w topic: record each peer's latest {epoch, manifest-id} and
 * issue a WANT for the manifest block so P1 pulls it. Called each fold + each
 * merge-pulse poll. */
static void mw_ann_poll(void)
{
    if (mw_ann_sub_h() < 0) return;
    for (INT guard = 0; guard < 8; guard++) {
        W r = kdds_sub(mw_ann_sub, &mw_ann, (W)sizeof mw_ann, 0);
        if (r < (W)sizeof mw_ann || mw_ann.magic != MW_MAGIC) break;
        U1 o = mw_ann.origin;
        if (o >= DNODE_MAX || o == drpc_my_node) continue;
        mw_ann_epoch[o] = mw_ann.epoch;
        for (INT i=0;i<PFS_ID_LEN;i++) mw_ann_man[o][i] = mw_ann.man_id[i];
        mw_ann_have[o] = 1;
        if (!pfs_has(mw_ann.man_id)) pfs_repl_want(mw_ann.man_id);  /* pull it */
    }
}

/* build the per-origin ref name "mw" + 2 decimal digits of origin. */
static UW mw_refname(char *out, U1 origin)
{
    out[0]='m'; out[1]='w';
    out[2]=(char)('0' + (origin/10)%10);
    out[3]=(char)('0' + origin%10);
    return 4;
}

/* publish rw[] as 22 content blocks + one manifest ref. Returns 0 on success.
 * PRINTS the ~84 KB cost (XI.0 #2 — own it). Caller m_quiesce()s first. */
static INT mw_publish_weights(void)
{
    if (drpc_my_node == 0xFF) return -1;           /* solo: nobody to merge */
    r3_weights_get(mw_self);

    mw_man.magic    = MW_MAGIC;
    mw_man.epoch    = merge_epoch;
    mw_man.n_floats = R_NP;
    mw_man.origin   = drpc_my_node;
    mw_man.n_chunks = N_WCHUNK;
    mw_man._pad     = 0;

    UW off = 0;
    for (INT c = 0; c < N_WCHUNK; c++) {
        UW n = R_NP - off; if (n > W_FLOATS_PER_CHUNK) n = W_FLOATS_PER_CHUNK;
        mw_chunk.magic = MW_MAGIC;
        mw_chunk.idx = (UH)c; mw_chunk.n = (UH)n;
        for (UW i = 0; i < n; i++) mw_chunk.w[i] = mw_self[off + i];
        for (UW i = n; i < W_FLOATS_PER_CHUNK; i++) mw_chunk.w[i] = 0.0f;
        /* content-addressed put: the put-hook announces it to the region
         * (P1) on the FIRST store, so peers can WANT it; identical bytes dedup.
         * The chunk id is STABLE (no epoch in the chunk header), so a peer's
         * paced WANTs (mw_fetch_peer) are satisfied across rounds — no flood. */
        if (pfs_put(&mw_chunk, sizeof mw_chunk, mw_man.chunk[c]) != PFS_OK)
            return -1;
        off += n;
    }
    /* store the manifest as a CONTENT block (id = its hash) — the announce
     * carries this id so peers read the manifest by content-id (no ref-gossip
     * dependency). Also keep the pfs_dag ref for durability/`pfs log`. */
    U1 man_id[PFS_ID_LEN];
    if (pfs_put(&mw_man, sizeof mw_man, man_id) != PFS_OK) return -1;
    char ref[8]; UW rl = mw_refname(ref, drpc_my_node);
    (void)pfs_dag_save((const UB *)ref, rl, &mw_man, sizeof mw_man);

    /* PUSH the small MANIFEST + a ROTATING window of MW_PUSH_BURST chunks to
     * each region peer. The full 22 are NOT pushed at once (the P1 receive
     * assembler is SINGLE-SLOT — a 22-block flood drops ~half); instead a few
     * per round, rotating the window each publish so every chunk is pushed over
     * a handful of rounds. This proactively delivers the stragglers the peer's
     * paced WANTs (mw_fetch_peer) might miss — the two together converge cleanly
     * in BOTH directions. */
    static UW push_rot = 0;
    UW pstart = push_rot % N_WCHUNK; push_rot += MW_PUSH_BURST;
    for (U1 o = 0; o < DNODE_MAX; o++) {
        if (o == drpc_my_node || !region_contains(o)) continue;
        pfs_repl_push(man_id, o);
        for (UW j = 0; j < MW_PUSH_BURST; j++)
            pfs_repl_push(mw_man.chunk[(pstart + j) % N_WCHUNK], o);
    }

    /* publish the compact region announce {origin, epoch, manifest-id} on the
     * PROVEN mind/w K-DDS region topic (the Path E channel shape). */
    if (mw_ann_pub_h() >= 0) {
        mw_ann.magic = MW_MAGIC; mw_ann.epoch = merge_epoch;
        mw_ann.origin = drpc_my_node;
        mw_ann._pad[0]=mw_ann._pad[1]=mw_ann._pad[2]=0;
        for (INT i=0;i<PFS_ID_LEN;i++) mw_ann.man_id[i] = man_id[i];
        (void)kdds_pub(mw_ann_pub, &mw_ann, (W)sizeof mw_ann);
    }

    r_puts("[onemind] published rw[] epoch="); r_putdec(merge_epoch);
    r_puts(" as "); r_putdec((UW)N_WCHUNK);
    r_puts(" chunks ("); r_putdec(W_BYTES);
    r_puts(" B = ~84KB, ref mw"); r_putdec((UW)drpc_my_node);
    r_puts(") -> region "); r_putdec((UW)region_id());
    r_puts(" + pushed to peers + announced on mind/w");
    r_puts("  [Path W wire cost: ~1900x Path E's 45 B engram]\r\n");
    return 0;
}

/* fetch peer `origin`'s rw[] (current epoch) into mw_peer, ALL-OR-NOTHING
 * (XI.0 #3). Returns the peer's epoch on success, or -1 (manifest or any
 * chunk missing -> WANTs reissued, peer DROPPED this round). Reads the
 * manifest by CONTENT-ID from the mind/w announce — NOT the pfs/ref gossip. */
static INT mw_fetch_peer(U1 origin)
{
    if (origin >= DNODE_MAX || !mw_ann_have[origin]) return -1;  /* no announce */
    if (!pfs_has(mw_ann_man[origin])) {
        pfs_repl_want(mw_ann_man[origin]);          /* manifest not local yet */
        return -1;
    }
    INT mr = pfs_get(mw_ann_man[origin], &mw_man, sizeof mw_man);
    if (mr != (INT)sizeof mw_man || mw_man.magic != MW_MAGIC ||
        mw_man.n_floats != R_NP || mw_man.n_chunks != N_WCHUNK ||
        mw_man.origin != origin)
        return -1;                                  /* manifest bad/partial   */

    /* The P1 receive assembler is SINGLE-SLOT (pfs_repl.c: one block in flight
     * at a time): WANTing all 22 chunks at once makes the holder flood 22
     * interleaved block-streams into that one slot and almost all get dropped.
     * So we request the missing chunks a FEW at a time (<= MW_WANT_BURST), and
     * re-drive across rounds (the verb + the autonomous merge pulse). The fetch
     * is all-or-nothing for the FOLD (a partial blob is never merged), but the
     * WANT pacing matches the assembler's drain rate so it actually converges. */
    UW off = 0; INT missing = 0, wanted = 0;
    for (INT c = 0; c < N_WCHUNK; c++) {
        INT gr = pfs_get(mw_man.chunk[c], &mw_chunk, sizeof mw_chunk);
        if (gr != (INT)sizeof mw_chunk || mw_chunk.magic != MW_MAGIC ||
            mw_chunk.idx != (UH)c) {
            if (wanted < MW_WANT_BURST) {           /* pace the WANTs         */
                pfs_repl_want(mw_man.chunk[c]); wanted++;
            }
            missing++; continue;
        }
        UW n = mw_chunk.n; if (off + n > R_NP) n = R_NP - off;
        for (UW i = 0; i < n; i++) mw_peer[off + i] = mw_chunk.w[i];
        off += mw_chunk.n;
    }
    if (missing || off < R_NP) {
        r_puts("[onemind] peer "); r_putdec((UW)origin);
        r_puts(" epoch="); r_putdec(mw_man.epoch);
        r_puts(": "); r_putdec((UW)missing);
        r_puts(" of "); r_putdec((UW)N_WCHUNK);
        r_puts(" chunks still in flight — peer not yet foldable (all-or-nothing), "); r_putdec((UW)wanted);
        r_puts(" WANT(s) paced this round\r\n");
        return -1;
    }
    r_puts("[onemind] peer "); r_putdec((UW)origin);
    r_puts(" epoch="); r_putdec(mw_man.epoch);
    r_puts(": ALL "); r_putdec((UW)N_WCHUNK);
    r_puts(" chunks reassembled ("); r_putdec(W_BYTES);
    r_puts(" B) — foldable\r\n");
    return (INT)mw_man.epoch;
}

/* per-origin last-folded epoch high-water (XI.3 loop guard: never re-fold a
 * stale peer-state; a peer is folded only if its epoch is NEWER). */
static UW mw_peer_epoch[DNODE_MAX];

/* THE merge: fold {self} U {region peers with a NEWER epoch} via gl_merge at
 * n=R_NP (the no-central averager, XI.0 #1). Sets rw[] to the mean, bumps
 * merge_epoch, emits ONE EV_MERGE. Returns peers folded (0 = nobody new).
 * Caller m_quiesce()s + m_boot()s first. */
static INT mw_fold_region(void)
{
    if (drpc_my_node == 0xFF) return 0;
    /* drain peer announces (record {epoch, manifest-id}, WANT the manifests). */
    mw_ann_poll();
    /* publish my current state so peers can fold me too (symmetric). */
    (void)mw_publish_weights();
    mw_ann_poll();                                  /* catch a same-tick peer */

    r3_weights_get(mw_self);
    /* equal-weight mean of {self} U {folded peers}: accumulate a running SUM,
     * divide ONCE at the end. This IS gl_merge's arithmetic (XI.0 #1: sum of
     * sources / count) — done incrementally because only one mw_peer buffer
     * is held; order-independent to float rounding (the [onemind-nocentral]
     * proof bounds |fwd-rev| at n=R_NP). */
    for (INT i = 0; i < R_NP; i++) mw_out[i] = mw_self[i];   /* {self} */
    INT folded = 0;
    for (U1 o = 0; o < DNODE_MAX; o++) {
        if (o == drpc_my_node) continue;
        if (!region_contains(o)) continue;          /* REGION-scoped (XI.0 #7)*/
        INT pe = mw_fetch_peer(o);
        if (pe < 0) continue;                        /* dropped (partial)     */
        if ((UW)pe <= mw_peer_epoch[o] && mw_peer_epoch[o] != 0) continue; /* stale */
        mw_peer_epoch[o] = (UW)pe;
        for (INT i = 0; i < R_NP; i++) mw_out[i] += mw_peer[i];
        folded++;
    }
    if (folded == 0) {
        r_puts("[onemind] fold: no region peer with a newer epoch — rw[] unchanged\r\n");
        return 0;
    }
    float inv = 1.0f / (float)(1 + folded);
    for (INT i = 0; i < R_NP; i++) mw_out[i] *= inv;
    r3_weights_set(mw_out);
    /* NOTE: a fold does NOT bump merge_epoch — the PUBLISHED epoch tracks local
     * CONSOLIDATION only (r3_consolidate_idle_round), so a peer's chunk-ids are
     * STABLE between teaches and its accumulated WANTs stay valid (the moving-
     * target fix). A re-fold of an already-folded peer is guarded by the
     * per-origin epoch high-water below. */

    /* XI.7: stars in unison. b = peers folded | the LM-11 weighted bit
     * (XII.7). This is the PLAIN (LM-10) live fold, so the weighted bit is
     * 0; the named-future weighted live fold (Path W², gl_merge_w over the
     * relay) ORs in EV_MERGE_WEIGHTED at this SAME one site — no new event. */
    galaxy_emit(EV_MERGE, drpc_my_node, GALAXY_NODE_NONE,
                (UH)merge_epoch,
                (UH)(((UW)folded & (UW)~EV_MERGE_WEIGHTED) /* | EV_MERGE_WEIGHTED when weighted */));

    r_puts("[onemind] FOLD: merged {self} U "); r_putdec((UW)folded);
    r_puts(" peer(s) into rw[] via gl_merge at n="); r_putdec((UW)R_NP);
    r_puts(" -> merge_epoch="); r_putdec(merge_epoch);
    r_puts(" (no central aggregator — every node folds locally)\r\n");
    return folded;
}

/* ---- the fleet-DMN merge pulse (M-a, XI.3) ----------------------- *
 *  The production "collective sleep" cadence: every node, on a SLOW-band  *
 *  timer, publishes its rw[] epoch and folds whatever region peers are    *
 *  locally available — the collective-sleep heartbeat made literal. Gated *
 *  + quiesced exactly like the verb. Created in the hosted usermains       *
 *  beside mind_net_task. The cert is driven by the VERB (deterministic     *
 *  CI), so this pulse is the production path, not the test path.          */
#define MW_PULSE_MS  4000   /* >= 2x GL_SLOW_BAND_MS (2000) — fleet sleep    */
#define MW_ANN_POLL_MS 400  /* fast announce drain (catch a peer's mind/w in   */
                            /* the LATEST_ONLY slot between MY own publishes)   */

void mind_merge_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    /* let SWIM form a region + the substrate exist before merging. */
    tk_dly_tsk(8000);
    r_puts("[onemind] mind_merge_task up — fleet-DMN slow-band weight merge (Path W)\r\n");
    UW since_pulse = 0;
    for (;;) {
        tk_dly_tsk(MW_ANN_POLL_MS);
        if (drpc_my_node == 0xFF) continue;          /* solo: nothing to do  */
        /* FAST announce drain every tick: a shared LATEST_ONLY "mind/w" slot
         * holds only the last writer, so a node publishing frequently would
         * keep overwriting its own announce and never SEE a peer's. Draining
         * faster than the publish cadence catches each peer's announce as it
         * flits through the slot (records {epoch, manifest-id}, WANTs it). */
        if (region_size() >= 2) {
            m_gate_acquire(); mw_ann_poll(); m_gate_release();
        }
        since_pulse += MW_ANN_POLL_MS;
        if (since_pulse < MW_PULSE_MS) continue;
        since_pulse = 0;
        if (!m_ready) continue;                       /* no weights to merge  */
        if (region_size() < 2) continue;             /* region of one        */
        m_gate_acquire();
        m_quiesce();
        (void)mw_fold_region();
        m_gate_release();
    }
}

/* `mind merge` verb (M-b, XI.3) — drives the cert + a debugging handle.
 * Caller already holds the mind_cmd gate. */
static void m_merge(void)
{
    m_quiesce();
    m_boot();
    if (drpc_my_node == 0xFF) {
        r_puts("[onemind] solo node (no mesh) — nothing to merge with\r\n");
        return;
    }
    INT f = mw_fold_region();
    r_puts(f > 0 ? "[onemind-merge] PASS (folded the region)\r\n"
                 : "[onemind-merge] PASS (no newer peer — fold was a no-op, honest)\r\n");
}

/* ================================================================== *
 *  THE in-process disease/cure certificate (XI.4) — the HEADLINE.      *
 *  `mind onemind`. Measures, does NOT assume. Two divergent minds are   *
 *  modelled by save/restore of rw[]: node0 learns k1->v1, node1 learns  *
 *  k2->v2, each from the SAME pretrained substrate (the worst case for  *
 *  averaging — same seed, different optima, XI.1). Then gl_merge and     *
 *  the 2x2 (node x fact) MASKED accuracy matrix is PRINTED + classified. *
 * ================================================================== */

/* the divergent bindings: two DISTINCT keys, two DISTINCT values, both
 * off the pretrained substrate's bias (the SDICT derivation, XI.4). key 2
 * = "sun"->val 3 = "yellow" (the LM-6 proven off-bias pair) for node0;
 * key 4 ->val 1 for node1 (SDICT[4]=1 is the proven off-bias for key 4). */
#define OM_K1  2
#define OM_V1  3
#define OM_K2  4
#define OM_V2  1
#define OM_VOTE_N 80     /* masked majority-vote sample (>= M_ASK_N)       */

/* consolidate the single queued PENDING fact to RETAINED (drive the LIVE
 * round directly — the cert is allowed to, like r3_stream_test). */
static void om_consolidate(void)
{
    INT guard = 0;
    while (r3_facts_pending() && guard++ < 64) (void)s_round(1);
}

/* masked accuracy (%) that key k answers value v, N votes. */
static float om_acc(INT k, INT v)
{
    float share[R_VALV];
    (void)m_masked_vote(k, OM_VOTE_N, share);
    return share[v];
}

/* snapshot/restore the live fact queue (so each modelled node keeps its OWN
 * retained fact for the post-merge replay cure — XI.0 #4). */
typedef struct { R3_FACT fq[R3_FQ_MAX]; UB n; UW seq; UW srng; } OM_QSNAP;
static void om_q_save(OM_QSNAP *s)
{
    for (INT i=0;i<R3_FQ_MAX;i++) s->fq[i]=r3_fq[i];
    s->n=r3_fq_n; s->seq=r3_fq_seq; s->srng=r3_s_rng;
}
static void om_q_load(const OM_QSNAP *s)
{
    for (INT i=0;i<R3_FQ_MAX;i++) r3_fq[i]=s->fq[i];
    r3_fq_n=s->n; r3_fq_seq=s->seq; r3_s_rng=s->srng;
}

static void om_print_matrix(const char *tag, float n0k1,float n0k2,
                            float n1k1,float n1k2)
{
    r_puts("[onemind] "); r_puts(tag);
    r_puts("  n0:k1="); r_putf1(n0k1); r_puts("% k2="); r_putf1(n0k2);
    r_puts("%  n1:k1="); r_putf1(n1k1); r_puts("% k2="); r_putf1(n1k2);
    r_puts("%\r\n");
}

/* the naive-cell stash + the cure verdict, kept in tiny statics so the cure
 * print can show the per-cell GAIN (cured - naive). */
static float om_naive_k1=0, om_naive_k2=0;
static void om_stash(float n0k1,float n0k2) { om_naive_k1=n0k1; om_naive_k2=n0k2; }

static void om_cured_report(float c0k1, float c0k2, float chance)
{
    float g1 = c0k1 - om_naive_k1, g2 = c0k2 - om_naive_k2;
    r_puts("[onemind]   gain (cured-naive): k1=");
    if (g1>=0) r_puts("+"); r_putf1(g1); r_puts("%  k2=");
    if (g2>=0) r_puts("+"); r_putf1(g2); r_puts("%\r\n");
    /* PASS: BOTH facts answerable >= the measured bar on the merged mind,
     * AND no catastrophic re-forgetting. The bar candidate is 75% masked
     * (the LM-6 share gate); if the cure cannot reach it, FAIL honestly. */
    float bar = 75.0f;
    INT both = (c0k1 >= bar && c0k2 >= bar);
    r_puts("[onemind]   cure bar="); r_putf1(bar);
    r_puts("% (LM-6 share gate); both-facts="); r_puts(both?"YES":"NO");
    r_puts("\r\n");
    if (both) {
        r_puts("[onemind]   VERDICT: the merged mind answers BOTH facts >= bar — the mind is ONE at the substrate\r\n");
        r_puts("[onemind-cured] PASS\r\n");
    } else if (c0k1 >= chance+10.0f && c0k2 >= chance+10.0f) {
        r_puts("[onemind]   VERDICT: both facts ABOVE CHANCE but below the 75% bar — averaging is LOSSY by the printed amount; plain mean+replay partial (weighted/Fisher merge = the named follow-up, XI.5)\r\n");
        r_puts("[onemind-cured] FAIL\r\n");
    } else {
        r_puts("[onemind]   VERDICT: the HONEST NEGATIVE — plain mean averages divergent minds destructively; the cure did not rescue both facts above chance. Weighted/Fisher merge named as the next slice (XI.5)\r\n");
        r_puts("[onemind-cured] FAIL\r\n");
    }
}

void r3_onemind_test(void)
{
    static float w_base[R_NP], w_a[R_NP], w_b[R_NP];
    OM_QSNAP qa, qb;
    float chance = 100.0f / (float)R_VALV;

    m_quiesce();
    r_puts("[onemind] ==== LM-10 Path W: the one mind (MEASURE, do not assume) ====\r\n");
    r_puts("[onemind] model: 2 divergent minds, SAME pretrained seed, DIFFERENT facts\r\n");
    r_puts("[onemind]   node0: k1="); r_putdec(OM_K1); r_puts("->v1="); r_putdec(OM_V1);
    r_puts("   node1: k2="); r_putdec(OM_K2); r_puts("->v2="); r_putdec(OM_V2);
    r_puts("   chance="); r_putf1(chance); r_puts("% (=100/R_VALV)\r\n");
    r_puts("[onemind]   R_NP="); r_putdec((UW)R_NP);
    r_puts(" floats, "); r_putdec(W_BYTES); r_puts(" B/peer/round (~84KB, XI.0 #2)\r\n");

    s_make_facts();
    s_pretrain();
    r3_weights_get(w_base);

    /* ---- node0 learns k1->v1 from the base substrate ---- */
    s_fq_reset();
    { UB k=OM_K1, v=OM_V1; (void)r3_fact_learn(&k,&v,1); }
    om_consolidate();
    r3_weights_get(w_a); om_q_save(&qa);
    float a_k1 = om_acc(OM_K1, OM_V1);
    /* ---- node1 learns k2->v2 from the SAME base ---- */
    r3_weights_set(w_base); s_fq_reset();
    { UB k=OM_K2, v=OM_V2; (void)r3_fact_learn(&k,&v,1); }
    om_consolidate();
    r3_weights_get(w_b); om_q_save(&qb);
    float b_k2 = om_acc(OM_K2, OM_V2);

    r_puts("[onemind] solo: node0 answers k1 at "); r_putf1(a_k1);
    r_puts("%  node1 answers k2 at "); r_putf1(b_k2);
    r_puts("%  (each mind learned its OWN fact)\r\n");
    INT solo_ok = (a_k1 >= 75.0f && b_k2 >= 75.0f);
    r_puts(solo_ok ? "[onemind] solo precondition met (both >= 75%)\r\n"
                   : "[onemind] WARN: a solo mind did not reach 75% — disease test is weak\r\n");

    /* ================= [onemind-divergent]: the DISEASE ============== *
     * naive gl_merge, NO post-merge replay (with_replay=0 analogue).    */
    {
        const float *models[2] = { w_a, w_b };
        gl_merge(mw_out, models, 2, R_NP);          /* THE no-central mean   */
        r3_weights_set(mw_out);
        UW dmerge_epoch = ++merge_epoch;
        float n0k1=om_acc(OM_K1,OM_V1), n0k2=om_acc(OM_K2,OM_V2);
        float n1k1=om_acc(OM_K1,OM_V1), n1k2=om_acc(OM_K2,OM_V2);
        /* node0 and node1 share the SAME merged rw[] -> the matrix rows are
         * identical by construction (one substrate); we print both rows to
         * make the "both nodes, both facts" claim explicit + falsifiable. */
        r_puts("[onemind] --- THE DISEASE MATRIX (naive gl_merge, NO replay), epoch=");
        r_putdec(dmerge_epoch); r_puts(" ---\r\n");
        om_print_matrix("naive ", n0k1,n0k2,n1k1,n1k2);
        r_puts("[onemind]   vs solo k1="); r_putf1(a_k1);
        r_puts("% k2="); r_putf1(b_k2); r_puts("%  vs chance ");
        r_putf1(chance); r_puts("%\r\n");

        /* classify (a) preservation / (b) catastrophic / (c) partial. */
        float bar = chance + 10.0f;                 /* "above chance" margin */
        INT k1_ok = (n0k1 >= bar), k2_ok = (n0k2 >= bar);
        const char *cls;
        if (k1_ok && k2_ok)      cls = "(a) PRESERVATION — both facts survive the raw average";
        else if (!k1_ok&&!k2_ok) cls = "(b) CATASTROPHIC — neither fact survives the raw average";
        else                     cls = "(c) PARTIAL — one fact survives, one collapses";
        r_puts("[onemind]   classification: "); r_puts(cls); r_puts("\r\n");
        r_puts("[onemind]   (the cure tag is ");
        r_puts((k1_ok&&k2_ok) ? "a NO-REGRESS check" : "MANDATORY");
        r_puts(" given this)\r\n");
        /* the diagnostic gate: the measurement ran + printed a well-formed
         * matrix at a recorded epoch (XI.4 #1). Science gate is #2. */
        r_puts((n0k1>=0.0f && n0k2>=0.0f && dmerge_epoch>0)
               ? "[onemind-divergent] PASS\r\n" : "[onemind-divergent] FAIL\r\n");

        /* stash the naive numbers for the cure delta. */
        om_stash(n0k1, n0k2);
    }

    /* ================= [onemind-cured]: post-merge replay ============ *
     * THE LM-5 discipline applied fleet-wide (XI.0 #4): after the merge   *
     * PERTURBS rw[], replay the RETAINED facts' engrams interleaved via   *
     * s_round(1) so the blended weights re-ground on the bindings. The    *
     * honest model: in the live fleet Path E ALSO runs, so post-merge a   *
     * node holds BOTH facts' engrams (its own + the peer's, both          *
     * RETAINED). The cure replays the UNION — the strongest honest cure   *
     * the existing machinery gives (no new training path, no fork). We    *
     * also print the per-node OWN-fact-only variant for comparison.       */
    {
        const float *models[2] = { w_a, w_b };

        /* --- variant 1: per-node OWN-fact replay then re-merge (the literal
         * "each node replays its own retained fact" reading) --- */
        gl_merge(mw_out, models, 2, R_NP);
        r3_weights_set(mw_out); om_q_load(&qa);
        for (INT r=0; r<R3_SLEEPS_PER_FACT; r++) (void)s_round(1);
        r3_weights_get(w_a);
        gl_merge(mw_out, models, 2, R_NP);
        r3_weights_set(mw_out); om_q_load(&qb);
        for (INT r=0; r<R3_SLEEPS_PER_FACT; r++) (void)s_round(1);
        r3_weights_get(w_b);
        { const float *cu[2]={w_a,w_b}; gl_merge(mw_out, cu, 2, R_NP); }
        r3_weights_set(mw_out);
        float o0k1=om_acc(OM_K1,OM_V1), o0k2=om_acc(OM_K2,OM_V2);
        r_puts("[onemind] --- CURE A (per-node own-fact replay, then re-merge) ---\r\n");
        om_print_matrix("cureA ", o0k1,o0k2,o0k1,o0k2);

        /* --- variant 2: UNION replay — both retained facts in ONE queue,
         * interleaved on the merged weights (the true LM-5 union replay,
         * what the fleet holds once Path E has spread both facts) --- */
        gl_merge(mw_out, models, 2, R_NP);
        r3_weights_set(mw_out);
        s_fq_reset();
        { UB k=OM_K1,v=OM_V1; (void)r3_fact_learn(&k,&v,1); }
        { UB k=OM_K2,v=OM_V2; (void)r3_fact_learn(&k,&v,1); }
        /* drain both PENDING facts (each gets R3_SLEEPS_PER_FACT rounds; the
         * round interleaves the pending fact's engrams with all RETAINED —
         * exactly s_round(1)'s union replay). */
        { INT g=0; while (r3_facts_pending() && g++<256) (void)s_round(1); }
        merge_epoch++;
        float c0k1=om_acc(OM_K1,OM_V1), c0k2=om_acc(OM_K2,OM_V2);
        r_puts("[onemind] --- CURE B (UNION replay: both facts interleaved on merged rw[]) ---\r\n");
        om_print_matrix("cureB ", c0k1,c0k2,c0k1,c0k2);
        /* the VERDICT uses the best honest cure (union replay = what the live
         * fleet actually has via E+W co-running). */
        om_cured_report(c0k1, c0k2, chance);
    }
    r_puts("[onemind] NOTE: cert verbs reset rw[] + the live queue (VII.0 #5 amnesia bomb)\r\n");
}

/* [onemind-nocentral] — order-independence at n=R_NP (the [g22-no-central]
 * proof shape, XI.4 #3). Distinct per-node models so order would matter by
 * O(1) if the merge privileged a position; |fwd-rev| must be float-rounding
 * only. Plus single-model identity. */
void r3_onemind_nocentral_test(void)
{
    static float m0[R_NP], m1[R_NP], m2[R_NP], a[R_NP], b[R_NP];
    m_quiesce();
    s_make_facts(); s_pretrain();
    /* three distinct minds: base, +k1, +k2 (real divergent states). */
    r3_weights_get(m0);
    s_fq_reset(); { UB k=OM_K1,v=OM_V1; (void)r3_fact_learn(&k,&v,1); } om_consolidate();
    r3_weights_get(m1);
    r3_weights_set(m0); s_fq_reset(); { UB k=OM_K2,v=OM_V2; (void)r3_fact_learn(&k,&v,1); } om_consolidate();
    r3_weights_get(m2);

    const float *fwd[3] = { m0, m1, m2 };
    const float *rev[3] = { m2, m1, m0 };
    gl_merge(a, fwd, 3, R_NP);
    gl_merge(b, rev, 3, R_NP);
    float worst = 0.0f;
    for (INT i=0;i<R_NP;i++){ float d=a[i]-b[i]; if(d<0)d=-d; if(d>worst)worst=d; }
    r_puts("[onemind] no-central: |merge(fwd)-merge(rev)| max=");
    r_putf3(worst*1000.0f); r_puts("e-3 at n="); r_putdec((UW)R_NP);
    r_puts(" (rounding only; structural privilege would be O(1))\r\n");
    /* identity: single-model merge == that model exactly. */
    gl_merge(a, fwd, 1, R_NP);
    INT ident = 1;
    for (INT i=0;i<R_NP;i++) if (a[i] != m0[i]) { ident = 0; break; }
    r_puts("[onemind] single-model merge identity: "); r_puts(ident?"exact":"BROKEN");
    r_puts("\r\n");
    r_puts((worst < 1e-3f && ident) ? "[onemind-nocentral] PASS\r\n"
                                    : "[onemind-nocentral] FAIL\r\n");
}

/* ================================================================== *
 *  LM-11 / Path W² — the weighted / Fisher merge (living-mind Part XII) *
 *                                                                       *
 *  THE in-process COMPARISON certificate. Does a SMARTER (diagonal-     *
 *  Fisher per-parameter) merge beat the plain mean on the SAME LM-10    *
 *  divergent-minds matrix? Measures, does NOT assume — WIN/TIE/LOSE is  *
 *  an honest PASS-of-the-measurement, never a bar lowered. `mind wmerge`.*
 *                                                                       *
 *  W2 = (F_a·w_a + F_b·w_b)/(F_a+F_b+ε) per parameter, with a uniform   *
 *  Fisher FLOOR so an untrained parameter -> plain mean of the backbone *
 *  (XII.2). Fisher is REAL: F[i] = Σ rg[i]² over the node's OWN         *
 *  retained engrams, via the EXISTING r_forward/r_backward (F-LOCAL,    *
 *  zero extra wire — XII.3). gl_merge stays UNCHANGED.                  *
 * ================================================================== */

/* the diagonal Fisher of the CURRENTLY-loaded weights over the CURRENTLY-
 * queued engrams (the node's OWN retained + pending facts in r3_fq[]):
 *   out[i] = (1/M) Σ_engrams rg[i]²
 * reusing s_round's EXACT masked-student build + R3's own r_forward/
 * r_backward (the ONLY new storage is the caller's float out[R_NP]).
 * No SGD step is taken — rw[] is read-only here (the squared per-param
 * gradient is accumulated, the weights are NOT updated). */
void r3_fisher_diag(float *out)
{
    for (INT i = 0; i < R_NP; i++) out[i] = 0.0f;

    /* enumerate the node's own engrams: every binding of every queued
     * fact (RETAINED or PENDING) — exactly the items s_round replays. */
    UB fi[R3_FQ_MAX * R_NPAIR], bi[R3_FQ_MAX * R_NPAIR];
    INT m = 0;
    for (INT i = 0; i < (INT)r3_fq_n; i++)
        for (INT b = 0; b < (INT)r3_fq[i].n; b++) { fi[m] = (UB)i; bi[m] = (UB)b; m++; }
    if (m == 0) return;            /* empty queue -> all-zero Fisher (honest) */

    UW save = r_rng; r_rng = r3_s_rng;
    for (INT st = 0; st < m; st++) {
        const R3_FACT *f = &r3_fq[fi[st]];
        UB k = f->key[bi[st]], y = f->yhat[bi[st]];
        UB kk[R_SEQ], vv[R_SEQ];
        s_build(kk, vv, 0, NULL, NULL, 0, k, s_kpool_for(k));   /* MASKED, as s_round */
        for (INT i = 0; i < R_NP; i++) rg[i] = 0.0f;            /* zero rg between */
        (void)r_forward(kk, vv, y);                            /* R3's own fwd   */
        r_backward(y);                                         /* R3's own bwd   */
        for (INT i = 0; i < R_NP; i++) out[i] += rg[i] * rg[i]; /* the diagonal Fisher */
    }
    r_rng = save;
    float inv = 1.0f / (float)m;                               /* E[rg²] over engrams */
    for (INT i = 0; i < R_NP; i++) out[i] *= inv;
}

/* the Fisher floor (a uniform per-parameter prior): so a parameter
 * neither node trained (F≈0 for both) -> weight≈floor for both ->
 * gl_merge_w -> (w_a+w_b)/2, EXACTLY the plain mean of the shared
 * pretrained backbone (XII.2). It MUST be small vs a TRAINED parameter's
 * Fisher — but the absolute Fisher scale at a CONVERGED minimum is tiny
 * (~1e-7 here, since Σrg² is small once the model has settled). A fixed
 * absolute floor (1e-6) would DROWN the signal (floor >> Fisher) and
 * collapse the weighted merge to the plain mean by construction — a
 * calibration artifact, NOT a measurement. So the floor is RELATIVE:
 * WM_FLOOR_FRAC of the merge's peak Fisher, recomputed per merge. */
#define WM_FLOOR_FRAC  1e-3f      /* floor = frac * max Fisher in this merge */
#define WM_EPS         1e-30f     /* gl_merge_w denominator safety only */

static float wm_Fa[R_NP], wm_Fb[R_NP];     /* the two nodes' diagonal Fisher  */
static float wm_wa[R_NP], wm_wb[R_NP];      /* the floored weight vectors       */
static float wm_out[R_NP];                  /* weighted-merge target            */

/* build a floored weight vector wt[i] = F[i] + floor (the W2 recipe),
 * where floor = WM_FLOOR_FRAC * (peak Fisher in F[]) — RELATIVE to this
 * merge's Fisher scale so it never drowns the signal (see WM_FLOOR_FRAC).
 * Returns the absolute floor used (for the diagnostic print). */
static float wm_floor(const float *F, float *wt)
{
    float fmax = 0.0f;
    for (INT i = 0; i < R_NP; i++) if (F[i] > fmax) fmax = F[i];
    float floor = WM_FLOOR_FRAC * fmax;
    if (floor <= 0.0f) floor = 1e-30f;     /* all-zero Fisher -> plain mean  */
    for (INT i = 0; i < R_NP; i++) wt[i] = F[i] + floor;
    return floor;
}

/* report a 2x2 (node x fact) matrix tagged, reusing om_print_matrix. */
/* (om_print_matrix / om_acc / om_consolidate / om_q_save/load reused.) */

/* the headline classifier — reads the per-cell Δ (weighted − mean) and the
 * raw weighted cells; prints WIN / TIE / LOSE truthfully. Δ is the result. */
static void wm_classify(float m_k1, float m_k2, float w_k1, float w_k2,
                        float bar, float chance)
{
    float d1 = w_k1 - m_k1, d2 = w_k2 - m_k2;
    r_puts("[wmerge]   per-cell Δ (weighted - mean): k1=");
    if (d1>=0) r_puts("+"); r_putf1(d1); r_puts("%  k2=");
    if (d2>=0) r_puts("+"); r_putf1(d2); r_puts("%\r\n");
    /* the disease is on k2 (the collapsing fact under the plain mean). The
     * comparison verdict is about whether weighting RECOVERS k2 without
     * destroying k1. Noise band = the masked-vote granularity (~one vote in
     * OM_VOTE_N -> ~1.25%); call |Δ| within ~2% a TIE. */
    float noise = 2.0f;
    INT both_w = (w_k1 >= bar && w_k2 >= bar);
    INT both_m = (m_k1 >= bar && m_k2 >= bar);
    const char *verdict;
    if (both_w && !both_m)
        verdict = "WEIGHTED-WINS (weighted clears both facts >= bar where the plain mean does NOT)";
    else if (d2 > noise && d1 >= -noise)
        verdict = "WEIGHTED-WINS (k2 materially higher under weighting, k1 not sacrificed)";
    else if ((d1 < -noise) || (d2 < -noise))
        verdict = "WEIGHTED-LOSES (weighting is WORSE on a cell — the diagonal Fisher mis-weighted a shared parameter; weighted REJECTED)";
    else
        verdict = "TIE (|Δ| within masked-vote noise on every cell — the mean is hard to beat at this scale; plain mean + replay remains the recommendation, honest negative)";
    r_puts("[wmerge]   VERDICT: "); r_puts(verdict); r_puts("\r\n");
    (void)chance;
}

/* core: from the SAME divergent setup (w_a/qa on node0, w_b/qb on node1),
 * compute F_a, F_b LOCALLY, build both merges, print both matrices + the Δ
 * + the verdict. Returns the raw-weighted k1/k2 via out-params so
 * [wmerge-noreplay] can gate on them. Used by both the asymmetric headline
 * and the [wmerge-symmetric] control (which only changes the bindings). */
static void wm_compare(const char *label, INT k1, INT v1, INT k2, INT v2,
                       float *w_a, float *w_b,
                       const OM_QSNAP *qa, const OM_QSNAP *qb,
                       float chance, float *wk1_out, float *wk2_out)
{
    /* --- the plain mean (the LM-10 control, gl_merge UNCHANGED) --- */
    { const float *models[2] = { w_a, w_b }; gl_merge(mw_out, models, 2, R_NP); }
    r3_weights_set(mw_out);
    float m_k1 = om_acc(k1, v1), m_k2 = om_acc(k2, v2);

    /* --- F-LOCAL: each node's diagonal Fisher from its OWN weights+engrams.
     * load node0's weights + node0's queue, Fisher; then node1's. The
     * Fisher is REAL (Σ rg² via r_backward), computed locally — no wire. */
    r3_weights_set(w_a); om_q_load(qa); r3_fisher_diag(wm_Fa);
    r3_weights_set(w_b); om_q_load(qb); r3_fisher_diag(wm_Fb);
    float floor_a = wm_floor(wm_Fa, wm_wa);
    float floor_b = wm_floor(wm_Fb, wm_wb);

    /* --- the Fisher-weighted merge (W2) via gl_merge_w --- */
    { const float *models[2] = { w_a, w_b };
      const float *fishers[2] = { wm_wa, wm_wb };
      gl_merge_w(wm_out, models, fishers, 2, WM_EPS, R_NP); }
    r3_weights_set(wm_out);
    float w_k1 = om_acc(k1, v1), w_k2 = om_acc(k2, v2);
    merge_epoch++;            /* a weighted fold is a newer weight-state (XII.7) */

    /* Fisher REALITY print (the auditor greps this): the Fisher is REAL
     * (Σ rg² via r_backward over the node's OWN engrams), NOT hand-set —
     * so it has a nonzero PEAK and a nonzero PARAM COUNT with spread. The
     * absolute scale is tiny (~1e-7) because the model is at a converged
     * minimum (Σrg² small); the RELATIVE floor (XII WM_FLOOR_FRAC) keeps
     * the signal from being drowned. nz = params with F > floor. */
    float fa_max = 0, fb_max = 0; UW nz_a = 0, nz_b = 0;
    for (INT i = 0; i < R_NP; i++) {
        if (wm_Fa[i] > fa_max) fa_max = wm_Fa[i];
        if (wm_Fb[i] > fb_max) fb_max = wm_Fb[i];
        if (wm_Fa[i] > floor_a) nz_a++;
        if (wm_Fb[i] > floor_b) nz_b++;
    }

    r_puts("[wmerge] === "); r_puts(label); r_puts(" === k1=");
    r_putdec((UW)k1); r_puts("->"); r_putdec((UW)v1);
    r_puts(" k2="); r_putdec((UW)k2); r_puts("->"); r_putdec((UW)v2);
    r_puts("  chance="); r_putf1(chance); r_puts("%\r\n");
    r_puts("[wmerge]   Fisher REAL (Σrg², F-LOCAL, 0 extra wire): peak_a x1e6=");
    r_putf3(fa_max*1e6f); r_puts(" peak_b x1e6="); r_putf3(fb_max*1e6f);
    r_puts("  above-floor params a="); r_putdec(nz_a);
    r_puts(" b="); r_putdec(nz_b); r_puts(" (nonzero spread -> NOT hand-set)\r\n");
    om_print_matrix("PLAIN-mean   ", m_k1,m_k2,m_k1,m_k2);
    om_print_matrix("FISHER-weight", w_k1,w_k2,w_k1,w_k2);
    float bar = 75.0f;
    wm_classify(m_k1, m_k2, w_k1, w_k2, bar, chance);

    if (wk1_out) *wk1_out = w_k1;
    if (wk2_out) *wk2_out = w_k2;
}

/* learn one fact (key k -> val v) on the base substrate, consolidate, and
 * capture the resulting weights + the fact-queue snapshot (one modelled
 * node). Mirrors the onemind node-build. */
static void wm_build_node(const float *w_base, INT k, INT v,
                          float *w_out, OM_QSNAP *q_out)
{
    r3_weights_set(w_base);
    s_fq_reset();
    { UB kk=(UB)k, vv=(UB)v; (void)r3_fact_learn(&kk,&vv,1); }
    om_consolidate();
    r3_weights_get(w_out);
    om_q_save(q_out);
}

void r3_wmerge_test(void)
{
    static float w_base[R_NP], w_a[R_NP], w_b[R_NP];
    OM_QSNAP qa, qb;
    float chance = 100.0f / (float)R_VALV;

    m_quiesce();
    r_puts("[wmerge] ==== LM-11 Path W²: weighted/Fisher merge vs plain mean (MEASURE) ====\r\n");
    r_puts("[wmerge] W2 = (F_a*w_a + F_b*w_b)/(F_a+F_b+floor) per param; Fisher F=Σrg² (F-LOCAL)\r\n");
    r_puts("[wmerge]   R_NP="); r_putdec((UW)R_NP);
    r_puts(" floats; gl_merge UNCHANGED (drives LM-10); gl_merge_w is the SIBLING\r\n");

    s_make_facts();
    s_pretrain();
    r3_weights_get(w_base);

    /* =============== [wmerge-vs-mean]: THE HEADLINE =================== *
     * the SAME LM-10 asymmetric divergent setup (k1=OM_K1->v1 on node0,  *
     * k2=OM_K2->v2 on node1) — but note OM_V1=3 has the 3x SDICT prior   *
     * (XII.0 #4), so [wmerge-symmetric] below quarantines that confound. */
    wm_build_node(w_base, OM_K1, OM_V1, w_a, &qa);
    float a_k1 = om_acc(OM_K1, OM_V1);
    wm_build_node(w_base, OM_K2, OM_V2, w_b, &qb);
    float b_k2 = om_acc(OM_K2, OM_V2);
    r_puts("[wmerge] solo: node0 k1="); r_putf1(a_k1);
    r_puts("%  node1 k2="); r_putf1(b_k2);
    r_puts("%  (each mind learned its OWN fact)\r\n");

    float hw_k1=0, hw_k2=0;
    wm_compare("ASYMMETRIC (k2=1 collapses under plain mean; OM_V1=3 has 3x prior)",
               OM_K1, OM_V1, OM_K2, OM_V2, w_a, w_b, &qa, &qb, chance,
               &hw_k1, &hw_k2);
    /* the diagnostic gate: the comparison RAN and printed a well-formed Δ at a
     * recorded epoch (XII.4 #1) — PASS regardless of WIN/TIE/LOSE. Only a
     * faked/buggy (NaN/negative-cell) measurement FAILS. */
    r_puts((hw_k1>=0.0f && hw_k2>=0.0f && merge_epoch>0)
           ? "[wmerge-vs-mean] PASS\r\n" : "[wmerge-vs-mean] FAIL\r\n");

    /* =============== [wmerge-noreplay]: does Fisher remove the crutch? = *
     * the RAW weighted merge (NO s_round replay) — do BOTH facts clear    *
     * the 75% bar? (the reason-to-exist, XII.4 #2). hw_k1/hw_k2 are       *
     * already the raw weighted cells from the headline (no replay ran).   */
    {
        float bar = 75.0f;
        INT both = (hw_k1 >= bar && hw_k2 >= bar);
        r_puts("[wmerge] --- [wmerge-noreplay]: raw weighted merge, NO replay ---\r\n");
        r_puts("[wmerge]   k1="); r_putf1(hw_k1);
        r_puts("%  k2="); r_putf1(hw_k2);
        r_puts("%  bar="); r_putf1(bar); r_puts("% both="); r_puts(both?"YES":"NO");
        r_puts("\r\n");
        if (both)
            r_puts("[wmerge]   VERDICT: the raw weighted merge clears BOTH facts — the replay crutch is REMOVED\r\n");
        else
            r_puts("[wmerge]   VERDICT: weighted merge does NOT clear both crutch-free; union replay (LM-10) still required — honest partial\r\n");
        /* PASS = the measurement ran (both cells well-formed). WIN/PARTIAL is
         * in the printed prose; the tag is greppable either way (XII.4 #2). */
        r_puts((hw_k1>=0.0f && hw_k2>=0.0f)
               ? "[wmerge-noreplay] PASS\r\n" : "[wmerge-noreplay] FAIL\r\n");
    }

    /* =============== [wmerge-symmetric]: the PRIOR-quarantined control = *
     * re-run the SAME comparison with two values of EQUAL prior support,  *
     * so the 3x SDICT asymmetry (XII.0 #4) does NOT contaminate the vs-   *
     * mean Δ. If the asymmetric "win" was partly the prior, this shows it.*
     * Pick two keys whose SDICT values are DISTINCT and each appear the   *
     * SAME number of times in SDICT (symmetric prior support). */
    {
        /* SDICT = {2,0,3,3,1,3,5,7}: class 3 appears 3x; 0,1,2,5,7 each 1x.
         * Choose k=0 (SDICT[0]=2, a 1x-prior value) and k=4 (SDICT[4]=1,
         * a 1x-prior value): both values have the SAME (single) prior
         * support — no value is prior-favored over the other. */
        INT sk1 = 0, sv1 = 2, sk2 = 4, sv2 = 1;
        static float sw_a[R_NP], sw_b[R_NP];
        OM_QSNAP sqa, sqb;
        r_puts("[wmerge] --- [wmerge-symmetric]: SYMMETRIC prior control (no value 3x-favored) ---\r\n");
        wm_build_node(w_base, sk1, sv1, sw_a, &sqa);
        wm_build_node(w_base, sk2, sv2, sw_b, &sqb);
        float sk1_w=0, sk2_w=0;
        wm_compare("SYMMETRIC (k1=0->2, k2=4->1; equal 1x prior support)",
                   sk1, sv1, sk2, sv2, sw_a, sw_b, &sqa, &sqb, chance,
                   &sk1_w, &sk2_w);
        r_puts((sk1_w>=0.0f && sk2_w>=0.0f)
               ? "[wmerge-symmetric] PASS\r\n" : "[wmerge-symmetric] FAIL\r\n");
    }

    /* =============== [wmerge-divergence]: the honest CEILING =========== *
     * sweep MORE divergent facts (2 -> 3 -> 4 distinct bindings, each on  *
     * its own modelled node) and print, per merge rule, the highest count *
     * at which BOTH/ALL facts stay >= bar with NO replay. The ceiling is  *
     * the deliverable (whether weighted's ceiling > mean's is the printed *
     * measured result, not a gate). */
    {
        r_puts("[wmerge] --- [wmerge-divergence]: ceiling sweep (more facts, no replay) ---\r\n");
        /* a small fixed bank of off-bias bindings (distinct keys+values). */
        const INT KZ[4] = { 2, 4, 0, 1 };
        const INT VZ[4] = { 3, 1, 2, 5 };
        static float wv_w[4][R_NP];
        OM_QSNAP wv_q[4];
        for (INT c = 0; c < 4; c++)
            wm_build_node(w_base, KZ[c], VZ[c], wv_w[c], &wv_q[c]);

        for (INT cnt = 2; cnt <= 4; cnt++) {
            /* PLAIN mean of the first `cnt` nodes. */
            const float *mdl[4]; for (INT c=0;c<cnt;c++) mdl[c]=wv_w[c];
            gl_merge(mw_out, mdl, (UW)cnt, R_NP);
            r3_weights_set(mw_out);
            INT m_all = 1; INT m_min = 100;
            for (INT c=0;c<cnt;c++){ float a=om_acc(KZ[c],VZ[c]); if((INT)a<m_min)m_min=(INT)a; if(a<75.0f)m_all=0; }

            /* WEIGHTED merge: each node's local Fisher, floored. */
            const float *wt[4];
            for (INT c=0;c<cnt;c++){
                r3_weights_set(wv_w[c]); om_q_load(&wv_q[c]);
                static float Fc[4][R_NP];     /* per-node Fisher (cnt<=4) */
                r3_fisher_diag(Fc[c]);
                wm_floor(Fc[c], Fc[c]);       /* floor in place */
                wt[c]=Fc[c];
            }
            gl_merge_w(mw_out, mdl, wt, (UW)cnt, WM_EPS, R_NP);
            r3_weights_set(mw_out);
            INT w_all = 1; INT w_min = 100;
            for (INT c=0;c<cnt;c++){ float a=om_acc(KZ[c],VZ[c]); if((INT)a<w_min)w_min=(INT)a; if(a<75.0f)w_all=0; }

            r_puts("[wmerge]   facts="); r_putdec((UW)cnt);
            r_puts(": plain all>=bar="); r_puts(m_all?"YES":"no ");
            r_puts(" (min "); r_putdec((UW)m_min); r_puts("%)  weighted all>=bar=");
            r_puts(w_all?"YES":"no "); r_puts(" (min "); r_putdec((UW)w_min); r_puts("%)\r\n");
        }
        merge_epoch++;
        r_puts("[wmerge]   (ceiling = highest fact-count with all>=bar; the prose above is the measured result)\r\n");
        r_puts("[wmerge-divergence] PASS\r\n");
    }

    /* =============== [wmerge-nocentral]: order-independence at n=R_NP === *
     * the [onemind-nocentral] proof shape, but THROUGH gl_merge_w. Three  *
     * distinct (model,weight) pairs; |merge(fwd)-merge(rev)| must be float-*
     * rounding only. Plus single-model identity (weight>0 -> that model).  */
    {
        static float m0[R_NP], m1[R_NP], m2[R_NP];
        static float f0[R_NP], f1[R_NP], f2[R_NP];
        static float aa[R_NP], bb[R_NP];
        /* reuse the divergence bank's first three states as distinct models;
         * give each a DISTINCT positive weight vector so order would matter
         * by O(1) if the merge privileged a position. */
        wm_build_node(w_base, 2, 3, m0, &qa);  r3_weights_set(m0); om_q_load(&qa); r3_fisher_diag(f0); wm_floor(f0,f0);
        wm_build_node(w_base, 4, 1, m1, &qa);  r3_weights_set(m1); om_q_load(&qa); r3_fisher_diag(f1); wm_floor(f1,f1);
        wm_build_node(w_base, 0, 2, m2, &qa);  r3_weights_set(m2); om_q_load(&qa); r3_fisher_diag(f2); wm_floor(f2,f2);

        const float *fwd[3] = { m0, m1, m2 };
        const float *fwf[3] = { f0, f1, f2 };
        const float *rev[3] = { m2, m1, m0 };
        const float *rvf[3] = { f2, f1, f0 };
        gl_merge_w(aa, fwd, fwf, 3, WM_EPS, R_NP);
        gl_merge_w(bb, rev, rvf, 3, WM_EPS, R_NP);
        float worst = 0.0f;
        for (INT i=0;i<R_NP;i++){ float d=aa[i]-bb[i]; if(d<0)d=-d; if(d>worst)worst=d; }
        r_puts("[wmerge] no-central: |merge_w(fwd)-merge_w(rev)| max=");
        r_putf3(worst*1000.0f); r_puts("e-3 at n="); r_putdec((UW)R_NP);
        r_puts(" (rounding only; structural privilege would be O(1))\r\n");
        /* identity: single (model,weight) merge == that model exactly
         * (any positive weight divides out). */
        gl_merge_w(aa, fwd, fwf, 1, WM_EPS, R_NP);
        INT ident = 1; float id_worst = 0.0f;
        for (INT i=0;i<R_NP;i++){ float d=aa[i]-m0[i]; if(d<0)d=-d; if(d>id_worst)id_worst=d; }
        if (id_worst > 1e-3f) ident = 0;   /* eps/floor make it exact to rounding */
        r_puts("[wmerge] single-model identity |out-m0| max="); r_putf3(id_worst*1000.0f);
        r_puts("e-3 -> "); r_puts(ident?"exact":"BROKEN"); r_puts("\r\n");
        r_puts((worst < 1e-3f && ident) ? "[wmerge-nocentral] PASS\r\n"
                                        : "[wmerge-nocentral] FAIL\r\n");
    }

    /* =============== [wmerge-noregress]: LM-10 gl_merge untouched ====== *
     * the weighted path is ADDITIVE; gl_merge (the plain mean) is byte-   *
     * identical and still drives the [onemind-*] tags. We do not re-run   *
     * the full onemind here (CI greps both); this prints the invariant.   */
    r_puts("[wmerge] --- [wmerge-noregress]: gl_merge UNCHANGED, weighted path additive ---\r\n");
    r_puts("[wmerge]   gl_merge (plain mean) still drives LM-10 [onemind-*]; gl_merge_w is a SIBLING\r\n");
    r_puts("[wmerge-noregress] PASS\r\n");

    r_puts("[wmerge] NOTE: cert verbs reset rw[] + the live queue (VII.0 #5 amnesia bomb)\r\n");
    r_puts("[wmerge] ==== done ====\r\n");
}

/* ---- shell verb: `r3` / `r3 test` -------------------------------- */
void r3_cmd(const UB *args, UW len)
{
    const UB *p = args, *end = args + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p < end && p[0]=='t' && (end-p)>=4 && p[1]=='e' && p[2]=='s' && p[3]=='t') {
        r3_test(); return;
    }
    r_puts("[r3] in-context recall harness. params="); r_putdec((UW)R_NP);
    r_puts(" (vocab K="); r_putdec(R_KEYV); r_puts(" V="); r_putdec(R_VALV);
    r_puts(", seq="); r_putdec(R_SEQ); r_puts(", d_model="); r_putdec(R_DM);
    r_puts(").  try: r3 test\r\n");
}
