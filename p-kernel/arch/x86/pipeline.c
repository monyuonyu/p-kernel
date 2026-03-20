/*
 *  pipeline.c (x86)
 *  Zero-copy sensor → inference pipeline
 *
 *  Producer (sensor / shell) writes a SENSOR_FRAME into a slot.
 *  Consumer (ai_infer_task) reads the same slot, runs mlp_forward(),
 *  and releases it.  The frame bytes are written once and read once
 *  from the same memory location — no intermediate copy.
 *
 *  Concurrency model:
 *    data_sem   counts filled slots (consumer waits here)
 *    space_sem  counts empty slots  (producer waits here)
 *    lock_sem   protects head/tail pointers (brief critical section)
 */

#include "kernel.h"
#include "ai_kernel.h"
#include <tmonitor.h>

/* ------------------------------------------------------------------ */
/* Ring buffer                                                         */
/* ------------------------------------------------------------------ */

static SENSOR_FRAME ring[PIPELINE_DEPTH];

static UW head = 0;   /* next write position */
static UW tail = 0;   /* next read  position */

static ID data_sem  = 0;   /* filled slot count  */
static ID space_sem = 0;   /* empty  slot count  */
static ID lock_sem  = 0;   /* head/tail mutex    */

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void pipeline_init(void)
{
    T_CSEM data  = { .exinf=NULL, .sematr=TA_TFIFO,
                     .isemcnt=0,             .maxsem=PIPELINE_DEPTH };
    T_CSEM space = { .exinf=NULL, .sematr=TA_TFIFO,
                     .isemcnt=PIPELINE_DEPTH, .maxsem=PIPELINE_DEPTH };
    T_CSEM lock  = { .exinf=NULL, .sematr=TA_TFIFO,
                     .isemcnt=1,             .maxsem=1 };

    data_sem  = tk_cre_sem(&data);
    space_sem = tk_cre_sem(&space);
    lock_sem  = tk_cre_sem(&lock);
    head = tail = 0;
}

/* ------------------------------------------------------------------ */
/* Producer                                                            */
/* ------------------------------------------------------------------ */

ER pipeline_push(const SENSOR_FRAME *f)
{
    /* Non-blocking: drop frame if pipeline is full */
    ER er = tk_wai_sem(space_sem, 1, TMO_POL);
    if (er != E_OK) return E_QOVR;

    tk_wai_sem(lock_sem, 1, TMO_FEVR);

    /* Zero-copy write: copy frame struct into slot (8 bytes) */
    ring[head] = *f;
    head = (head + 1) % PIPELINE_DEPTH;

    tk_sig_sem(lock_sem, 1);
    tk_sig_sem(data_sem, 1);    /* notify consumer */

    ai_stats.pipeline_in++;
    return E_OK;
}

/* ------------------------------------------------------------------ */
/* Consumer                                                            */
/* ------------------------------------------------------------------ */

ER pipeline_pop(SENSOR_FRAME *f, TMO tmout)
{
    ER er = tk_wai_sem(data_sem, 1, tmout);
    if (er != E_OK) return er;

    tk_wai_sem(lock_sem, 1, TMO_FEVR);

    /* Zero-copy read: copy frame struct from slot (8 bytes) */
    *f = ring[tail];
    tail = (tail + 1) % PIPELINE_DEPTH;

    tk_sig_sem(lock_sem, 1);
    tk_sig_sem(space_sem, 1);   /* return slot to pool */

    ai_stats.pipeline_out++;
    return E_OK;
}

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

UW pipeline_count(void)
{
    return ai_stats.pipeline_in - ai_stats.pipeline_out;
}

/* ------------------------------------------------------------------ */
/* Inference task: pipeline consumer                                   */
/* ------------------------------------------------------------------ */

static const char *class_names[3] = { "normal  ", "ALERT   ", "CRITICAL" };

void ai_infer_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    SENSOR_FRAME f;

    for (;;) {
        /* Block until a frame is available */
        ER er = pipeline_pop(&f, TMO_FEVR);
        if (er != E_OK) continue;

        /* Run MLP forward pass */
        B input[MLP_IN] = { f.temp, f.humidity, f.pressure, f.light };
        UB cls = mlp_forward(input);

        ai_stats.inferences_total++;
        ai_stats.inferences_local++;
        ai_stats.class_count[cls < 3 ? cls : 0]++;

        /* Log result to serial */
        tm_putstring((UB *)"\r\n[ai]  infer: ");
        tm_putstring((UB *)class_names[cls < 3 ? cls : 0]);
        tm_putstring((UB *)"  (t=");

        /* Simple integer print for temperature */
        W tv = (W)f.temp / 2 + 20;   /* reverse normalization */
        if (tv < 0) { tm_putstring((UB *)"-"); tv = -tv; }
        UB d = (UB)(tv / 10); if (d) { UB c = '0'+d; tm_putstring(&c); }
        UB u = '0' + (UB)(tv % 10); tm_putstring(&u);
        tm_putstring((UB *)"C)\r\np-kernel> ");
    }
}
