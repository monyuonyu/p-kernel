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

/* Shell dispatcher for the line "selfc ..." (full line passed in):
 *   selfc demo            — compile + run the built-in demo source
 *   selfc save <name>     — save the demo source as p-fs object <name>
 *                           (replicates to region peers like any block)
 *   selfc run <name>      — read C source from p-fs object <name>,
 *                           compile it here, run it as a task
 *   selfc ls              — list compiled units
 */
void selfc_cmd(const UB *line, INT n);
