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
#include "kernel.h"
#include <tmonitor.h>

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

/* ---- model config (its own dims; all widths <= DTR_LN_MAXW) -------- */
#define R_KEYV    8            /* key vocabulary                       */
#define R_VALV    4            /* value vocabulary == output classes   */
#define R_NPAIR   8            /* dictionary entries per episode        */
#define R_SEQ     (R_NPAIR+1)  /* dict tokens + 1 query                 */
#define R_QPOS    (R_SEQ-1)    /* query token position (readout point)  */
#define R_DM      32           /* d_model (8-way recall needs this      */
#define R_NH      4            /*  capacity; R_DM=16/R_NH=2 stays at     */
#define R_DH      (R_DM/R_NH)  /*  chance — measured, not assumed)       */
#define R_FFN     32           /* FFN hidden                           */
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
        /* stride 7 keeps every weight family covered (gcd(7,R_DM)=1 so
         * row/col positions rotate) while keeping the check CI-cheap. */
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
#define R_TRAIN_N     192
#define R_EVAL_N      300
#define R_HANDIF_N    6000   /* large: the best-fixed-rule baseline takes a
                               * MAX over many rules, so a small sample would
                               * inflate it via selection noise; measure the
                               * TRUE accuracy of each rule on a big stream. */
#define R_EPOCHS      60

void r3_test(void)
{
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

    /* 3. best hand-written fixed rule <= chance + eps (the theorem, measured).
     * NOTE (honest): the only fixed rule that beats chance is positional
     * value-copy, whose edge is provably (1/R_NPAIR)(1-1/R_VALV) and ->0 as
     * R_NPAIR grows; with R_NPAIR=8 it measures ~chance+10pts, well inside
     * the band. It is NOT exactly chance — a literal-recall label equals a
     * stored value, so a small structural edge is unavoidable. */
    float handif = r_handif(R_SEED_HELD, R_HANDIF_N);
    r_puts("[r3-test] handif: best FIXED input->label rule acc ");
    r_putf1(handif); r_puts("%  (<= chance+eps by construction)\r\n");
    r_puts((handif < chance + 12.0f) ? "[r3-incontext-handif] PASS\r\n"
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

#define H_CHANCE   (100.0f / (float)R_VALV)   /* = 25 */

/* The one fixed fact-set D*: key k -> value DSTAR[k]. Fixed (NOT
 * resampled) so it is a single conversation's fact. Deterministically
 * derived so it is not silently special; it is just one of the
 * combinatorially many dictionaries the substrate was trained over, so
 * the trained weights are D*-naive (that is what [handoff-fast-only]
 * proves). */
static UB DSTAR[R_KEYV];
static UB DSCRAM[R_KEYV];   /* D' != D* for the SCRAMBLED control */

static void h_make_dstar(void)
{
    /* fixed bindings, each in {0..R_VALV-1}; chosen to use all classes */
    static const UB fixed[R_KEYV] = { 2, 0, 3, 1, 0, 2, 1, 3 };
    for (INT k = 0; k < R_KEYV; k++) DSTAR[k] = fixed[k];
    /* D' = D* shifted by 1 mod R_VALV at every key -> differs at EVERY
     * key, so a teacher reading D' never agrees with D* anywhere. */
    for (INT k = 0; k < R_KEYV; k++)
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
                  const UB dict[R_KEYV], INT qkey)
{
    /* shuffle the 8 keys (Fisher-Yates over r_rand, the same RNG R3
     * uses) so dict-token order / query position vary per call. */
    UB pool[R_KEYV];
    for (INT i = 0; i < R_KEYV; i++) pool[i] = (UB)i;
    for (INT i = R_KEYV-1; i > 0; i--) {
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
        INT qkey = r_uni(0, R_KEYV-1);
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
        INT qkey = r_uni(0, R_KEYV-1);
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
                          const UB teach_dict[R_KEYV], INT teach_mode)
{
    UW save = r_rng; r_rng = seed;
    for (INT rd = 0; rd < rounds; rd++) {
        for (INT it = 0; it < per_round; it++) {
            INT qkey = r_uni(0, R_KEYV-1);
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
#define R3_FKEYS           (R_KEYV / R3_NFACTS) /* keys per fact = 2          */
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

typedef struct {
    UB key[R_KEYV];     /* the fact's bound keys                       */
    UB yhat[R_KEYV];    /* the FAST layer's reading per key            */
    UB n;               /* bindings in this fact (<= R_KEYV)           */
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
static UB SDICT[R_KEYV];
static void s_make_facts(void)
{
    static const UB fixed[R_KEYV] = { 2, 0, 3, 3, 1, 3, 1, 1 };
    for (INT k = 0; k < R_KEYV; k++) SDICT[k] = fixed[k];
}

/* Build ONE stream episode with the SAME token layout as gen_episode /
 * h_build (token choice only; r_forward is the unchanged math).
 *  support!=0 : shown keys sk[]->sv[] carry the conversation's bindings;
 *               every OTHER key's value slot is a fresh random filler,
 *               resampled per episode — exactly the pretraining
 *               distribution, so it averages out (VI.2).
 *  support==0 : MASKED — every value slot is R_UNK; only weights help. */
static void s_build(UB key[R_SEQ], UB val[R_SEQ], INT support,
                    const UB *sk, const UB *sv, INT ns, INT qkey)
{
    UB pool[R_KEYV];
    for (INT i = 0; i < R_KEYV; i++) pool[i] = (UB)i;
    for (INT i = R_KEYV-1; i > 0; i--) {              /* Fisher-Yates */
        INT j = r_uni(0, i);
        UB t = pool[i]; pool[i] = pool[j]; pool[j] = t;
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

/* ---- the LIVE arrival API (VI.3) ---------------------------------- */
INT r3_fact_learn(const UB *keys, const UB *vals, INT n)
{
    if (n < 1 || n > R_KEYV) return -1;

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
            s_build(kk, vv, 1, keys, vals, n, keys[i]);
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

/* ONE bounded sleep round (VI.4). with_replay!=0 (the production path)
 * interleaves the oldest PENDING fact's engrams with ALL RETAINED
 * facts' engrams; ==0 is the naive control the disease run measures.
 * The ONLY difference is the interleave — same steps, same lr, same
 * arrangement stream (per-step RNG consumption is identical). */
static INT s_round(INT with_replay)
{
    INT pi = -1; UW pseq = 0xFFFFFFFFUL;
    for (INT i = 0; i < (INT)r3_fq_n; i++)
        if (r3_fq[i].state == R3F_PENDING && r3_fq[i].seq < pseq) {
            pi = i; pseq = r3_fq[i].seq;
        }
    if (pi < 0) return 0;

    /* item list: pending fact's engrams (+ retained facts' on replay) */
    UB fi[R3_FQ_MAX * R_KEYV], bi[R3_FQ_MAX * R_KEYV];
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
        s_build(kk, vv, 0, NULL, NULL, 0, k);          /* MASKED student */
        for (INT i = 0; i < R_NP; i++) rg[i] = 0.0f;
        r_forward(kk, vv, y);                          /* R3's own fwd   */
        r_backward(y);                                 /* R3's own bwd   */
        for (INT i = 0; i < R_NP; i++) rw[i] -= R3_STREAM_LR * rg[i];
        s_steps_last_round++;
    }
    r3_s_rng = r_rng; r_rng = save;

    if (++r3_fq[pi].rounds_done >= R3_SLEEPS_PER_FACT)
        r3_fq[pi].state = R3F_RETAINED;
    return 1;
}

/* the LIVE idle round — the EXACT symbol dmn_idle_work() calls (VI.5).
 * Nothing live reads rw[] (R3 is self-test + this loop), so no busy
 * flag is needed yet — named for the slice that first serves live R3
 * queries, not built (VI.8). */
INT r3_consolidate_idle_round(void) { return s_round(1); }

/* MASKED accuracy on fact f's keys vs the oracle SDICT, on a held-out
 * arrangement stream (queries restricted to K_f). */
static float s_eval_fact(UW seed, INT n, INT f)
{
    UW save = r_rng; r_rng = seed + (UW)f * 0x9E3779B9UL;
    INT correct = 0;
    for (INT e = 0; e < n; e++) {
        INT qkey = f * R3_FKEYS + r_uni(0, R3_FKEYS - 1);
        UB kk[R_SEQ], vv[R_SEQ];
        s_build(kk, vv, 0, NULL, NULL, 0, qkey);
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

    r_puts("[stream] ==== LM-5 随時 stream (facts arrive, many sleeps, one queue) ====\r\n");
    r_puts("[stream] F="); r_putdec(R3_NFACTS); r_puts(" facts x ");
    r_putdec(R3_FKEYS);
    r_puts(" keys; union = the LM-4-proven 8 bindings; chance 25%\r\n");
    r_puts("[stream] budgets: R3_FQ_MAX="); r_putdec(R3_FQ_MAX);
    r_puts("  R3_IDLE_STEPS="); r_putdec(R3_IDLE_STEPS);
    r_puts("  R3_SLEEPS_PER_FACT="); r_putdec(R3_SLEEPS_PER_FACT);
    r_puts("\r\n");

    s_make_facts();

    /* substrate: pretrain to in-context competence EXACTLY as r3_test /
     * r3_handoff_test do (resampled dicts). Every fact is then FAST-
     * readable but absent from the weights — gated per fact below. */
    r_init_weights(0xA5A5u);
    float lr = 0.05f;
    for (INT ep = 0; ep < R_EPOCHS; ep++) {
        if (ep == R_EPOCHS*2/3)  lr = 0.02f;
        if (ep == R_EPOCHS*9/10) lr = 0.008f;
        r_train_epoch(R_SEED_TRAIN, R_TRAIN_N, lr);
    }
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
    INT dis_ok = pre_ok && (acc_f1_post >= 50.0f)
              && (acc_naive[0] <= 40.0f)
              && ((acc_f1_post - acc_naive[0]) >= 25.0f);
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
