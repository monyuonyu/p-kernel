/*
 *  dtr_train.c — R3a: a genuinely trained brain.
 *
 *  This file is the direct answer to PR #3's critique: the Transformer
 *  in dtr.c was initialized with LCG noise and never trained, so its
 *  `infer` output was deterministic noise dressed up as "AI". Here the
 *  same 635-parameter model is trained for real:
 *
 *    - SUPERVISED TASK: classify a 4-channel int8 sensor reading
 *      [temp, humidity, pressure, light] into the 3 classes the shell
 *      already advertises (0=normal, 1=alert, 2=critical). The label
 *      is driven by a LATENT temperature; the observed channel-0 value
 *      is the latent plus noise, so samples near the class boundaries
 *      are genuinely ambiguous (Bayes error > 0 — a hand threshold on
 *      the observed value cannot reach 100% either: the BEST oracle-
 *      fitted double threshold on observed temperature scores 94.3%
 *      over all 300 samples / 93.8% on the train split, measured by
 *      replaying this exact generator offline).
 *    - DATASET: deterministic synthetic generator (fixed LCG seed,
 *      same bytes on every node / every ABI). 300 samples, class-
 *      balanced; first 240 train, last 60 held out. Secondary
 *      channels: humidity anti-correlated with temperature, pressure
 *      pure noise, light weakly class-correlated — correlated
 *      distractors, not free giveaways.
 *    - TRAINING: full-batch SGD with ANALYTIC backprop through the
 *      whole graph (dtr.c dtr_train_batch) and a real cross-entropy
 *      loss. Not finite differences; `dtr grad` cross-checks the
 *      analytic gradient against central differences.
 *    - HONEST METRICS: `dtr eval` prints held-out accuracy + mean CE
 *      at any time — run it BEFORE training (random weights, ~33%) and
 *      AFTER. Both numbers are part of the demo, no cherry-picking.
 *    - WEIGHTS BECOME MEMORY: `dtr save` serializes the trained
 *      weights (fixed-width header + 635 float32) and stores them as a
 *      p-fs P2 versioned object named "dtr/weights" — content-
 *      addressed, replicated by P1, history preserved by the version
 *      DAG. `dtr load` on ANY node that has the replica restores the
 *      same brain: learning propagates through the network's memory.
 *
 *  arch/common discipline: no <string.h>, fixed-width persisted types,
 *  static (not task-stack) buffers, output via sio_send_frame.
 */

#include "dtr.h"
#include "retrieval.h"
#include "pfs_block.h"
#include "pfs_dag.h"
#include "gossip_learn.h"   /* G22: decentralized collective learning */
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* output helpers (sio frame channel, like pfs_dag.c)                  */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void tr_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void tr_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { tr_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    tr_puts(&buf[i]);
}

/* xx.x — used for accuracy percent and CE loss */
static void tr_putf1(float f)
{
    if (f < 0.0f) { tr_puts("-"); f = -f; }
    UW whole = (UW)f;
    UW frac  = (UW)((f - (float)whole) * 10.0f + 0.5f);
    if (frac >= 10) { whole++; frac = 0; }
    tr_putdec(whole); tr_puts("."); tr_putdec(frac);
}

/* x.xxx — finer print for losses / gradient errors */
static void tr_putf3(float f)
{
    if (f < 0.0f) { tr_puts("-"); f = -f; }
    UW whole = (UW)f;
    UW frac  = (UW)((f - (float)whole) * 1000.0f + 0.5f);
    if (frac >= 1000) { whole++; frac = 0; }
    tr_putdec(whole); tr_puts(".");
    if (frac < 100) tr_puts("0");
    if (frac < 10)  tr_puts("0");
    tr_putdec(frac);
}

/* ------------------------------------------------------------------ */
/* dataset — deterministic synthetic sensor readings                   */
/* ------------------------------------------------------------------ */

#define DS_N       300            /* total samples                     */
#define DS_TRAIN   240            /* training split (first 240)        */
#define DS_TEST    (DS_N - DS_TRAIN)   /* held-out split (last 60)     */

#define DS_SEED    0x5EED2026UL   /* fixed: same dataset on every node */

static B  ds_x[DS_N][DTR_SEQ_LEN];
static UB ds_y[DS_N];
static UB ds_ready = 0;

static UW ds_rng;

static UW ds_rand(void)            /* LCG, 15-bit output */
{
    ds_rng = ds_rng * 1664525UL + 1013904223UL;
    return (ds_rng >> 16) & 0x7FFF;
}

static INT ds_uniform(INT lo, INT hi)        /* inclusive */
{
    return lo + (INT)(ds_rand() % (UW)(hi - lo + 1));
}

/* approx-gaussian noise: sum of 4 uniforms in [-s, s], halved
 * (sigma ~= 0.58 * s) */
static INT ds_noise(INT s)
{
    INT v = 0;
    for (INT i = 0; i < 4; i++) v += ds_uniform(-s, s);
    return v / 2;
}

static B ds_clamp(INT v)
{
    if (v >  127) v =  127;
    if (v < -128) v = -128;
    return (B)v;
}

/*
 *  Sample generation. The LABEL comes from a latent temperature:
 *    normal   : latent in [-50, 24]
 *    alert    : latent in [ 25, 69]
 *    critical : latent in [ 70,120]
 *  The OBSERVED temperature is latent + noise(sigma~7), so readings
 *  near 25 and 70 overlap across classes — the task is learnable but
 *  not separable, and a fixed threshold on the observed value is NOT
 *  trivially perfect.
 */
static void ds_init(void)
{
    if (ds_ready) return;
    ds_rng = DS_SEED;

    for (INT i = 0; i < DS_N; i++) {
        UB c = (UB)(i % 3);              /* class-balanced both splits */
        INT latent;
        if (c == 0)      latent = ds_uniform(-50,  24);
        else if (c == 1) latent = ds_uniform( 25,  69);
        else             latent = ds_uniform( 70, 120);

        INT temp  = latent + ds_noise(12);            /* sigma ~ 7     */
        INT hum   = 60 - latent / 2 + ds_noise(20);   /* anti-corr.    */
        INT press = ds_uniform(-30, 90);              /* pure noise    */
        INT light = 10 + 30 * (INT)c + ds_noise(35);  /* weak signal   */

        ds_x[i][0] = ds_clamp(temp);
        ds_x[i][1] = ds_clamp(hum);
        ds_x[i][2] = ds_clamp(press);
        ds_x[i][3] = ds_clamp(light);
        ds_y[i]    = c;
    }
    ds_ready = 1;
}

/* ------------------------------------------------------------------ */
/* eval — honest numbers, train and held-out, any time                 */
/* ------------------------------------------------------------------ */

static void print_split(const char *tag, const B (*X)[DTR_SEQ_LEN],
                        const UB *y, UW n, const char *suffix)
{
    UW correct = 0;
    float ce = dtr_eval_batch(X, y, n, &correct);
    float pct = (float)correct * 100.0f / (float)n;

    tr_puts("[dtr] eval "); tr_puts(tag);
    tr_puts(": acc "); tr_putf1(pct);
    tr_puts("% ("); tr_putdec(correct); tr_puts("/"); tr_putdec(n);
    tr_puts(")  mean CE "); tr_putf3(ce);
    tr_puts(suffix);
    tr_puts("\r\n");
}

/* Wave 8 ①: 同じ重みで retrieval OFF / ON の両方を並記する — どちらの
 * 数字も同じ dtr_eval_batch から出る、cherry-pick なしの直接比較。
 * ON 側は p-fs に "dtr/engrams" が無ければ測れない (記憶が源)。 */
static void cmd_eval(void)
{
    ds_init();

    UB rprev = ret_set(0);
    print_split("train   ", ds_x,            ds_y,            DS_TRAIN,
                "  [ret off]");
    print_split("held-out", ds_x + DS_TRAIN, ds_y + DS_TRAIN, DS_TEST,
                "  [ret off]");

    ret_set(1);
    if (ret_avail() > 0) {
        print_split("train   ", ds_x,            ds_y,            DS_TRAIN,
                    "  [ret ON]");
        print_split("held-out", ds_x + DS_TRAIN, ds_y + DS_TRAIN, DS_TEST,
                    "  [ret ON]");
    } else {
        tr_puts("[dtr] eval [ret ON] skipped: no '" RET_REF "' in p-fs"
                " — without memory, retrieval cannot think\r\n");
    }
    ret_set(rprev);
}

/* ------------------------------------------------------------------ */
/* train                                                               */
/* ------------------------------------------------------------------ */

#define TR_EPOCHS_DFLT 300
#define TR_EPOCHS_MAX  5000

static void cmd_train(UW epochs)
{
    ds_init();
    if (epochs == 0)             epochs = TR_EPOCHS_DFLT;
    if (epochs > TR_EPOCHS_MAX)  epochs = TR_EPOCHS_MAX;

    tr_puts("[dtr] training: ");
    tr_putdec(DS_TRAIN); tr_puts(" samples, ");
    tr_putdec(epochs);   tr_puts(" epochs, full-batch SGD,"
                                 " analytic backprop\r\n");

    /* block concurrent dtr_infer while weights are mid-update (the GA
     * uses the same flag for the same reason) */
    dtr_ga_busy = 1;

    SYSTIM t0, t1;
    tk_get_tim(&t0);

    float loss = 0.0f;
    for (UW e = 1; e <= epochs; e++) {
        /* simple step decay keeps full-batch SGD stable late on */
        float lr = (e <= epochs / 2) ? 0.1f : 0.05f;
        loss = dtr_train_batch(ds_x, ds_y, DS_TRAIN, lr);
        if (e == 1 || e % 50 == 0 || e == epochs) {
            tr_puts("[dtr]   epoch "); tr_putdec(e);
            tr_puts("  train CE "); tr_putf3(loss);
            tr_puts("\r\n");
        }
    }

    tk_get_tim(&t1);
    dtr_ga_busy = 0;

    UW ms = t1.lo - t0.lo;           /* mod-2^32 safe for demo spans */
    tr_puts("[dtr] trained "); tr_putdec(epochs);
    tr_puts(" epochs in "); tr_putdec(ms);
    tr_puts(" ms ("); tr_putdec(epochs * DS_TRAIN);
    tr_puts(" fwd+bwd passes)\r\n");

    cmd_eval();
}

/* ------------------------------------------------------------------ */
/* save / load — the trained brain as a versioned p-fs object          */
/* ------------------------------------------------------------------ */

#define DTR_WEIGHTS_REF "dtr/weights"
#define DTR_WEIGHTS_REF_LEN 11

/* persisted image: header + params. Static (stack discipline) and
 * packed; layout pinned below. Float-bits portability: see the honest
 * statement at DTR_WBLOB_HDR in dtr.h (all 4 targets are LE IEEE754). */
static struct __attribute__((packed, aligned(4))) {
    DTR_WBLOB_HDR h;                /* 20 B -> w[] stays 4-aligned     */
    float         w[DTR_WEIGHT_FLOATS];
} wblob;

_Static_assert(sizeof(DTR_WBLOB_HDR) == 20, "weight blob header is 20 B");
_Static_assert(sizeof(float) == 4, "float must be IEEE754 binary32");
_Static_assert(sizeof(wblob) == 20 + DTR_WEIGHT_FLOATS * 4,
               "weight blob must be header + 635 packed float32");
_Static_assert(sizeof(wblob) <= PFS_BLOCK_MAX,
               "weight blob must fit one p-fs block");

static void cmd_save(void)
{
    wblob.h.magic    = DTR_WBLOB_MAGIC;
    wblob.h.version  = DTR_WBLOB_VER;
    wblob.h.n_params = DTR_WEIGHT_FLOATS;
    wblob.h.d_model  = DTR_EMBED_DIM;
    wblob.h.n_heads  = DTR_NUM_HEADS;
    wblob.h.seq_len  = DTR_SEQ_LEN;
    wblob.h.ffn_dim  = DTR_FFN_DIM;
    wblob.h.out_dim  = DTR_OUT_DIM;
    wblob.h._pad[0] = wblob.h._pad[1] = wblob.h._pad[2] = 0;
    dtr_weights_get(wblob.w);

    INT r = pfs_dag_save((const UB *)DTR_WEIGHTS_REF, DTR_WEIGHTS_REF_LEN,
                         &wblob, (UW)sizeof(wblob));
    if (r != PFS_OK) {
        tr_puts("[dtr] save failed (pfs err ");
        tr_putdec((UW)(-r)); tr_puts(")\r\n");
        return;
    }
    /* pfs_dag_save already printed manifest/content ids + seq */
    tr_puts("[dtr] weights ("); tr_putdec((UW)sizeof(wblob));
    tr_puts(" B) saved as p-fs object '" DTR_WEIGHTS_REF
            "' — replicates to region peers\r\n");
}

/* shared load core: read + validate + install. Returns E_OK style:
 * 0 on success, -1 not found, -2 rejected. Used by `dtr load` and by
 * the guard recover_fn after a worker-task fault. */
static INT load_weights_from_pfs(void)
{
    INT r = pfs_dag_read((const UB *)DTR_WEIGHTS_REF, DTR_WEIGHTS_REF_LEN,
                         &wblob, (UW)sizeof(wblob));
    if (r == PFS_E_NOTFOUND) return -1;
    if (r != (INT)sizeof(wblob) ||
        wblob.h.magic    != DTR_WBLOB_MAGIC ||
        wblob.h.version  != DTR_WBLOB_VER   ||
        wblob.h.n_params != DTR_WEIGHT_FLOATS ||
        wblob.h.d_model  != DTR_EMBED_DIM ||
        wblob.h.n_heads  != DTR_NUM_HEADS ||
        wblob.h.seq_len  != DTR_SEQ_LEN ||
        wblob.h.ffn_dim  != DTR_FFN_DIM ||
        wblob.h.out_dim  != DTR_OUT_DIM)
        return -2;
    dtr_weights_set(wblob.w);
    return 0;
}

static void cmd_load(void)
{
    INT r = load_weights_from_pfs();
    if (r == -1) {
        tr_puts("[dtr] no '" DTR_WEIGHTS_REF "' object here yet"
                " (want sent if a ref is known — retry)\r\n");
        return;
    }
    if (r == -2) {
        tr_puts("[dtr] '" DTR_WEIGHTS_REF "' blob rejected"
                " (bad size/magic/dims)\r\n");
        return;
    }
    tr_puts("[dtr] weights loaded from p-fs object '" DTR_WEIGHTS_REF
            "' ("); tr_putdec((UW)sizeof(wblob));
    tr_puts(" B) — this node now runs the trained brain\r\n");
}

/* ------------------------------------------------------------------ */
/* Wave 8 ① — remember / ret: 記憶→思考の配線の操作面                  */
/* ------------------------------------------------------------------ */

/* `dtr remember`: 訓練セットから代表 engram を p-fs に保存する。
 * 重みは一切保存しない — これは dtr save とは別物 (あちらはライフ
 * サイクル、こちらは思考中に参照される記憶)。 */
static void cmd_remember(void)
{
    ds_init();
    ret_remember(ds_x, ds_y, DS_TRAIN);
}

/* (`dtr ret …` の本体 cmd_ret はディスパッチャ節 — tr_tok の後 — にある) */

static void cmd_grad(void)
{
    ds_init();
    float worst = 0.0f;
    for (INT s = 0; s < 3; s++) {            /* one sample per class */
        float e = dtr_grad_check(ds_x[s], ds_y[s]);
        if (e > worst) worst = e;
    }
    tr_puts("[dtr] gradient check (analytic vs central finite diff,"
            " 3 samples, every 13th param): max rel err ");
    tr_putf3(worst);
    tr_puts(worst < 0.08f ? "  [OK]\r\n" : "  [SUSPECT]\r\n");
}

/* ------------------------------------------------------------------ */
/* Wave 7 — guarded worker task + crash injection + recover_fn         */
/* ------------------------------------------------------------------ */

/* `dtr crash` arms this; the guarded worker trips on it. volatile:
 * written from the shell task, read from the worker task. */
volatile UB dtr_crash_req = 0;

/* The guarded ring-0 inference worker. usermain registers it via
 * guard_register("dtr-worker", ...) at boot, with recover_fn =
 * dtr_recover_weights. Its demo job is to be killable: on `dtr crash`
 * it (1) CORRUPTS the live in-memory weights — so a later `dtr eval`
 * can only score trained accuracy if recovery REALLY reloaded them
 * from p-fs, the assert is not vacuous — and (2) writes through a
 * NULL pointer, faulting in ring-0 task context. */
void dtr_worker_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    for (;;) {
        if (dtr_crash_req) {
            dtr_crash_req = 0;

            /* zeroed weights => uniform softmax => ~33% accuracy.
             * static (not task-stack): 2.5 KB. */
            static float junk[DTR_WEIGHT_FLOATS];
            for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) junk[i] = 0.0f;
            dtr_weights_set(junk);
            tr_puts("[dtr] worker: weights ZEROED in memory;"
                    " now writing through NULL...\r\n");

            /* the fault. Via a volatile pointer variable so the
             * compiler can neither warn about nor elide the store. */
            {
                volatile UW *p = (volatile UW *)0;
                *p = 0xDEADBEEFUL;          /* SIGSEGV -> guard path */
            }
            tr_puts("[dtr] worker: STILL ALIVE after NULL write —"
                    " fault capture is broken\r\n");
        }
        tk_dly_tsk(100);
    }
}

/* guard recover_fn — runs in the guard supervisor task right before
 * the worker is respawned. The whole point of wave 6's `dtr save`:
 * the brain survives its body. */
void dtr_recover_weights(void)
{
    INT r = load_weights_from_pfs();
    if (r == 0) {
        tr_puts("[guard] dtr recover: weights restored from p-fs"
                " object '" DTR_WEIGHTS_REF "'\r\n");
    } else {
        tr_puts("[guard] dtr recover: no usable '" DTR_WEIGHTS_REF
                "' object in p-fs — weights NOT restored"
                " (train+save first)\r\n");
    }
}

static void cmd_crash(void)
{
    tr_puts("[dtr] crash injection armed — worker will corrupt"
            " weights and fault within ~100 ms\r\n");
    dtr_crash_req = 1;
}

/* ------------------------------------------------------------------ */
/* shell dispatcher — args points just past "dtr"                      */
/* ------------------------------------------------------------------ */

static const UB *tr_skip_ws(const UB *p, const UB *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static INT tr_tok(const UB *p, const UB *end, const char *kw)
{
    INT i = 0;
    while (kw[i]) { if (p + i >= end || p[i] != (UB)kw[i]) return 0; i++; }
    return (p + i == end || p[i] == ' ' || p[i] == '\t');
}

IMPORT void dtr_stat(void);

/* `dtr ret` / `dtr ret on|off|reload` — retrieval ブレンドの操作 */
static void cmd_ret(const UB *p, const UB *end)
{
    p = tr_skip_ws(p, end);
    if (p >= end) { ret_stat(); return; }

    if (tr_tok(p, end, "on")) {
        ret_set(1);
        tr_puts("[ret] blend ON — forward now votes with p-fs engrams\r\n");
        ret_stat();
        return;
    }
    if (tr_tok(p, end, "off")) {
        ret_set(0);
        tr_puts("[ret] blend OFF — weights only\r\n");
        return;
    }
    if (tr_tok(p, end, "reload")) {
        ret_drop();
        if (ret_avail() > 0) ret_stat();
        else tr_puts("[ret] reload: no '" RET_REF "' in p-fs yet"
                     " (want sent — retry)\r\n");
        return;
    }
    tr_puts("usage: dtr ret [on|off|reload]\r\n");
}

void dtr_train_cmd(const UB *args, UW len)
{
    const UB *end = args + len;
    const UB *p   = tr_skip_ws(args, end);

    if (p >= end || tr_tok(p, end, "stat")) { dtr_stat(); return; }

    if (tr_tok(p, end, "eval")) { cmd_eval(); return; }
    if (tr_tok(p, end, "save")) { cmd_save(); return; }
    if (tr_tok(p, end, "load")) { cmd_load(); return; }
    if (tr_tok(p, end, "grad")) { cmd_grad(); return; }
    if (tr_tok(p, end, "crash")) { cmd_crash(); return; }
    if (tr_tok(p, end, "remember")) { cmd_remember(); return; }
    if (tr_tok(p, end, "ret")) { cmd_ret(p + 3, end); return; }
    if (tr_tok(p, end, "gossip")) { gl_cmd(p + 6, (UW)(end - (p + 6))); return; }
    if (tr_tok(p, end, "train")) {
        p += 5;
        p = tr_skip_ws(p, end);
        UW epochs = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            epochs = epochs * 10 + (UW)(*p - '0'); p++;
        }
        cmd_train(epochs);
        return;
    }

    tr_puts("usage: dtr [stat] | dtr eval | dtr train [epochs]"
            " | dtr save | dtr load | dtr grad | dtr crash"
            " | dtr remember | dtr ret [on|off|reload]"
            " | dtr gossip [test|solo|run|status]\r\n");
}
