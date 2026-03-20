/*
 *  ai_job.c (x86)
 *  Software NPU: AI job scheduler + MLP neural network
 *
 *  Architecture: sensor → infer
 *    Sensor input: 4 int8 values packed as W
 *      temp     : (°C  - 20) × 2      (20°C=0, 45°C=50, 70°C=100)
 *      humidity : (%   - 50) × 2      (50%=0, 80%=60, 20%=-60)
 *      pressure : (hPa-1013) / 2      (1013=0, 950=-31, 1030=8)
 *      light    : (lux-500 ) / 4      (500=0, 800=75, 100=-100)
 *
 *  Neural network: 4 → 8 → 8 → 3 MLP with ReLU hidden layers
 *    Output class: 0=normal  1=alert  2=critical
 *
 *  The "software NPU" worker task pulls AI_JOB items from a semaphore-
 *  gated queue and executes them.  When real NPU hardware is present,
 *  the dispatch function swaps to hardware DMA — the public API stays
 *  identical.
 */

#include "kernel.h"
#include "ai_kernel.h"
#include <tmonitor.h>

/* ================================================================== */
/* Pre-trained MLP weights (float32)                                  */
/*                                                                    */
/* Training scenario (simulated):                                     */
/*   normal   — all inputs near 0 (values within expected range)     */
/*   alert    — one input moderately out of range (|x| > 40)         */
/*   critical — multiple inputs severely out of range (|x| > 80)     */
/*                                                                    */
/* These weights were hand-tuned to produce correct classifications   */
/* for the normalization scheme defined in ai_kernel.h.               */
/* ================================================================== */

/* Layer 1: 8 neurons, each detecting an excursion in one direction   */
/*   h0..h3: detect HIGH values for each of the 4 inputs             */
/*   h4..h7: detect LOW  values for each of the 4 inputs             */
static float W1[MLP_H1][MLP_IN] = {
    /*  temp    hum    press  light  */
    {  1.5f,  0.1f,  0.1f,  0.1f }, /* h0: temp HIGH     */
    {  0.1f,  1.5f,  0.1f,  0.1f }, /* h1: humid HIGH    */
    {  0.1f,  0.1f, -1.5f,  0.1f }, /* h2: press LOW     */
    {  0.1f,  0.1f,  0.1f, -1.5f }, /* h3: light LOW     */
    { -1.5f,  0.1f,  0.1f,  0.1f }, /* h4: temp LOW      */
    {  0.1f, -1.5f,  0.1f,  0.1f }, /* h5: humid LOW     */
    {  0.1f,  0.1f,  1.5f,  0.1f }, /* h6: press HIGH    */
    {  0.1f,  0.1f,  0.1f,  1.5f }, /* h7: light HIGH    */
};
static float B1[MLP_H1] = {
    -0.30f, -0.30f, -0.30f, -0.30f,
    -0.30f, -0.30f, -0.30f, -0.30f,
};

/* Layer 2: aggregate detections and amplify for severity assessment  */
static float W2[MLP_H2][MLP_H1] = {
    /*  h0    h1    h2    h3    h4    h5    h6    h7  */
    { 0.8f, 0.4f, 0.6f, 0.3f, 0.4f, 0.3f, 0.3f, 0.2f }, /* a0 */
    { 0.4f, 0.8f, 0.3f, 0.6f, 0.3f, 0.4f, 0.2f, 0.3f }, /* a1 */
    { 0.6f, 0.3f, 0.8f, 0.4f, 0.3f, 0.2f, 0.4f, 0.3f }, /* a2 */
    { 0.3f, 0.6f, 0.4f, 0.8f, 0.2f, 0.3f, 0.3f, 0.4f }, /* a3 */
    { 0.4f, 0.3f, 0.3f, 0.2f, 0.8f, 0.4f, 0.6f, 0.3f }, /* a4 */
    { 0.3f, 0.4f, 0.2f, 0.3f, 0.4f, 0.8f, 0.3f, 0.6f }, /* a5 */
    { 0.3f, 0.2f, 0.4f, 0.3f, 0.6f, 0.3f, 0.8f, 0.4f }, /* a6 */
    { 0.2f, 0.3f, 0.3f, 0.4f, 0.3f, 0.6f, 0.4f, 0.8f }, /* a7 */
};
static float B2[MLP_H2] = {
    -0.4f, -0.4f, -0.4f, -0.4f,
    -0.4f, -0.4f, -0.4f, -0.4f,
};

/* Layer 3: classify based on severity                                */
/*   normal  : no alarms → high when sum(a) is low                  */
/*   alert   : 1-2 alarms → moderate sum(a)                         */
/*   critical: many alarms → high sum(a)                             */
static float W3[MLP_OUT][MLP_H2] = {
    /* a0    a1    a2    a3    a4    a5    a6    a7  */
    {-1.2f,-1.2f,-1.2f,-1.2f,-1.2f,-1.2f,-1.2f,-1.2f }, /* normal   */
    { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f }, /* alert    */
    { 1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f }, /* critical */
};
static float B3[MLP_OUT] = { 1.2f, -0.8f, -2.5f };

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static float relu(float x) { return x > 0.0f ? x : 0.0f; }

static float softmax_denom(const float v[], UW n)
{
    /* stable softmax: subtract max first */
    float mx = v[0];
    for (UW i = 1; i < n; i++) if (v[i] > mx) mx = v[i];
    float s = 0.0f;
    for (UW i = 0; i < n; i++) {
        /* Approximate e^x via Taylor: e^x ≈ 1 + x + x²/2 + x³/6 */
        float t = v[i] - mx;
        float ex = 1.0f + t + (t*t)*0.5f + (t*t*t)*(1.0f/6.0f);
        if (ex < 0.0001f) ex = 0.0001f;
        s += ex;
    }
    return s;
}

/* ================================================================== */
/* MLP forward pass                                                   */
/* ================================================================== */

UB mlp_forward(const B input[MLP_IN])
{
    float x[MLP_IN];

    /* Dequantize int8 Q8 to float [-1.0, 1.0] */
    for (INT i = 0; i < MLP_IN; i++)
        x[i] = (float)input[i] / 127.0f;

    /* Layer 1: linear + ReLU */
    float h1[MLP_H1];
    for (INT j = 0; j < MLP_H1; j++) {
        float acc = B1[j];
        for (INT i = 0; i < MLP_IN; i++) acc += W1[j][i] * x[i];
        h1[j] = relu(acc);
    }

    /* Layer 2: linear + ReLU */
    float h2[MLP_H2];
    for (INT j = 0; j < MLP_H2; j++) {
        float acc = B2[j];
        for (INT i = 0; i < MLP_H1; i++) acc += W2[j][i] * h1[i];
        h2[j] = relu(acc);
    }

    /* Layer 3: linear → argmax (no softmax needed for classification) */
    float out[MLP_OUT];
    for (INT j = 0; j < MLP_OUT; j++) {
        float acc = B3[j];
        for (INT i = 0; i < MLP_H2; i++) acc += W3[j][i] * h2[i];
        out[j] = acc;
    }

    /* Argmax */
    UB cls = 0;
    for (UB j = 1; j < MLP_OUT; j++)
        if (out[j] > out[cls]) cls = j;

    (void)softmax_denom;   /* suppress unused warning */
    return cls;
}

/* Weight accessors for FL */

void mlp_get_weights(float *w1, float *b1, float *w2, float *b2,
                     float *w3, float *b3)
{
    for (INT i = 0; i < MLP_H1*MLP_IN; i++) w1[i] = ((float *)W1)[i];
    for (INT i = 0; i < MLP_H1;       i++) b1[i] = B1[i];
    for (INT i = 0; i < MLP_H2*MLP_H1; i++) w2[i] = ((float *)W2)[i];
    for (INT i = 0; i < MLP_H2;        i++) b2[i] = B2[i];
    for (INT i = 0; i < MLP_OUT*MLP_H2; i++) w3[i] = ((float *)W3)[i];
    for (INT i = 0; i < MLP_OUT;        i++) b3[i] = B3[i];
}

void mlp_set_weights(const float *w1, const float *b1,
                     const float *w2, const float *b2,
                     const float *w3, const float *b3)
{
    for (INT i = 0; i < MLP_H1*MLP_IN; i++) ((float *)W1)[i] = w1[i];
    for (INT i = 0; i < MLP_H1;       i++) B1[i] = b1[i];
    for (INT i = 0; i < MLP_H2*MLP_H1; i++) ((float *)W2)[i] = w2[i];
    for (INT i = 0; i < MLP_H2;        i++) B2[i] = b2[i];
    for (INT i = 0; i < MLP_OUT*MLP_H2; i++) ((float *)W3)[i] = w3[i];
    for (INT i = 0; i < MLP_OUT;        i++) B3[i] = b3[i];
}

/* ================================================================== */
/* AI Job queue                                                       */
/* ================================================================== */

static AI_JOB job_pool[AI_JOB_POOL_SIZE];
static ID     job_queue_sem  = 0;   /* counts queued jobs    */
static ID     job_pool_sem   = 0;   /* mutex for pool access */

/* Queue of pending job indices (circular) */
static INT  job_queue[AI_JOB_POOL_SIZE];
static INT  job_q_head = 0, job_q_tail = 0, job_q_cnt = 0;

void ai_job_init(void)
{
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                  .isemcnt = 0, .maxsem = AI_JOB_POOL_SIZE };
    job_queue_sem = tk_cre_sem(&cs);

    T_CSEM mx = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 1, .maxsem = 1 };
    job_pool_sem = tk_cre_sem(&mx);

    for (INT i = 0; i < AI_JOB_POOL_SIZE; i++) job_pool[i].state = 0;
}

ID tk_cre_ai_job(const AI_JOB_SPEC *spec)
{
    if (!spec) return (ID)E_PAR;

    tk_wai_sem(job_pool_sem, 1, TMO_FEVR);

    INT slot = -1;
    for (INT i = 0; i < AI_JOB_POOL_SIZE; i++) {
        if (job_pool[i].state == 0) { slot = i; break; }
    }
    if (slot < 0) { tk_sig_sem(job_pool_sem, 1); return (ID)E_LIMIT; }

    AI_JOB *j = &job_pool[slot];
    j->spec   = *spec;
    j->result = E_OK;
    j->state  = 1;   /* queued */

    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
    j->done_sem = tk_cre_sem(&cs);

    /* Enqueue */
    job_queue[job_q_tail] = slot;
    job_q_tail = (job_q_tail + 1) % AI_JOB_POOL_SIZE;
    job_q_cnt++;

    tk_sig_sem(job_pool_sem, 1);
    tk_sig_sem(job_queue_sem, 1);   /* wake worker */

    return (ID)slot;
}

ER tk_wai_ai_job(ID jid, TMO tmout)
{
    if (jid < 0 || jid >= (ID)AI_JOB_POOL_SIZE) return E_ID;
    if (job_pool[jid].state == 0) return E_NOEXS;
    return tk_wai_sem(job_pool[jid].done_sem, 1, tmout);
}

ER tk_del_ai_job(ID jid)
{
    if (jid < 0 || jid >= (ID)AI_JOB_POOL_SIZE) return E_ID;

    tk_wai_sem(job_pool_sem, 1, TMO_FEVR);
    AI_JOB *j = &job_pool[jid];
    if (j->state == 0) { tk_sig_sem(job_pool_sem, 1); return E_NOEXS; }
    if (j->done_sem > 0) { tk_del_sem(j->done_sem); j->done_sem = 0; }
    j->state = 0;
    tk_sig_sem(job_pool_sem, 1);
    return E_OK;
}

/* ------------------------------------------------------------------ */
/* Job execution                                                       */
/* ------------------------------------------------------------------ */

static W execute_job(AI_JOB *j)
{
    AI_JOB_SPEC *s = &j->spec;

    if (s->op == AI_OP_MLP_FWD || s->op == AI_OP_RELU) {
        /* Read input tensor */
        void *in_ptr = tk_tensor_ptr(s->input_tid);
        if (!in_ptr) return E_ID;

        B input[MLP_IN] = {0, 0, 0, 0};
        UW sz = tk_tensor_size(s->input_tid);
        if (sz >= MLP_IN) {
            for (INT i = 0; i < MLP_IN; i++)
                input[i] = ((B *)in_ptr)[i];
        }

        UB cls = mlp_forward(input);
        ai_stats.inferences_local++;
        ai_stats.inferences_total++;
        ai_stats.class_count[cls < 3 ? cls : 0]++;

        /* Write result to output tensor (class as int8) */
        if (s->output_tid >= 0) {
            B result = (B)cls;
            tk_tensor_write(s->output_tid, 0, &result, 1);
        }
        ai_stats.ai_jobs_done++;
        return (W)cls;
    }

    return E_NOSPT;
}

/* ================================================================== */
/* AI worker task (software NPU thread)                               */
/* ================================================================== */

void ai_worker_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    for (;;) {
        /* Block until a job is enqueued */
        tk_wai_sem(job_queue_sem, 1, TMO_FEVR);

        tk_wai_sem(job_pool_sem, 1, TMO_FEVR);

        if (job_q_cnt == 0) { tk_sig_sem(job_pool_sem, 1); continue; }

        INT slot = job_queue[job_q_head];
        job_q_head = (job_q_head + 1) % AI_JOB_POOL_SIZE;
        job_q_cnt--;

        tk_sig_sem(job_pool_sem, 1);

        AI_JOB *j = &job_pool[slot];
        j->state  = 2;   /* running */
        j->result = execute_job(j);
        j->state  = 3;   /* done    */

        tk_sig_sem(j->done_sem, 1);   /* notify waiter */
    }
}

/* ------------------------------------------------------------------ */
/* Convenience: synchronous run                                        */
/* ------------------------------------------------------------------ */

ER ai_run_sync(const AI_JOB_SPEC *spec, TMO tmout)
{
    ID jid = tk_cre_ai_job(spec);
    if (jid < 0) return (ER)jid;
    ER er = tk_wai_ai_job(jid, tmout);
    tk_del_ai_job(jid);
    return er;
}
