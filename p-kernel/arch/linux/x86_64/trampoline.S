/* arch/linux/x86_64/trampoline.S
 *
 * task_trampoline — the function the dispatcher resumes into the very
 * first time a fresh context is scheduled. It exists to dodge the
 * "entry returning loops back into entry" hazard: by routing the
 * initial PC through this trampoline, when the entry function
 * eventually returns, control falls through to task_exit instead of
 * recursing into entry.
 *
 * Setup contract (filled in by arch_init_full_ctx and by
 * knl_setup_context):
 *   %r12 = entry function pointer
 *   %r13 = argument to pass as entry's first parameter
 *   %rsp = 16-aligned stack top
 *
 * We use r12 and r13 instead of the standard arg registers (rdi/rsi)
 * because those are caller-saved — the very first signal-driven entry
 * into this code path may see them clobbered by the signal frame setup.
 * r12 and r13 are callee-saved so they survive intact through the
 * sigreturn-and-jump-here dance.
 *
 * Stack-alignment note: System V AMD64 ABI requires rsp to be a
 * multiple of 16 immediately before a `call` instruction (the call
 * pushes 8 bytes so the callee sees (rsp + 8) % 16 == 0 at entry).
 *
 * The dispatcher arranges that rsp == stack_top at our entry (both the
 * sigreturn-style install and the dormant-frame restore land here with
 * a 16-aligned rsp). We make no further pushes before `call`, so the
 * call site has rsp % 16 == 0 — ABI compliant without any local fix-up.
 */

    .text
    .global task_trampoline
    .type   task_trampoline, @function
    .balign 16

task_trampoline:
    movq    %r13, %rdi       /* arg → first parameter */
    call    *%r12            /* entry(arg) */

    /* If entry returns, fall through to task_exit. rsp is back to its
     * value at trampoline entry (16-aligned), which is also the right
     * alignment for the next `call`. */
    call    task_exit

    /* task_exit is declared noreturn; be defensive in case it wasn't. */
    ud2

    .size task_trampoline, . - task_trampoline

    .section .note.GNU-stack, "", @progbits
