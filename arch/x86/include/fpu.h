/*
 *  fpu.h (x86)
 *  Per-task x87/MMX/SSE context save — debt wave (RING3-C follow-up)
 */
#pragma once
#include "kernel.h"

/*
 * fpu_init()
 *   Arm eager per-task FXSAVE/FXRSTOR in the dispatcher:
 *   sets CR4.OSFXSR, clears CR0.TS/EM, captures a clean (fninit)
 *   512-byte template for first-run tasks, and enables the
 *   knl_fpu_save_ctx / knl_fpu_restore_ctx hooks called from
 *   cpu_support.S.  Call once from usermain() before any FP-using
 *   task is created (the hooks are no-ops until then).
 */
void fpu_init(void);

/*
 * fpu_task_reset(tid)
 *   Invalidate the saved FPU image for a task ID so a REUSED tid
 *   starts from the clean template instead of a dead task's state.
 *   Called from user_proc_teardown().
 */
void fpu_task_reset(ID tid);
