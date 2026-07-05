/*
 *  arch/windows/x86_64/selfc_stub.c
 *
 *  Self-compile (selfc) is DISABLED on the native Windows port for P1 —
 *  the Linux/Android reasoning applies: no gcc/libtcc-at-runtime and no
 *  fork()/exec() germ-supervisor model on Windows (like Bionic). This TU
 *  provides honest stubs for the three selfc entry points the rest of the
 *  build references (usermain.c: selfc_proc_task, selfc_cmd;
 *  genome.c: selfc_compile_and_run), instead of compiling the POSIX-heavy
 *  arch/linux/selfc.c / selfc_proc.c.
 *
 *  Re-enabling selfc on Windows (a libtcc backend or a CreateProcess-based
 *  germ) is P2/P3 work.
 */

#include "kernel.h"

/* selfc_proc.h forward-declares these; keep signatures identical. */

/* The germ-supervisor task: on Windows it simply parks (no germs to reap). */
void selfc_proc_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for ( ;; ) {
        tk_dly_tsk(1000);
    }
}

/* Shell "selfc ..." dispatcher: honest not-supported message. */
void selfc_cmd(const UB *line, INT n)
{
    (void)line; (void)n;
    tm_putstring((UB *)"[win] selfc (self-compile) disabled in v1\n");
}

/* In-memory compile+run (genome self-modification): unsupported on Windows. */
ER selfc_compile_and_run(const char *src, const char *entry_sym,
                         const char *what)
{
    (void)src; (void)entry_sym; (void)what;
    return E_NOSPT;
}
