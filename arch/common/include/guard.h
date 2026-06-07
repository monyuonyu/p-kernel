/*
 *  guard.h
 *  Task fault isolation + auto-respawn supervisor (wave 7).
 *
 *  Answers PR #4 hole (4): heal.c can only restart ring-3 ELF
 *  daemons, but the flagship Transformer runs ring-0 — one bug used
 *  to kill the whole node. guard.c turns that into TASK-level fault
 *  isolation: a fault in a guarded task kills only that task; the
 *  supervisor respawns it and calls its recover_fn (for dtr: reload
 *  the trained weights from the p-fs object "dtr/weights").
 *
 *  Division of labour:
 *    - guard.c (this layer, arch/common): guard table, the killer
 *      that runs in task context and tk_exd_tsk()s the dying task,
 *      the supervisor task that respawns with backoff, the `guard`
 *      shell verb. Pure T-Kernel API — compiles on every target.
 *    - arch fault capture (arch/linux/{aarch64,x86_64}/fault.c):
 *      SIGSEGV/SIGBUS/SIGFPE handlers on the sigaltstack that ask
 *      guard_fault_isolate() whether the faulting task is guarded
 *      and, if so, rewrite ONLY the named PC/SP registers of the
 *      signal frame so sigreturn lands in guard_task_killer().
 *      A bare-metal port hooks the same two functions from its
 *      data-abort / #PF vector instead (not wired yet).
 *
 *  What is NOT recoverable (honest list): faults in unguarded tasks
 *  (shell, idle, kernel housekeeping), faults inside an IRQ-disabled
 *  or dispatch-disabled critical section, faults in handler context,
 *  and a second fault while a kill is already in flight. All of
 *  those abort the node — kernel state can no longer be trusted.
 */

#pragma once
#include "kernel.h"

#define GUARD_MAX          8     /* guarded-task table size            */
#define GUARD_NAME_LEN     16
#define GUARD_MAX_DEATHS   5     /* respawn cap per task               */
#define GUARD_BACKOFF_MS   200   /* base backoff; doubles per death    */
#define GUARD_POLL_MS      200   /* supervisor poll interval           */

typedef void (*GUARD_RECOVER_FN)(void);

/* guard entry states (guard_print shows them) */
#define GUARD_ST_FREE      0
#define GUARD_ST_RUNNING   1
#define GUARD_ST_DEAD      2     /* fault captured, awaiting respawn   */
#define GUARD_ST_GIVEN_UP  3     /* exceeded GUARD_MAX_DEATHS          */

/* Create the supervisor task. Call once at boot, before any
 * guard_register. */
void guard_init(void);

/* Register a task under guard: creates AND starts it. recover_fn
 * (may be NULL) is called by the supervisor after each respawn,
 * before the new incarnation starts. Returns the task ID or a
 * negative T-Kernel error / E_LIMIT when the table is full. */
W guard_register(const char *name, FP entry, W stack_size, W priority,
                 GUARD_RECOVER_FN recover_fn);

/* `guard` shell verb — table dump (state, deaths, last fault,
 * last recovery time). */
void guard_print(void);

/* ---- fault-capture hooks (arch side calls these) ------------------ */
/* Plain C types so arch/linux fault.c can declare them without
 * pulling T-Kernel headers into a POSIX signal-handler file. */

/* Called from the arch fault handler (signal/abort context!).
 * Decides whether the currently running task can be isolated.
 * Returns the emergency stack pointer to resume on (16-aligned,
 * grows down) or NULL when the fault is not recoverable.  Only
 * reads/writes guard-module state — no kernel calls. */
void *guard_fault_isolate(int sig, unsigned long pc, unsigned long addr);

/* The arch handler points the faulting task's PC here (SP at the
 * value guard_fault_isolate returned). Runs in ordinary task context
 * on the emergency stack: logs the fault, marks the entry DEAD,
 * wakes the supervisor and tk_exd_tsk()s. Never returns. */
void guard_task_killer(void);
