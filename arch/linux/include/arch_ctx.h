/* arch/linux/include/arch_ctx.h
 *
 * Userspace-resident task context for the Linux HAL port.
 *
 * The struct definition is per host arch — outer (kernel/common) code
 * only sees an opaque `arch_ctx_t *`. The host-arch asm in
 * arch/linux/<host>/ctx_switch.S knows the layout below by offset.
 *
 * AArch64 layout matches AAPCS64 callee-saved set: x19-x28, x29 (fp),
 * x30 (lr), and the stack pointer. 13 * 8 = 104 bytes per task.
 *
 * x86_64 layout matches System V AMD64 callee-saved set: rbx, rbp,
 * r12-r15, and rsp. 7 * 8 = 56 bytes per task.
 *
 * Caller-saved registers are already preserved by the C compiler at
 * each call site of arch_ctx_switch(), so we deliberately do not
 * save them — that would be wasted work.
 */
#ifndef _PKERNEL_ARCH_LINUX_ARCH_CTX_H
#define _PKERNEL_ARCH_LINUX_ARCH_CTX_H

#if defined(__aarch64__)

typedef struct arch_ctx {
    unsigned long x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    unsigned long x29;   /* fp                                          */
    unsigned long x30;   /* lr — where the final `ret` in ctx_switch goes */
    unsigned long sp;
} arch_ctx_t;

#elif defined(__x86_64__)

typedef struct arch_ctx {
    unsigned long rbx, rbp, r12, r13, r14, r15;
    unsigned long rsp;   /* on return from ctx_switch, ret pops [rsp]    */
} arch_ctx_t;

#else
#error "arch/linux not yet ported to this host arch"
#endif

/*
 * Save the live callee-saved register set into *prev, then restore
 * from *next and resume next's execution. On the very first switch
 * into a freshly initialised next, control transfers to next's entry
 * function (set up by the per-arch initial-context routine).
 */
extern void arch_ctx_switch(arch_ctx_t *prev, arch_ctx_t *next);

#endif /* _PKERNEL_ARCH_LINUX_ARCH_CTX_H */
