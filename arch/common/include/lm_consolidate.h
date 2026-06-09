/*
 *  lm_consolidate.h — living-mind first slice: DMN sleep-consolidation.
 *
 *  Spec: docs/architecture/living-mind.md Part II. A rest-time ("sleep")
 *  consolidation that REPLAYS stored engrams and DISTILLS them into the
 *  dtr weights via the G22 gossip merge, so the mind learns a STREAM of
 *  tasks WITHOUT catastrophic forgetting, decentralized, surviving node
 *  death + rejoin.
 *
 *  ANTI-FORK (living-mind.md §II.7): this module owns NO new math. It
 *  drives the SAME shared kernels every other brain uses --
 *    - dtr.h:  dtr_train_batch / dtr_eval_batch / dtr_reinit_weights /
 *              dtr_weights_get / dtr_weights_set / dtr_grad_check
 *    - gossip_learn.h: gl_merge (the no-central consolidation merge)
 *    - pfs_dag.h: pfs_dag_save / pfs_dag_read (durable engram store)
 *  -- swapping weight sets in/out to simulate N nodes in-process exactly
 *  the way gossip_learn.c does. The DMN organ (dmn.c) is EXTENDED, not
 *  forked: dmn_idle_work() calls lm_consolidate_idle_round().
 *
 *  arch/common discipline: no host libc, fixed-width types, static (not
 *  task-stack) buffers, output via sio_send_frame, _Static_assert sizes.
 */

#pragma once
#include "kernel.h"
#include "dtr.h"      /* DTR_SEQ_LEN */

/* ------------------------------------------------------------------ */
/* one engram -- the "hippocampal" episode (living-mind.md II.3).       */
/* 8 bytes, the size class of DTR_LOG_ENTRY; many fit one p-fs block.   */
/* ------------------------------------------------------------------ */

typedef struct {
    B   input[DTR_SEQ_LEN];   /* the episode's sensor input (int8[4])    */
    UB  label;                /* its target class (post task permutation)*/
    UB  task_id;              /* which task in the stream produced it     */
    UB  salience;             /* replay priority (uniform here; see .c)   */
    UB  _pad;
} LM_ENGRAM;                  /* 8 bytes */

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* The falsifiable acceptance suite (living-mind.md II.5). Emits the
 * printed numbers then a canonical "[dmn-*] PASS/FAIL" line for each of:
 *   [dmn-forgetting] [dmn-consolidated] [dmn-distributed]
 *   [dmn-survive]    [dmn-gradcheck]
 * Wired to CI via the `dmn test` shell verb. */
void lm_test(void);

/* Live DMN idle hook (called from dmn.c dmn_idle_work, alongside
 * ga_step). If engrams are pending in the ring it runs ONE bounded
 * replay-consolidation round and returns 1; if the ring is empty it is
 * a no-op and returns 0. Manages dtr_ga_busy around the weight update. */
INT  lm_consolidate_idle_round(void);

/* TRUE if the engram ring currently holds replayable episodes. */
BOOL lm_engrams_pending(void);
