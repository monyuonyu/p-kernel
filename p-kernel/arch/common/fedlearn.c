/*
 *  fedlearn.c (x86)
 *  Federated Learning — FedAvg across cluster nodes
 *
 *  Protocol (two-phase):
 *    Phase A: local training step (finite-difference gradient approximation)
 *    Phase B: DRPC-based weight aggregation
 *      1. Caller sends local delta weights to aggregator node (node 0) via UDP
 *      2. Aggregator collects deltas from all alive nodes
 *      3. Aggregator computes weighted average (FedAvg)
 *      4. Aggregator broadcasts aggregated weights back to all nodes
 *      5. All nodes apply the update
 *
 *  Weight transfer: floats are sent as raw UDP on FL_UDP_PORT (7375)
 *  to avoid fragmenting the 32-byte DRPC_PKT.  DRPC_CALL_FL_AGG is
 *  used only as a lightweight control message (notify + acknowledge).
 *
 *  This is a demonstration-grade implementation; production FL would
 *  add differential privacy, secure aggregation, and stragglers timeout.
 */

#include "kernel.h"
#include "ai_kernel.h"
#include "drpc.h"
#include "netstack.h"
#include "gossip_learn.h"   /* G22: decentralized no-central averaging */
#include <tmonitor.h>

/* G22 NOTE (wave 16): the canonical, CI-proven collective-learning
 * implementation is gossip_learn.c on dtr.c's 635-param transformer —
 * disjoint leave-one-class-out shards, gossip-averaged full weight
 * bodies, no central aggregator, surviving node death ([g22-*] +
 * samples/32). This file is the LEGACY 4->8->8->3 MLP path; wave 16
 * retires its two documented fakes — (1) fl_local_train finite-
 * differencing ONLY b3, and (2) dtk_fl_aggregate's E_NOSPT central
 * aggregator — by training the FULL MLP body and folding peers in with
 * the SAME no-central p-fs gossip-average primitive (gl_pfs_* / gl_merge).
 * The legacy MLP path is not wired into CI; the transformer path is. */

/* p-fs ref for the gossiped MLP model (decentralized, per-node). */
#define FL_MODEL_REF      "fl/model/"
#define FL_MODEL_REF_LEN  9      /* + node digit(s) appended            */
#define FL_FLAT_N         (MLP_IN*MLP_H1 + MLP_H1 + MLP_H1*MLP_H2 + MLP_H2 \
                           + MLP_H2*MLP_OUT + MLP_OUT)   /* 139 floats   */

/* ------------------------------------------------------------------ */
/* Local training: finite-difference gradient approximation           */
/*                                                                     */
/*   For each weight w_i:                                              */
/*     loss(w_i + eps) - loss(w_i - eps)                              */
/*     delta_w_i ≈ ─────────────────────────── × lr                   */
/*                           2 × eps                                   */
/*                                                                     */
/*   Loss = cross-entropy on the provided labelled samples.            */
/*   This is O(P × N × forward_passes) — demo-scale only.             */
/* ------------------------------------------------------------------ */

#define FL_EPS    0.05f    /* finite-difference step         */
#define FL_LR     0.01f    /* learning rate for local update */
#define FL_ROUNDS_MAX  8

static UW  fl_rounds = 0;
static float fl_last_loss = 0.0f;

/* accurate libc-free ln from dtr.c (R3a) */
IMPORT float dtr_logf(float x);

/* Cross-entropy loss on one sample (true label = label).
 *
 * R3a HONESTY NOTE: this used to be
 *     return (pred == label) ? 0.1f : 1.0f;
 * — a step function of the argmax dressed up as a loss (PR #3 called
 * it out, correctly: "何も学習していません"). A step function has zero
 * gradient almost everywhere, so the finite-difference loop below was
 * measuring noise. Now it is the real thing: -ln softmax_label(MLP),
 * smooth in the weights, so the finite differences are actual
 * gradients. */
static float cross_entropy_loss(const B input[MLP_IN], UB label)
{
    float p[MLP_OUT];
    mlp_forward_probs(input, p);
    float pl = p[label];
    if (pl < 1e-7f) pl = 1e-7f;
    return -dtr_logf(pl);
}

ER fl_local_train(const B samples[][MLP_IN], const UB labels[],
                  UW n,
                  float delta_w1[MLP_IN*MLP_H1], float delta_b1[MLP_H1],
                  float delta_w2[MLP_H1*MLP_H2], float delta_b2[MLP_H2],
                  float delta_w3[MLP_H2*MLP_OUT], float delta_b3[MLP_OUT])
{
    if (n == 0 || !samples || !labels) return E_PAR;

    /* Get current weights */
    float w1[MLP_IN*MLP_H1], b1[MLP_H1];
    float w2[MLP_H1*MLP_H2], b2[MLP_H2];
    float w3[MLP_H2*MLP_OUT], b3[MLP_OUT];
    mlp_get_weights(w1, b1, w2, b2, w3, b3);

    /* Compute baseline loss */
    float baseline = 0.0f;
    for (UW s = 0; s < n; s++)
        baseline += cross_entropy_loss(samples[s], labels[s]);
    baseline /= (float)n;
    fl_last_loss = baseline;

    /* central finite-difference gradient over the WHOLE weight body.
     *
     * HONESTY (G22, wave 16): this used to finite-difference ONLY b3
     * (MLP_OUT params); delta_w1/w2/w3/b1/b2 were allocated and left at
     * 0, so w1/w2/w3 never moved — the audit's "重み本体は誰も outcome で
     * 更新しない". Now every parameter gets a real central-difference
     * gradient against the smooth cross-entropy loss. Helper folds the
     * boilerplate over each of the six arrays. */
#define FL_GRAD(arr, darr, cnt)                                            \
    do {                                                                  \
        for (INT _i = 0; _i < (cnt); _i++) {                              \
            float _o = (arr)[_i];                                         \
            (arr)[_i] = _o + FL_EPS;                                      \
            mlp_set_weights(w1, b1, w2, b2, w3, b3);                      \
            float _lp = 0.0f;                                            \
            for (UW _s = 0; _s < n; _s++)                                 \
                _lp += cross_entropy_loss(samples[_s], labels[_s]);       \
            (arr)[_i] = _o - FL_EPS;                                      \
            mlp_set_weights(w1, b1, w2, b2, w3, b3);                      \
            float _lm = 0.0f;                                            \
            for (UW _s = 0; _s < n; _s++)                                 \
                _lm += cross_entropy_loss(samples[_s], labels[_s]);       \
            (darr)[_i] = -FL_LR * ((_lp - _lm) / (float)n)               \
                         / (2.0f * FL_EPS);                               \
            (arr)[_i] = _o;   /* restore */                              \
        }                                                                 \
    } while (0)

    FL_GRAD(w1, delta_w1, MLP_IN*MLP_H1);
    FL_GRAD(b1, delta_b1, MLP_H1);
    FL_GRAD(w2, delta_w2, MLP_H1*MLP_H2);
    FL_GRAD(b2, delta_b2, MLP_H2);
    FL_GRAD(w3, delta_w3, MLP_H2*MLP_OUT);
    FL_GRAD(b3, delta_b3, MLP_OUT);
#undef FL_GRAD

    /* Restore original weights (deltas are applied by dtk_fl_aggregate) */
    mlp_set_weights(w1, b1, w2, b2, w3, b3);
    return E_OK;
}

/* flatten / unflatten the 6 MLP arrays to one FL_FLAT_N float vector
 * (same layout the shell packs and that gl_pfs_* gossips). */
static void fl_flatten(float *out)
{
    float w1[MLP_IN*MLP_H1], b1[MLP_H1];
    float w2[MLP_H1*MLP_H2], b2[MLP_H2];
    float w3[MLP_H2*MLP_OUT], b3[MLP_OUT];
    mlp_get_weights(w1, b1, w2, b2, w3, b3);
    UW o = 0;
    for (INT i = 0; i < MLP_IN*MLP_H1;  i++) out[o++] = w1[i];
    for (INT i = 0; i < MLP_H1;         i++) out[o++] = b1[i];
    for (INT i = 0; i < MLP_H1*MLP_H2;  i++) out[o++] = w2[i];
    for (INT i = 0; i < MLP_H2;         i++) out[o++] = b2[i];
    for (INT i = 0; i < MLP_H2*MLP_OUT; i++) out[o++] = w3[i];
    for (INT i = 0; i < MLP_OUT;        i++) out[o++] = b3[i];
}
static void fl_unflatten(const float *in)
{
    float w1[MLP_IN*MLP_H1], b1[MLP_H1];
    float w2[MLP_H1*MLP_H2], b2[MLP_H2];
    float w3[MLP_H2*MLP_OUT], b3[MLP_OUT];
    UW o = 0;
    for (INT i = 0; i < MLP_IN*MLP_H1;  i++) w1[i] = in[o++];
    for (INT i = 0; i < MLP_H1;         i++) b1[i] = in[o++];
    for (INT i = 0; i < MLP_H1*MLP_H2;  i++) w2[i] = in[o++];
    for (INT i = 0; i < MLP_H2;         i++) b2[i] = in[o++];
    for (INT i = 0; i < MLP_H2*MLP_OUT; i++) w3[i] = in[o++];
    for (INT i = 0; i < MLP_OUT;        i++) b3[i] = in[o++];
    mlp_set_weights(w1, b1, w2, b2, w3, b3);
}

static UW fl_model_ref(char *out, UW node)
{
    const char *p = FL_MODEL_REF;
    UW i = 0;
    while (p[i]) { out[i] = p[i]; i++; }
    if (node >= 10) out[i++] = (char)('0' + (node / 10) % 10);
    out[i++] = (char)('0' + node % 10);
    return i;
}

/* ------------------------------------------------------------------ */
/* FedAvg aggregation via DRPC                                        */
/* ------------------------------------------------------------------ */

/*
 *  Simplified FedAvg for demo:
 *    - Only bias deltas are exchanged (small enough for one UDP packet)
 *    - Node 0 averages incoming deltas weighted by n_samples
 *    - All nodes apply the average locally
 *
 *  In production, full weight matrices would be exchanged using the
 *  FL_UDP_PORT bulk transfer mechanism.
 */

ER dtk_fl_aggregate(UB aggregator_node,
                    const float *my_delta, UW my_n_samples, TMO tmout)
{
    (void)aggregator_node; (void)my_n_samples; (void)tmout;
    if (!my_delta) return E_PAR;

    /* (1) apply MY full-body delta to MY weights (full body now, not b3) */
    {
        float w1[MLP_IN*MLP_H1], b1[MLP_H1];
        float w2[MLP_H1*MLP_H2], b2[MLP_H2];
        float w3[MLP_H2*MLP_OUT], b3[MLP_OUT];
        mlp_get_weights(w1, b1, w2, b2, w3, b3);
        UW o = 0;
        for (INT i = 0; i < MLP_IN*MLP_H1;  i++) w1[i] += my_delta[o++];
        for (INT i = 0; i < MLP_H1;         i++) b1[i] += my_delta[o++];
        for (INT i = 0; i < MLP_H1*MLP_H2;  i++) w2[i] += my_delta[o++];
        for (INT i = 0; i < MLP_H2;         i++) b2[i] += my_delta[o++];
        for (INT i = 0; i < MLP_H2*MLP_OUT; i++) w3[i] += my_delta[o++];
        for (INT i = 0; i < MLP_OUT;        i++) b3[i] += my_delta[o++];
        mlp_set_weights(w1, b1, w2, b2, w3, b3);
    }

    /* Single-node: nothing to average — the local update IS the round. */
    if (drpc_my_node == 0xFF) { fl_rounds++; ai_stats.fl_rounds++; return E_OK; }

    /* (2) DECENTRALIZED no-central averaging (replaces the E_NOSPT central
     * aggregator). I publish my model and fold in whatever ALIVE peers'
     * models are present in p-fs — the SAME gl_merge primitive the
     * transformer path uses, peer-symmetric, no server. Shell-task ctx
     * (gl_pfs_* share the pfs_dag scratch). */
    static float flat[FL_FLAT_N];
    fl_flatten(flat);
    char ref[16]; UW rl = fl_model_ref(ref, drpc_my_node);
    (void)gl_pfs_publish(ref, rl, flat, FL_FLAT_N);

    static float peer[DNODE_MAX][FL_FLAT_N];
    const float *ptrs[DNODE_MAX];
    UW cnt = 0, slot = 0;
    ptrs[cnt++] = flat;                     /* myself */
    for (UB p = 0; p < DNODE_MAX && cnt < DNODE_MAX; p++) {
        if (p == drpc_my_node) continue;
        if (dnode_table[p].state != DNODE_ALIVE) continue;
        char pref[16]; UW prl = fl_model_ref(pref, p);
        if (gl_pfs_fetch(pref, prl, peer[slot], FL_FLAT_N) == 0) {
            ptrs[cnt++] = peer[slot];
            slot++;
        }
    }
    if (cnt > 1) {
        static float avg[FL_FLAT_N];
        gl_merge(avg, ptrs, cnt, FL_FLAT_N);
        fl_unflatten(avg);
    }

    fl_rounds++;
    ai_stats.fl_rounds++;
    return E_OK;
}

ER fl_apply_update(const float *new_weights)
{
    if (!new_weights) return E_PAR;
    const float *p = new_weights;
    float w1[MLP_IN*MLP_H1], b1[MLP_H1];
    float w2[MLP_H1*MLP_H2], b2[MLP_H2];
    float w3[MLP_H2*MLP_OUT], b3[MLP_OUT];

    for (INT i = 0; i < MLP_IN*MLP_H1; i++) w1[i] = *p++;
    for (INT i = 0; i < MLP_H1;        i++) b1[i] = *p++;
    for (INT i = 0; i < MLP_H1*MLP_H2; i++) w2[i] = *p++;
    for (INT i = 0; i < MLP_H2;        i++) b2[i] = *p++;
    for (INT i = 0; i < MLP_H2*MLP_OUT; i++) w3[i] = *p++;
    for (INT i = 0; i < MLP_OUT;        i++) b3[i] = *p++;

    mlp_set_weights(w1, b1, w2, b2, w3, b3);
    return E_OK;
}

/* ------------------------------------------------------------------ */
/* Status print                                                        */
/* ------------------------------------------------------------------ */

static void put_float(float v)
{
    if (v < 0.0f) { tm_putstring((UB *)"-"); v = -v; }
    INT whole = (INT)v;
    INT frac  = (INT)((v - (float)whole) * 100.0f);
    char buf[16];
    INT i = 0;
    if (whole == 0) { buf[i++] = '0'; }
    else {
        INT tmp = whole, digits = 0;
        while (tmp > 0) { digits++; tmp /= 10; }
        tmp = whole;
        for (INT d = digits-1; d >= 0; d--) {
            buf[i+d] = (char)('0' + tmp%10); tmp /= 10;
        }
        i += digits;
    }
    buf[i++] = '.';
    buf[i++] = (char)('0' + frac/10);
    buf[i++] = (char)('0' + frac%10);
    buf[i]   = '\0';
    tm_putstring((UB *)buf);
}

void fl_status(void)
{
    tm_putstring((UB *)"[FL] rounds=");
    char buf[12]; INT i = 11; buf[i] = '\0';
    UW v = fl_rounds;
    if (v == 0) buf[--i] = '0';
    else while (v > 0 && i > 0) { buf[--i] = (char)('0' + v%10); v /= 10; }
    tm_putstring((UB *)(buf+i));
    tm_putstring((UB *)"  last_loss=");
    put_float(fl_last_loss);
    tm_putstring((UB *)"\r\n");
}
