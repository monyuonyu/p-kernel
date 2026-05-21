/* arch/linux/aarch64/ctx_switch.S
 *
 * AArch64 Linux user-mode context switch — the smallest piece of code
 * that makes the whole arch/linux port possible. AAPCS64 says these
 * registers are callee-saved and must survive a function call:
 *
 *     x19-x28, x29 (fp), x30 (lr), sp
 *
 * Caller-saved registers (x0-x18, including the argument regs we use
 * here) were already saved by the C compiler at the arch_ctx_switch()
 * call site, so we deliberately skip them.
 *
 * Struct layout — see arch/linux/include/arch_ctx.h:
 *   +0x00 x19   +0x10 x21   +0x20 x23   +0x30 x25   +0x40 x27   +0x50 x29 (fp)
 *   +0x08 x20   +0x18 x22   +0x28 x24   +0x38 x26   +0x48 x28   +0x58 x30 (lr)
 *   +0x60 sp
 *
 * Calling convention (AAPCS64):
 *   x0 = arch_ctx_t *prev   (where to save the live regs)
 *   x1 = arch_ctx_t *next   (where to load the regs to resume into)
 */

    .text
    .global arch_ctx_switch
    .type   arch_ctx_switch, %function
    .balign 4

arch_ctx_switch:
    /* --- save live callee-saved regs into *prev (x0) ------------- */
    stp     x19, x20, [x0, #0x00]
    stp     x21, x22, [x0, #0x10]
    stp     x23, x24, [x0, #0x20]
    stp     x25, x26, [x0, #0x30]
    stp     x27, x28, [x0, #0x40]
    stp     x29, x30, [x0, #0x50]
    mov     x9, sp                  /* sp can't go straight to memory */
    str     x9,       [x0, #0x60]

    /* --- restore from *next (x1) --------------------------------- */
    ldp     x19, x20, [x1, #0x00]
    ldp     x21, x22, [x1, #0x10]
    ldp     x23, x24, [x1, #0x20]
    ldp     x25, x26, [x1, #0x30]
    ldp     x27, x28, [x1, #0x40]
    ldp     x29, x30, [x1, #0x50]
    ldr     x9,       [x1, #0x60]
    mov     sp, x9

    /* ret jumps to x30 (lr). For a paused task this is the address
     * right after its own arch_ctx_switch call. For a freshly created
     * task this is its entry function, placed there by init_ctx. */
    ret

    .size arch_ctx_switch, . - arch_ctx_switch

/* Tell the linker our assembly does not need an executable stack —
 * silences the GNU linker warning that otherwise appears on Linux. */
    .section .note.GNU-stack, "", %progbits
