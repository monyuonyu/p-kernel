/*
 *  arch/linux/include/selfc.h — self-compilation, first milestone.
 *
 *  The running kernel compiles C source INSIDE its own process via
 *  libtcc (TCC_OUTPUT_MEMORY) and starts the result as a new T-Kernel
 *  task. No external compiler process, no files on disk, no mothership.
 *
 *  Hosted (arch/linux) only for now: bare-metal targets have no libtcc
 *  yet. Builds everywhere — when the toolchain has no libtcc the
 *  Makefile omits -DHAVE_LIBTCC and this module degrades to a stub
 *  that explains itself.
 *
 *  Security note (honest): compiled code runs with full kernel
 *  privileges, same address space, same rings. That is the design —
 *  this is the minimal organ of self-evolution — but there is NO
 *  verification / sandbox / signature layer yet. See
 *  docs/architecture/self-compile.md.
 */
#pragma once
#include "kernel.h"

/* Compile `src` in-memory and start `entry_sym` (signature:
 * void entry(void)) as a new T-Kernel task. `what` is a short label
 * used in log lines and the unit registry. Returns the task ID
 * (>0) on success or a negative ER code:
 *   E_NOSPT  — built without libtcc
 *   E_PAR    — compile / link / missing-entry error (details printed)
 *   E_LIMIT  — unit registry full
 * The compiled code's memory stays alive for the life of the kernel
 * process (the task may run forever; we never tcc_delete a unit). */
ER selfc_compile_and_run(const char *src, const char *entry_sym,
                         const char *what);

/* selfc-ring3 §6 reused path: read unit/<name>@seq source from p-fs,
 * compile it ISOLATED (proxy API table), register the RWX image in the
 * unit table, and return the resolved selfc_main entry (or NULL). The
 * germ supervisor (arch/linux/selfc_proc.c) forks AFTER this resolution
 * so the child inherits the compiled image via COW. */
void *selfc_resolve_unit(const char *name, U4 seq);

/* Shell dispatcher for the line "selfc ..." (full line passed in):
 *   selfc demo            — compile + run the built-in demo (LEGACY in-task)
 *   selfc save <name>     — save the demo source as p-fs object unit/<name>
 *   selfc adopt <name>    — explicitly accept a unit for germination (§3)
 *   selfc run <name>      — germinate unit/<name> in a germ process (the
 *                           crash boundary; the v1 DEFAULT, selfc-ring3 §2.1)
 *   selfc test            — the [selfc-isolated]/[selfc-rollback]/
 *                           [selfc-lineage] acceptance gates (§5)
 *   selfc ls              — list compiled units + germ reap count
 */
void selfc_cmd(const UB *line, INT n);
