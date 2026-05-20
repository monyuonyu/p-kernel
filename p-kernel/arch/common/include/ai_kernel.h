/*
 *  ai_kernel.h (x86)
 *  AI-native kernel primitives — the OS for the AI era
 *
 *  Three kernel pillars:
 *    Tensor   — kernel-managed N-dimensional array, 32-byte aligned, zero-copy
 *    AI Job   — hardware-agnostic inference unit (software NPU, upgrades to NPU/GPU)
 *    Pipeline — lock-free producer/consumer: sensor → inference, no memcpy
 *
 *  Distributed extensions (via DRPC):
 *    dtk_infer()        — route inference to any node transparently
 *    dtk_fl_aggregate() — FedAvg gradient aggregation across cluster
 *
 *  Design principles:
 *    - Data never moves; pointers move (zero-copy end-to-end)
 *    - Compute is a first-class kernel resource (like CPU tasks)
 *    - Model weights are protected kernel objects
 *    - Distributed = same API as local; kernel routes transparently
 */

#pragma once
#include "kernel.h"

/* ================================================================== */
/* Tensor — kernel-managed N-dimensional array                         */
/* ================================================================== */

#define TENSOR_DTYPE_I8    0    /* int8  (quantized, 1 byte/elem)     */
#define TENSOR_DTYPE_I32   1    /* int32 (accumulator, 4 bytes/elem)  */
#define TENSOR_DTYPE_F32   2    /* float (software FP, 4 bytes/elem)  */

#define TENSOR_LAYOUT_FLAT 0    /* row-major contiguous               */
#define TENSOR_LAYOUT_NCHW 1    /* batch × channel × H × W           */
#define TENSOR_LAYOUT_NHWC 2    /* batch × H × W × channel           */

#define TENSOR_MAX_DIM        4
#define TENSOR_POOL_SIZE     16    /* max concurrent live tensors      */
#define TENSOR_DATA_POOL_KB  16    /* shared data pool (16 KB)        */
#define TENSOR_ALIGN         32    /* SIMD alignment boundary         */

typedef struct {
    UB   dtype;                        /* TENSOR_DTYPE_*              */
    UB   layout;                       /* TENSOR_LAYOUT_*             */
    UB   ndim;                         /* number of dimensions (1-4)  */
    UB   in_use;                       /* pool slot occupied          */
    UW   shape[TENSOR_MAX_DIM];        /* size of each dimension      */
    UW   nbytes;                       /* total byte count            */
    void *data;                        /* pointer into data pool      */
} TENSOR_DESC;

/* tensor_init  — call once at boot before any tk_cre_tensor()        */
void  tensor_init(void);

/* tk_cre_tensor — allocate a new tensor; returns slot ID (≥0) or <0 */
ID    tk_cre_tensor(UB ndim, const UW shape[], UB dtype, UB layout);

/* tk_del_tensor — release slot and (if last) reclaim data pool bytes */
ER    tk_del_tensor(ID tid);

/* zero-copy accessors */
void *tk_tensor_ptr(ID tid);        /* direct pointer to data        */
UW    tk_tensor_size(ID tid);       /* byte count                    */
ER    tk_tensor_write(ID tid, UW off, const void *src, UW len);
ER    tk_tensor_read (ID tid, UW off,       void *dst, UW len);
ER    tk_tensor_zero (ID tid);      /* memset data to 0              */

/* ================================================================== */
/* AI Job — software NPU (upgradable to real accelerator)             */
/* ================================================================== */

#define AI_OP_MATMUL   0x01    /* C[M,N] = A[M,K] × B[K,N]         */
#define AI_OP_RELU     0x02    /* in-place: x = max(0, x)           */
#define AI_OP_SOFTMAX  0x03    /* in-place: x = softmax(x)          */
#define AI_OP_LINEAR   0x04    /* out = W·in + bias                 */
#define AI_OP_MLP_FWD  0x10    /* full MLP forward pass             */

#define MODEL_SENSOR_CLS  0x0001   /* 4→8→8→3 sensor classifier     */

#define AI_JOB_POOL_SIZE  8
#define AI_WORKER_PRI     6        /* lower than drpc(5), net(3)    */
#define AI_WORKER_STACK   4096

typedef struct {
    UB   op;            /* AI_OP_*                                   */
    UH   model_id;      /* MODEL_* (for AI_OP_MLP_FWD)              */
    ID   input_tid;     /* input  tensor slot ID                     */
    ID   output_tid;    /* output tensor slot ID                     */
    W    param[2];      /* op-specific (M, K, N for matmul, etc.)   */
} AI_JOB_SPEC;

typedef struct {
    AI_JOB_SPEC spec;
    W    result;        /* E_OK or negative error code               */
    ID   done_sem;      /* signalled on completion                   */
    UB   state;         /* 0=free 1=queued 2=running 3=done         */
} AI_JOB;

void ai_job_init(void);
void ai_worker_task(INT stacd, void *exinf);

ID   tk_cre_ai_job(const AI_JOB_SPEC *spec);
ER   tk_wai_ai_job(ID jid, TMO tmout);      /* wait for completion   */
ER   tk_del_ai_job(ID jid);

/* Convenience: submit + wait (blocking infer on worker task) */
ER   ai_run_sync(const AI_JOB_SPEC *spec, TMO tmout);

/* ================================================================== */
/* MLP neural network — 4→8→8→3 sensor classifier                    */
/* ================================================================== */

#define MLP_IN   4     /* [temp, humidity, pressure, light]  int8    */
#define MLP_H1   8     /* hidden layer 1                            */
#define MLP_H2   8     /* hidden layer 2                            */
#define MLP_OUT  3     /* [normal, alert, critical]                 */

/* MLP_WEIGHT_BYTES: total bytes for all weights + biases             */
#define MLP_WEIGHT_BYTES \
    ((MLP_IN*MLP_H1 + MLP_H1) + (MLP_H1*MLP_H2 + MLP_H2) + (MLP_H2*MLP_OUT + MLP_OUT))
/* = (32+8) + (64+8) + (24+3) = 139 bytes                           */

/* Forward pass: input int8 Q8 → output class [0,1,2]                */
/*   class 0 = normal, 1 = alert, 2 = critical                       */
UB   mlp_forward(const B input[MLP_IN]);

/* Get current model weights (for FL export)                          */
void mlp_get_weights(float *w1, float *b1, float *w2, float *b2,
                     float *w3, float *b3);

/* Apply updated weights (for FL import)                              */
void mlp_set_weights(const float *w1, const float *b1,
                     const float *w2, const float *b2,
                     const float *w3, const float *b3);

/* ================================================================== */
/* Zero-Copy Pipeline — sensor → inference without memcpy             */
/* ================================================================== */

/*
 *  The pipeline holds PIPELINE_DEPTH slots.  The producer (sensor
 *  interrupt / shell) writes a SENSOR_FRAME into the *next free slot*
 *  and commits it.  The consumer (ai_infer_task) reads the oldest
 *  committed slot, runs mlp_forward(), and releases it.
 *
 *  "Zero-copy" means the frame bytes are written once into a slot and
 *  read once from the same location — no intermediate buffer.
 */

#define PIPELINE_DEPTH  16

typedef struct {
    B    temp;          /* temperature  int8: (°C - 20) × 2          */
    B    humidity;      /* humidity     int8: (% - 50)  × 2          */
    B    pressure;      /* pressure     int8: (hPa-1013)× 0.5        */
    B    light;         /* light level  int8: (lux-500) / 4          */
    UW   tick;          /* system tick when pushed                    */
} SENSOR_FRAME;

/* Normalisation helpers (raw → int8) — clamp to [-127, 127]         */
static inline B sensor_norm_temp(W t)
    { W v = (t - 20) * 2; return (B)(v>127?127:v<-127?-127:v); }
static inline B sensor_norm_hum(W h)
    { W v = (h - 50) * 2; return (B)(v>127?127:v<-127?-127:v); }
static inline B sensor_norm_press(W p)
    { W v = (p - 1013) / 2; return (B)(v>127?127:v<-127?-127:v); }
static inline B sensor_norm_light(W l)
    { W v = (l - 500) / 4; return (B)(v>127?127:v<-127?-127:v); }

void pipeline_init(void);
ER   pipeline_push(const SENSOR_FRAME *f);         /* producer       */
ER   pipeline_pop (SENSOR_FRAME *f, TMO tmout);    /* consumer       */
UW   pipeline_count(void);                         /* queued frames  */

/* Inference pipeline task — pops frames, runs MLP, logs result       */
void ai_infer_task(INT stacd, void *exinf);

/* ================================================================== */
/* Distributed Inference — dtk_infer() routes to any node via DRPC   */
/* ================================================================== */

/*
 *  4 int8 sensor values are packed into one W (32 bits) for the
 *  DRPC_PKT payload — no fragmentation needed.
 *
 *    bits 31..24 = temp    int8
 *    bits 23..16 = humidity int8
 *    bits 15.. 8 = pressure int8
 *    bits  7.. 0 = light   int8
 */
#define SENSOR_PACK(t,h,p,l) \
    ( ((W)(B)(t)<<24) | ((W)(UB)(h)<<16) | ((W)(UB)(p)<<8) | (UB)(l) )
#define SENSOR_UNPACK_T(w)  ((B)((W)(w)>>24))
#define SENSOR_UNPACK_H(w)  ((B)((W)(w)>>16))
#define SENSOR_UNPACK_P(w)  ((B)((W)(w)>>8))
#define SENSOR_UNPACK_L(w)  ((B)((W)(w)))

/*
 *  dtk_infer(node_id, packed, &class_out, tmout)
 *    node_id  = 0xFF → run locally; otherwise route to remote node
 *    Returns E_OK and sets *class_out to 0/1/2 on success.
 */
ER   dtk_infer(UB node_id, W sensor_packed, UB *class_out, TMO tmout);

/* ================================================================== */
/* Federated Learning — FedAvg across cluster nodes                   */
/* ================================================================== */

/*
 *  Simplified FedAvg protocol:
 *    1. Each node runs fl_local_train() to compute weight deltas
 *    2. dtk_fl_aggregate() broadcasts deltas via DRPC to node 0
 *    3. Node 0 averages (weighted by n_samples) and broadcasts back
 *    4. All nodes apply the update with fl_apply_update()
 *
 *  Weights are transmitted as float32 (sizeof(float)×MLP_WEIGHT_BYTES/4).
 *  For packets larger than DRPC_PKT payload, a two-step protocol is used:
 *    Step A: DRPC notifies node 0 of incoming weight data
 *    Step B: raw UDP on port 7375 carries the weight payload
 */

#define FL_UDP_PORT    7375    /* separate port for weight transfers  */

/* Simulate one local training step on provided sensor data.          */
/* Computes gradient via finite differences (demo-grade).             */
ER   fl_local_train(const B samples[][MLP_IN], const UB labels[],
                    UW n, float delta_w1[MLP_IN*MLP_H1],
                    float delta_b1[MLP_H1], float delta_w2[MLP_H1*MLP_H2],
                    float delta_b2[MLP_H2], float delta_w3[MLP_H2*MLP_OUT],
                    float delta_b3[MLP_OUT]);

/* FedAvg aggregation: send local delta, receive global update.       */
ER   dtk_fl_aggregate(UB aggregator_node,
                      const float *my_delta, UW my_n_samples, TMO tmout);

/* Apply aggregated weights received from aggregator.                 */
ER   fl_apply_update(const float *new_weights);

/* Print FL round statistics.                                         */
void fl_status(void);

/* ================================================================== */
/* Global AI statistics                                               */
/* ================================================================== */

typedef struct {
    UW  inferences_total;
    UW  inferences_local;
    UW  inferences_remote;
    UW  class_count[3];       /* normal / alert / critical            */
    UW  pipeline_in;
    UW  pipeline_out;
    UW  fl_rounds;
    UW  ai_jobs_done;
} AI_STATS;

extern AI_STATS ai_stats;

void ai_kernel_init(void);    /* call from usermain() after NIC init  */
void ai_stats_print(void);
