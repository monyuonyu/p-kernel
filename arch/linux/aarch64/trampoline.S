/* arch/linux/aarch64/trampoline.S
 *
 * Task entry trampoline for the Linux userspace port.
 *
 * Every task's initial mcontext is set up so that the first time the
 * scheduler picks it (via mcontext manipulation on signal return), the
 * CPU lands here with:
 *
 *     x19 = entry function pointer
 *     x20 = argument to pass to entry
 *     sp  = aligned stack top
 *     pc  = task_trampoline
 *
 * The trampoline loads the argument into x0 (AAPCS64 first-argument
 * register), calls the entry function, and on its return calls
 * task_exit. If task_exit ever returns (it shouldn't), we hang in a
 * busy loop so the failure is obvious rather than silently executing
 * whatever happens to be in memory.
 *
 * Keeping the trampoline in pure assembly means the entry's stack
 * frame layout is exactly what the C ABI expects on function entry —
 * no synthetic prologue, no "return into a half-built frame" hazard.
 */

    .text
    .global task_trampoline
    .type   task_trampoline, %function
    .balign 4

task_trampoline:
    mov     x0, x20                 /* arg -> first-arg register */
    blr     x19                     /* call entry(arg)           */
    bl      task_exit               /* if entry returns, exit    */
1:  b       1b                      /* defensive hang            */

    .size task_trampoline, . - task_trampoline

    .section .note.GNU-stack, "", %progbits
