/* arch/linux/x86_64/trampoline.S
 *
 * task_trampoline — the function the dispatcher resumes into the very
 * first time a fresh context is scheduled. It exists to dodge the
 * "entry returning loops back into entry" hazard the bare PoC has:
 * by routing the initial PC through this trampoline, when the entry
 * function eventually returns, control falls through to task_exit
 * instead of recursing into entry.
 *
 * Setup contract (filled in by arch_init_full_ctx):
 *   %r12 = entry function pointer
 *   %r13 = argument to pass as entry's first parameter
 *   %rsp = 16-aligned stack top
 *   %rip = task_trampoline
 *
 * We use r12 and r13 instead of the standard arg registers (rdi/rsi)
 * because those are caller-saved — the very first signal-driven entry
 * into this code path may see them clobbered by the signal frame setup.
 * r12 and r13 are callee-saved so they survive intact.
 *
 * System V AMD64 ABI says rsp must be ≡ 8 (mod 16) at function entry.
 * We arrange that: arch_init_full_ctx sets rsp to a 16-aligned address
 * — the first instruction below pushes 8 bytes (the alignment slot)
 * before any `call`, so by the time we `call *%r12` rsp ≡ 0 (mod 16),
 * matching the pre-`call` ABI condition.
 */

    .text
    .global task_trampoline
    .type   task_trampoline, @function
    .balign 16

task_trampoline:
    /* Bring rsp to ≡ 0 (mod 16) before `call`. We arrived here via
     * the dispatcher's `ret`, so rsp is currently ≡ 8 (mod 16). Push
     * a dummy 8 bytes to flip it. (Using `sub $8, %rsp` would be the
     * same; push is one byte shorter.) */
    pushq   $0

    movq    %r13, %rdi       /* arg → first parameter */
    call    *%r12            /* entry(arg) */

    /* If entry returns, fall through to task_exit. Adjust the push
     * we made above (mostly cosmetic — task_exit doesn't read rsp). */
    addq    $8, %rsp
    call    task_exit

    /* task_exit is declared noreturn but be defensive. */
    ud2

    .size task_trampoline, . - task_trampoline

    .section .note.GNU-stack, "", @progbits
