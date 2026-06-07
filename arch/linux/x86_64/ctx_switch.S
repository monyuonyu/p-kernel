/* arch/linux/x86_64/ctx_switch.S
 *
 * x86_64 Linux user-mode context switch. System V AMD64 ABI says these
 * registers are callee-saved and must survive a function call:
 *
 *     rbx, rbp, r12, r13, r14, r15, rsp
 *
 * Caller-saved registers (rax, rcx, rdx, rsi, rdi, r8-r11, including the
 * argument regs we use here) were already saved by the C compiler at
 * the arch_ctx_switch() call site, so we deliberately skip them.
 *
 * Struct layout — see arch/linux/include/arch_ctx.h:
 *   +0x00 rbx   +0x10 r12   +0x20 r14   +0x30 rsp
 *   +0x08 rbp   +0x18 r13   +0x28 r15
 *
 * Calling convention (System V AMD64):
 *   %rdi = arch_ctx_t *prev   (where to save the live regs)
 *   %rsi = arch_ctx_t *next   (where to load the regs to resume into)
 */

    .text
    .global arch_ctx_switch
    .type   arch_ctx_switch, @function
    .balign 16

arch_ctx_switch:
    /* --- save live callee-saved regs into *prev (%rdi) ------------- */
    movq    %rbx,  0(%rdi)
    movq    %rbp,  8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    movq    %rsp, 48(%rdi)

    /* --- restore from *next (%rsi) --------------------------------- */
    movq     0(%rsi), %rbx
    movq     8(%rsi), %rbp
    movq    16(%rsi), %r12
    movq    24(%rsi), %r13
    movq    32(%rsi), %r14
    movq    40(%rsi), %r15
    movq    48(%rsi), %rsp

    /* ret pops the top-of-stack qword into %rip. For a paused task
     * that is the return address right after its own arch_ctx_switch
     * call. For a freshly created task it is the entry function,
     * placed at the bottom of its stack by init_ctx. */
    ret

    .size arch_ctx_switch, . - arch_ctx_switch

/* Tell the linker our assembly does not need an executable stack —
 * silences the GNU linker warning that otherwise appears on Linux. */
    .section .note.GNU-stack, "", @progbits
